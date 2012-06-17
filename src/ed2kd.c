#include <stdint.h>
#include <signal.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>
#include <zlib.h>
#include "ed2kd.h"
#include "util.h"
#include "log.h"
#include "config.h"
#include "db.h"
#include "ed2k_proto.h"
#include "packet_buffer.h"
#include "client.h"
#include "portcheck.h"

struct ed2kd_cfg g_ed2kd_cfg;
struct ed2kd_rt g_ed2kd_rt;

const struct ed2kd_cfg *ed2kd_cfg()
{
    return &g_ed2kd_cfg;
}

struct ed2kd_rt *ed2kd_rt()
{
	return &g_ed2kd_rt;
}

int ed2kd_init()
{
    if ( evutil_secure_rng_init() < 0 ) {
        ED2KD_LOGERR("Failed to seed random number generator");
        return -1;
    }
    memset(&g_ed2kd_cfg, 0, sizeof(g_ed2kd_cfg));
	memset(&g_ed2kd_rt, 0, sizeof(g_ed2kd_rt));

    return 0;
}

static int
process_login_request( struct packet_buffer *pb, struct e_client *client )
{
	uint32_t tag_count;
	
	// user hash 16b
    PB_MEMCPY(pb, client->hash, sizeof client->hash);

    // user id 4b
    PB_SEEK(pb, sizeof(uint32_t));

    // user port 2b
    PB_READ_UINT16(pb, client->port);

    // tag count 4b
    PB_READ_UINT32(pb, tag_count);

    for ( ; tag_count>0; --tag_count ) {
        const struct tag_header *tag_hdr = (struct tag_header*)pb->ptr;
        
		// no new tags allowed here
		PB_CHECK( (tag_hdr->type & 0x80) == 0 );
		
		// only int-based tag names
		PB_CHECK( 1 == tag_hdr->name_len );

        PB_SKIP_TAGHDR(pb, tag_hdr);
        
		switch ( tag_hdr->name ) {
        case TN_NAME:
            PB_CHECK(TT_STRING == tag_hdr->type);
            PB_READ_UINT16(pb, client->nick_len);
			client->nick_len = client->nick_len > MAX_NICK_LEN ? MAX_NICK_LEN : client->nick_len;
            PB_MEMCPY(pb, client->nick, client->nick_len);
            client->nick[client->nick_len] = 0;
            break;

        case TN_PORT:
            PB_CHECK(TT_UINT16 == tag_hdr->type);
            PB_READ_UINT16(pb, client->port);
            break;

        case TN_VERSION: {
            uint32_t ver;
            PB_CHECK(TT_UINT32 == tag_hdr->type);
            PB_READ_UINT32(pb, ver);
            PB_CHECK(EDONKEYVERSION == ver);
            break;
        }

        case TN_SERVER_FLAGS:
            PB_CHECK(TT_UINT32 == tag_hdr->type);
            PB_READ_UINT32(pb, client->server_flags);
            break;

        case TN_EMULE_VERSION:
            PB_CHECK(TT_UINT32 == tag_hdr->type);
            PB_READ_UINT32(pb, client->emule_ver);
            break;

        default:
            PB_CHECK(0);
        }
    }

    if ( client_portcheck_start(client) < 0 ) {
		client_portcheck_failed(client);
		return -1;
	}

    return 0;

malformed:
    return -1;
}

static int
process_offer_files( struct packet_buffer *pb, struct e_client *client )
{
	size_t i;
	uint32_t count;

	PB_READ_UINT32(pb, count);
	PB_CHECK(count <= 200 );
	
	for( i=0; i<count; ++i ) {
		uint32_t tag_count, id;
		uint16_t port;
		struct e_file file = {0};

		PB_MEMCPY(pb, file.hash, sizeof file.hash);

		PB_READ_UINT32(pb, id);
		PB_READ_UINT16(pb, port);

		if ( (0xfbfbfbfb == id) && (0xfbfb == port) ) {
			// todo: count complete sources
			// 0xfc... -> incomplete file, 0xfb... -> complete
		}

		PB_READ_UINT32(pb, tag_count);

		for ( ; tag_count>0; --tag_count ) {
			struct tag_header *tag_hdr = (struct tag_header*)pb->ptr;

			// new tags currently unsupported
			PB_CHECK( (tag_hdr->type & 0x80) == 0 );

			PB_SKIP_TAGHDR(pb, tag_hdr);

			// string-named tags
			if (tag_hdr->name_len > 1 ) {
				if( memcmp(TNS_MEDIA_LENGTH, &tag_hdr->name, tag_hdr->name_len) == 0 ) {
					if ( TT_UINT32 == tag_hdr->type ) {
						uint32_t media_len;
						PB_READ_UINT32(pb, media_len);
					} else if ( TT_STRING == tag_hdr->type ) {
						uint16_t len;
						PB_READ_UINT16(pb, len);
						PB_SEEK(pb, len);
					} else {
						PB_CHECK(0);
					}
				} else if( memcmp(TNS_MEDIA_BITRATE, &tag_hdr->name, tag_hdr->name_len) == 0 ) {
					uint32_t bitrate;
					PB_CHECK( TT_UINT32 == tag_hdr->type );
					PB_READ_UINT32(pb, bitrate);
				} else if( memcmp(TNS_MEDIA_CODEC, &tag_hdr->name, tag_hdr->name_len) == 0 ) {
					uint16_t len;
					PB_CHECK( TT_STRING == tag_hdr->type );
					PB_READ_UINT16(pb, len);
					PB_SEEK(pb, len);
				} else {
					PB_CHECK(0);
				}
			} else {
				switch ( tag_hdr->name ) {

				case TN_FILENAME:
					PB_CHECK(TT_STRING == tag_hdr->type);
					PB_READ_UINT16(pb, file.name_len);
					file.name_len = file.name_len > MAX_FILENAME_LEN ? MAX_FILENAME_LEN : file.name_len;
					PB_MEMCPY(pb, file.name, file.name_len);
					file.name[file.name_len] = 0;
					break;

				case TN_FILESIZE:
					PB_CHECK(TT_UINT32 == tag_hdr->type);
					PB_READ_UINT32(pb, file.size);
					break;

				case FT_FILESIZE_HI: {
					uint32_t size_hi;
					PB_CHECK(TT_UINT32 == tag_hdr->type);
					PB_READ_UINT32(pb, size_hi);
					file.size = (uint64_t)size_hi << 32;
					break;
				}

				case TN_FILERATING:
					PB_CHECK(TT_UINT32 == tag_hdr->type);
					PB_READ_UINT8(pb, file.rating);
					break;

				case TN_FILETYPE:
					if ( TT_UINT32 == tag_hdr->type ) {
						PB_READ_UINT8(pb, file.type);
					} else if ( TT_STRING == tag_hdr->type ) {
						// todo: read string file type and find its integer representation
					} else {
						PB_CHECK(0);
					}
					break;

				default:
					PB_CHECK(0);
				}
			}
		}



		db_add_file( &file, client );

		count--;
	}

	return 0;

malformed:
	return -1;
}

static int
process_packet( struct packet_buffer *pb, uint8_t opcode, struct e_client *client )
{
	// verify portcheck state

    switch ( opcode ) {
    case OP_LOGINREQUEST:
        PB_CHECK( process_login_request(pb, client) == 0 );
        return 0;

	case OP_GETSERVERLIST:
		// todo: implement me
		return 0;

	case OP_SEARCHREQUEST:
		// todo: implement me
		return 0;

	case OP_GETSOURCES:
		send_found_sources(client, pb->ptr);
		return 0;

	case OP_OFFERFILES:
		PB_CHECK( process_offer_files(pb, client) == 0 );
		return 0;

    default:
        PB_CHECK(0);
    }

malformed:
    return -1;
}

static void
read_cb( struct bufferevent *bev, void *ctx )
{
	struct e_client *client = (struct e_client*)ctx;
	struct evbuffer *input = bufferevent_get_input(bev);
	size_t src_len = evbuffer_get_length(input);

	while( src_len > sizeof(struct packet_header) ) {
		unsigned char *data;
		struct packet_buffer pb;
		size_t packet_len;
		int ret;
		const struct packet_header *header =
			(struct packet_header*)evbuffer_pullup(input, sizeof(struct packet_header));

		// check header protocol
		if  ( PROTO_PACKED != header->proto && PROTO_EDONKEY != header->proto ) {
#ifdef DEBUG
			ED2KD_LOGDBG("unknown packet protocol from %s:%u", client->dbg.ip_str, client->port);
#endif
			client_delete(client);
			return;
		}

		// wait for full length packet
		packet_len = header->length + sizeof(struct packet_header);
		// todo: max packet size limit
		if ( packet_len > src_len )
			return;

		data = evbuffer_pullup(input, packet_len) + sizeof(struct packet_header);

		if ( PROTO_PACKED == header->proto ) {
			unsigned char *unpacked;
			unsigned long unpacked_len = header->length*10 + 300;
			if ( unpacked_len > 50000 ) {
				 unpacked_len = 50000;
			}
			unpacked = (unsigned char*)malloc(unpacked_len);
			ret = uncompress(unpacked, &unpacked_len, data+1, header->length-1);
			if ( Z_OK == ret ) {
				PB_INIT(&pb, unpacked, unpacked_len);
				ret = process_packet(&pb, *data, client);
			} else {
#ifdef DEBUG
				ED2KD_LOGDBG("failed to unpack packet from %s:%u", client->dbg.ip_str, client->port);
#endif
				ret = -1;
			}
			free(unpacked);
		} else {
			PB_INIT(&pb, data+1, header->length-1);
			ret = process_packet(&pb, *data, client);
		}

		
		if (  ret < 0 ) {
#ifdef DEBUG
			ED2KD_LOGDBG("server packet parsing error (%s:%u)", client->dbg.ip_str, client->port);
#endif
			client_delete(client);
			return;
		}

		evbuffer_drain(input, packet_len);
		src_len = evbuffer_get_length(input);
	}    
}

static void
event_cb( struct bufferevent *bev, short events, void *ctx )
{
	if ( events & (BEV_EVENT_EOF | BEV_EVENT_ERROR) ) {
		struct e_client *client = (struct e_client*)ctx;
#ifdef DEBUG
        ED2KD_LOGDBG("got EOF or error from %s:%u", client->dbg.ip_str, client->port);
#endif
        client_delete(client);
    }
}

static void
accept_cb( struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *sa, int socklen, void *ctx )
{
    struct sockaddr_in *peer_sa = (struct sockaddr_in*)sa;
	struct e_client *client;
	struct event_base *base;
	struct bufferevent *bev;

    // todo: limit connections from same ip
    // todo: block banned ips

    client = client_new();

    base = evconnlistener_get_base(listener);
    bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);

    client->ip = peer_sa->sin_addr.s_addr;
    client->bev_srv = bev;
#ifdef DEBUG
	evutil_inet_ntop(AF_INET, &(client->ip), client->dbg.ip_str, sizeof client->dbg.ip_str);
	ED2KD_LOGNFO("client connected (%s)", client->dbg.ip_str);
#endif

    bufferevent_setcb(bev, read_cb, NULL, event_cb, client);
    bufferevent_enable(bev, EV_READ|EV_WRITE);

    // todo: set timeout for op_login
}

static void
accept_error_cb( struct evconnlistener * listener, void * ctx )
{
    struct event_base * base = evconnlistener_get_base(listener);
    int err = EVUTIL_SOCKET_ERROR();
    ED2KD_LOGERR("error %d (%s) on the listener. terminating...", \
        err, evutil_socket_error_to_string(err));

    event_base_loopexit(base, NULL);
}

static void
signal_cb( evutil_socket_t fd, short what, void * ctx )
{
    struct event_base * base = (struct event_base *)ctx;

    ED2KD_LOGNFO("caught SIGINT, terminating...");
    event_base_loopexit(base, NULL);
}

int ed2kd_run()
{
    int ret;
	struct event_base *base;
	struct event *sigint_event;
	struct sockaddr_in bind_sa;
	int bind_sa_len;
	struct evconnlistener *listener;

#ifdef DEBUG
    {
        int i;
        const char **methods = event_get_supported_methods();
        if ( NULL == methods ) {
            ED2KD_LOGERR("failed to get supported libevent methods");
            return EXIT_FAILURE;
        }
        ED2KD_LOGDBG("using libevent %s. available methods are:", event_get_version());
        for ( i=0; methods[i] != NULL; ++i ) {
            ED2KD_LOGDBG("    %s", methods[i]);
        }
    }
#endif

#ifdef EVTHREAD_USE_WINDOWS_THREADS_IMPLEMENTED
    ret = evthread_use_windows_threads();
#elif EVTHREAD_USE_PTHREADS_IMPLEMENTED
    ret = evthread_use_pthreads();
#else
#error "unable to determine threading model"
#endif
    if ( ret < 0 ) {
        ED2KD_LOGERR("failed to init libevent threading model");
        return EXIT_FAILURE;
    }

    base = event_base_new();
    if ( NULL == base ) {
        // todo: get last error
        ED2KD_LOGERR("failed to create main event loop");
        return EXIT_FAILURE;
    }

    // setup signals
    sigint_event = evsignal_new(base, SIGINT, signal_cb, base);
    evsignal_add(sigint_event, NULL);

    bind_sa_len = sizeof(bind_sa);
    memset(&bind_sa, 0, sizeof(bind_sa));
    ret = evutil_parse_sockaddr_port(ed2kd_cfg()->listen_addr, (struct sockaddr*)&bind_sa, &bind_sa_len);
    bind_sa.sin_port = htons(ed2kd_cfg()->listen_port);
    bind_sa.sin_family = AF_INET;

    listener = evconnlistener_new_bind(base,
        accept_cb, NULL, LEV_OPT_CLOSE_ON_FREE|LEV_OPT_REUSEABLE,
        ed2kd_cfg()->listen_backlog, (struct sockaddr*)&bind_sa, sizeof(bind_sa) );
    if ( NULL == listener ) {
        // todo: get last error
        ED2KD_LOGERR("failed to start listen on %s:%u", ed2kd_cfg()->listen_addr, ed2kd_cfg()->listen_port);
        return EXIT_FAILURE;
    }

    evconnlistener_set_error_cb(listener, accept_error_cb);

    ED2KD_LOGNFO("start listening on %s:%u", ed2kd_cfg()->listen_addr, ed2kd_cfg()->listen_port);

    ret = event_base_dispatch(base);
    if ( ret < 0 ) {
        // todo: get last error
        ED2KD_LOGERR("failed to start main event loop");
        return EXIT_FAILURE;
    }
    else if ( 0 == ret ) {
        ED2KD_LOGWRN("no active events in main loop");
    }

    evconnlistener_free(listener);
    event_free(sigint_event);
    event_base_free(base);

    return EXIT_SUCCESS;
}
