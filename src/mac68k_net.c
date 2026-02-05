/*
 * mac68k_net.c - Networking for 68k Macintosh using Open Transport
 *
 * This file provides BSD socket-like functions implemented on top of
 * the Open Transport networking stack.
 *
 * Requirements:
 *   - Mac OS with Open Transport installed (System 7.5.2+)
 *   - Open Transport TCP/IP
 */

#ifdef MAC68K_PLATFORM

#include <OpenTransport.h>
#include <OpenTransportProviders.h>
#include <string.h>

#include "mac68k_net.h"
#include "mac68k_console.h"
#include "globals.h"

/* Global errno for error reporting */
int errno = 0;

/* Maximum number of connections we can track */
#define MAX_STREAMS 34  /* 1 listener + MAX_PLAYERS clients + margin */

/* Peek buffer size - enough for protocol header checks */
#define PEEK_BUFFER_SIZE 16

/* Option buffer for TCP_NODELAY and other options */
#define OPT_BUFFER_SIZE 64

/* Stream state */
typedef struct {
    EndpointRef endpoint;           /* Open Transport endpoint reference */
    int in_use;                     /* Is this slot in use? */
    int is_listener;                /* Is this a listening socket? */
    int is_connected;               /* Is connection established? */
    InetAddress local_addr;         /* Local address (for bind) */
    InetAddress remote_addr;        /* Remote address (after accept) */

    /* For listener: pending connection info */
    TCall pending_call;             /* Stores pending connection info */
    InetAddress pending_addr;       /* Buffer for pending call address */
    int has_pending;                /* Is there a pending connection? */

    /* Peek buffer for MSG_PEEK support */
    unsigned char peek_buf[PEEK_BUFFER_SIZE];
    int peek_len;                   /* How many bytes in peek buffer */

    /* Disconnect state tracking */
    int orderly_disconnect_sent;    /* Have we sent orderly disconnect? */
    int orderly_disconnect_rcvd;    /* Have we received orderly disconnect? */
} OTStreamInfo;

/* File descriptor to stream mapping */
/* FD 0-2 reserved for stdin/stdout/stderr, start at 3 */
#define FD_BASE 3
static OTStreamInfo g_streams[MAX_STREAMS];
static int g_ot_initialized = 0;

/* Convert fake fd to stream index */
static int fd_to_index(int fd) {
    int idx = fd - FD_BASE;
    if (idx < 0 || idx >= MAX_STREAMS) return -1;
    return idx;
}

/* Find a free stream slot, returns fake fd or -1 */
static int alloc_stream_slot(void) {
    int i;
    for (i = 0; i < MAX_STREAMS; i++) {
        if (!g_streams[i].in_use) {
            memset(&g_streams[i], 0, sizeof(OTStreamInfo));
            g_streams[i].in_use = 1;
            g_streams[i].endpoint = kOTInvalidEndpointRef;
            return i + FD_BASE;
        }
    }
    return -1;
}

/* Initialize Open Transport - called automatically on first socket() */
static int init_open_transport(void) {
    OSStatus err;

    if (g_ot_initialized) return 0;

    console_print("Initializing Open Transport...\r");

    /* Initialize Open Transport subsystem */
    err = InitOpenTransport();
    if (err != noErr) {
        console_printf("ERROR: InitOpenTransport failed: %ld\r", (long)err);
        console_print("Open Transport may not be installed.\r");
        return -1;
    }

    memset(g_streams, 0, sizeof(g_streams));
    g_ot_initialized = 1;
    console_print("Open Transport initialized.\r");
    return 0;
}

/*
 * Configure TCP endpoint for low-latency game server operation.
 * Sets TCP_NODELAY to disable Nagle's algorithm and configures
 * the endpoint for optimal non-blocking performance.
 */
static void configure_endpoint_for_performance(EndpointRef endpoint) {
    UInt8 opt_buffer[OPT_BUFFER_SIZE];
    TOptMgmt opt_req, opt_ret;
    TOption *opt;
    OSStatus err;

    /* Build option request for TCP_NODELAY */
    OTMemzero(opt_buffer, sizeof(opt_buffer));
    opt = (TOption *)opt_buffer;
    opt->level = INET_TCP;
    opt->name = TCP_NODELAY;
    opt->len = kOTFourByteOptionSize;
    *(UInt32 *)opt->value = 1;  /* Enable TCP_NODELAY */

    opt_req.flags = T_NEGOTIATE;
    opt_req.opt.buf = opt_buffer;
    opt_req.opt.len = opt->len;

    opt_ret.flags = 0;
    opt_ret.opt.buf = opt_buffer;
    opt_ret.opt.maxlen = sizeof(opt_buffer);

    err = OTOptionManagement(endpoint, &opt_req, &opt_ret);
    if (err != noErr) {
        /* Non-fatal - continue without TCP_NODELAY */
        console_printf("Note: TCP_NODELAY not set: %ld\r", (long)err);
    }

    /* Configure endpoint to not wait for send acknowledgments.
     * This makes OTSnd return immediately without waiting for
     * the data to be acknowledged, improving throughput. */
    OTDontAckSends(endpoint);
}

/* Drain endpoint of remaining data during shutdown */
static void drain_endpoint(EndpointRef endpoint) {
    char drain_buf[256];
    OTFlags flags;
    OTResult result;
    int attempts = 0;
    const int max_attempts = 10;

    /* Drain any pending data */
    while (attempts < max_attempts) {
        result = OTRcv(endpoint, drain_buf, sizeof(drain_buf), &flags);
        if (result == kOTLookErr) {
            OTResult look = OTLook(endpoint);
            if (look == T_ORDREL) {
                OTRcvOrderlyDisconnect(endpoint);
                break;
            } else if (look == T_DISCONNECT) {
                OTRcvDisconnect(endpoint, NULL);
                break;
            }
        } else if (result <= 0) {
            break;
        }
        attempts++;
    }
}

/* Handle disconnect events, return 1 if connection is closed */
static int handle_disconnect_event(OTStreamInfo *info, OTResult event) {
    switch (event) {
        case T_DISCONNECT:
            /* Abortive disconnect */
            OTRcvDisconnect(info->endpoint, NULL);
            info->is_connected = 0;
            return 1;

        case T_ORDREL:
            /* Orderly disconnect from peer */
            OTRcvOrderlyDisconnect(info->endpoint);
            info->orderly_disconnect_rcvd = 1;

            /* If we haven't sent ours, do it now */
            if (!info->orderly_disconnect_sent) {
                OTSndOrderlyDisconnect(info->endpoint);
                info->orderly_disconnect_sent = 1;
            }
            info->is_connected = 0;
            return 1;

        default:
            return 0;
    }
}

int socket(int domain, int type, int protocol) {
    OSStatus err;
    int fd;
    OTStreamInfo *info;
    OTConfigurationRef config;

    (void)domain; (void)protocol;

    if (!g_ot_initialized) {
        if (init_open_transport() < 0) {
            errno = ENOTSOCK;
            return -1;
        }
    }

    if (type != SOCK_STREAM) {
        errno = ENOTSOCK;
        return -1;
    }

    fd = alloc_stream_slot();
    if (fd < 0) {
        errno = EMFILE;
        return -1;
    }

    info = &g_streams[fd - FD_BASE];

    /* Create TCP endpoint configuration */
    config = OTCreateConfiguration(kTCPName);
    if (config == NULL) {
        info->in_use = 0;
        errno = ENOTSOCK;
        return -1;
    }

    /* Open the endpoint */
    info->endpoint = OTOpenEndpoint(config, 0, NULL, &err);
    if (err != noErr || info->endpoint == kOTInvalidEndpointRef) {
        console_printf("ERROR: OTOpenEndpoint failed: %ld\r", (long)err);
        info->in_use = 0;
        errno = ENOTSOCK;
        return -1;
    }

    /* Configure endpoint:
     * - Synchronous mode for simpler operation
     * - Non-blocking for integration with server loop
     */
    OTSetSynchronous(info->endpoint);
    OTSetNonBlocking(info->endpoint);

    info->is_listener = 0;
    info->is_connected = 0;
    info->peek_len = 0;
    info->has_pending = 0;
    info->orderly_disconnect_sent = 0;
    info->orderly_disconnect_rcvd = 0;

    return fd;
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int idx;
    OTStreamInfo *info;
    const struct sockaddr_in *sin;
    TBind req_bind, ret_bind;
    OSStatus err;

    (void)addrlen;

    idx = fd_to_index(sockfd);
    if (idx < 0 || !g_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_streams[idx];
    sin = (const struct sockaddr_in *)addr;

    /* Initialize the bind address structure */
    OTInitInetAddress(&info->local_addr, sin->sin_port, sin->sin_addr.s_addr);

    /* Set up bind request */
    OTMemzero(&req_bind, sizeof(TBind));
    req_bind.addr.buf = (UInt8 *)&info->local_addr;
    req_bind.addr.len = sizeof(InetAddress);
    req_bind.qlen = 5;  /* Backlog for listener sockets */

    OTMemzero(&ret_bind, sizeof(TBind));
    ret_bind.addr.buf = (UInt8 *)&info->local_addr;
    ret_bind.addr.maxlen = sizeof(InetAddress);

    err = OTBind(info->endpoint, &req_bind, &ret_bind);
    if (err != noErr) {
        console_printf("ERROR: OTBind failed: %ld\r", (long)err);
        errno = EADDRINUSE;
        return -1;
    }

    console_printf("Bound to port %u\r", (unsigned)sin->sin_port);
    return 0;
}

int listen(int sockfd, int backlog) {
    int idx;
    OTStreamInfo *info;

    (void)backlog;  /* Already set in bind via qlen */

    idx = fd_to_index(sockfd);
    if (idx < 0 || !g_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_streams[idx];
    info->is_listener = 1;
    info->has_pending = 0;

    console_printf("Listening on port %u...\r", (unsigned)info->local_addr.fPort);
    return 0;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int idx;
    OTStreamInfo *info;
    int new_fd;
    OTStreamInfo *new_info;
    OSStatus err;
    OTResult look_result;
    OTConfigurationRef config;
    TBind bind_req;

    idx = fd_to_index(sockfd);
    if (idx < 0 || !g_streams[idx].in_use || !g_streams[idx].is_listener) {
        errno = EBADF;
        return -1;
    }

    info = &g_streams[idx];

    /* Check if we already have a pending connection from previous poll */
    if (!info->has_pending) {
        /* Check for T_LISTEN event indicating incoming connection */
        look_result = OTLook(info->endpoint);

        if (look_result == T_LISTEN) {
            /* Set up the call structure to receive connection info */
            OTMemzero(&info->pending_call, sizeof(TCall));
            info->pending_call.addr.buf = (UInt8 *)&info->pending_addr;
            info->pending_call.addr.maxlen = sizeof(InetAddress);

            err = OTListen(info->endpoint, &info->pending_call);
            if (err == noErr) {
                info->has_pending = 1;
            } else if (err != kOTNoDataErr) {
                console_printf("OTListen error: %ld\r", (long)err);
            }
        }
    }

    if (!info->has_pending) {
        /* No pending connection - give time and return */
        console_poll_events();
        errno = EAGAIN;
        return -1;
    }

    /* Allocate a new fd for this connection */
    new_fd = alloc_stream_slot();
    if (new_fd < 0) {
        /* Reject connection - no slots available */
        OTSndDisconnect(info->endpoint, &info->pending_call);
        info->has_pending = 0;
        errno = EMFILE;
        return -1;
    }

    new_info = &g_streams[new_fd - FD_BASE];

    /* Create a new endpoint for the accepted connection */
    config = OTCreateConfiguration(kTCPName);
    if (config == NULL) {
        new_info->in_use = 0;
        OTSndDisconnect(info->endpoint, &info->pending_call);
        info->has_pending = 0;
        errno = EMFILE;
        return -1;
    }

    new_info->endpoint = OTOpenEndpoint(config, 0, NULL, &err);
    if (err != noErr || new_info->endpoint == kOTInvalidEndpointRef) {
        console_printf("ERROR: OTOpenEndpoint for accept failed: %ld\r", (long)err);
        new_info->in_use = 0;
        OTSndDisconnect(info->endpoint, &info->pending_call);
        info->has_pending = 0;
        errno = EMFILE;
        return -1;
    }

    /* Configure the new endpoint */
    OTSetSynchronous(new_info->endpoint);
    OTSetNonBlocking(new_info->endpoint);

    /* Bind the new endpoint (ephemeral port, qlen=0 since not a listener) */
    OTMemzero(&bind_req, sizeof(TBind));
    bind_req.qlen = 0;

    err = OTBind(new_info->endpoint, NULL, NULL);
    if (err != noErr) {
        console_printf("OTBind for accept failed: %ld\r", (long)err);
        OTCloseProvider(new_info->endpoint);
        new_info->in_use = 0;
        OTSndDisconnect(info->endpoint, &info->pending_call);
        info->has_pending = 0;
        errno = EMFILE;
        return -1;
    }

    /* Accept the connection onto the new endpoint */
    err = OTAccept(info->endpoint, new_info->endpoint, &info->pending_call);
    if (err != noErr) {
        console_printf("OTAccept error: %ld\r", (long)err);
        OTUnbind(new_info->endpoint);
        OTCloseProvider(new_info->endpoint);
        new_info->in_use = 0;
        info->has_pending = 0;
        errno = EAGAIN;
        return -1;
    }

    /* Connection accepted successfully */
    info->has_pending = 0;
    new_info->is_connected = 1;
    new_info->is_listener = 0;
    new_info->peek_len = 0;
    new_info->orderly_disconnect_sent = 0;
    new_info->orderly_disconnect_rcvd = 0;
    memcpy(&new_info->remote_addr, &info->pending_addr, sizeof(InetAddress));

    /* Apply performance optimizations (TCP_NODELAY, etc.) */
    configure_endpoint_for_performance(new_info->endpoint);

    /* Fill in the address if requested */
    if (addr != NULL && addrlen != NULL) {
        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
        sin->sin_family = AF_INET;
        sin->sin_port = new_info->remote_addr.fPort;
        sin->sin_addr.s_addr = new_info->remote_addr.fHost;
        *addrlen = sizeof(struct sockaddr_in);
    }

    console_print("Client connected!\r");
    return new_fd;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    int idx;
    OTStreamInfo *info;
    OTResult result;
    OTResult look_result;

    (void)flags;

    idx = fd_to_index(sockfd);
    if (idx < 0 || !g_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_streams[idx];
    if (!info->is_connected) {
        errno = ENOTCONN;
        return -1;
    }

    /* Send data - OTSnd will return kOTLookErr if disconnect occurred */
    result = OTSnd(info->endpoint, (void *)buf, len, 0);

    if (result >= 0) {
        return (ssize_t)result;
    }

    /* Handle errors */
    if (result == kOTLookErr) {
        look_result = OTLook(info->endpoint);
        handle_disconnect_event(info, look_result);
        errno = ECONNRESET;
        return -1;
    }

    if (result == kOTFlowErr) {
        /* Flow control - try again later */
        errno = EAGAIN;
        return -1;
    }

    console_printf("OTSnd error: %ld\r", (long)result);
    errno = ECONNRESET;
    return -1;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    int idx;
    OTStreamInfo *info;
    OTResult result;
    OTFlags ot_flags = 0;
    OTResult look_result;
    OTByteCount avail;
    size_t copy_len;

    idx = fd_to_index(sockfd);
    if (idx < 0 || !g_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_streams[idx];
    if (!info->is_connected) {
        errno = ENOTCONN;
        return -1;
    }

    /* Handle MSG_PEEK - read data but keep it for next recv */
    if (flags & MSG_PEEK) {
        /* If we already have enough peeked data, return it */
        if (info->peek_len >= (int)len) {
            memcpy(buf, info->peek_buf, len);
            return (ssize_t)len;
        }

        /* Try to read more data into peek buffer */
        if (info->peek_len < PEEK_BUFFER_SIZE) {
            /* Performance: Check if data is available first */
            if (OTCountDataBytes(info->endpoint, &avail) == noErr && avail > 0) {
                size_t to_read = PEEK_BUFFER_SIZE - info->peek_len;
                if (to_read > (size_t)avail) to_read = (size_t)avail;

                result = OTRcv(info->endpoint,
                              info->peek_buf + info->peek_len,
                              to_read, &ot_flags);

                if (result > 0) {
                    info->peek_len += result;
                } else if (result == kOTLookErr) {
                    look_result = OTLook(info->endpoint);
                    if (handle_disconnect_event(info, look_result)) {
                        return 0;  /* Connection closed */
                    }
                }
            }
        }

        /* Return what we have in peek buffer */
        if (info->peek_len > 0) {
            copy_len = ((size_t)info->peek_len < len) ? (size_t)info->peek_len : len;
            memcpy(buf, info->peek_buf, copy_len);
            return (ssize_t)copy_len;
        }

        errno = EAGAIN;
        return -1;
    }

    /* Normal recv (not peek) - first drain peek buffer */
    if (info->peek_len > 0) {
        copy_len = ((size_t)info->peek_len < len) ? (size_t)info->peek_len : len;
        memcpy(buf, info->peek_buf, copy_len);

        /* Shift remaining peek data */
        if (copy_len < (size_t)info->peek_len) {
            memmove(info->peek_buf, info->peek_buf + copy_len,
                    info->peek_len - copy_len);
        }
        info->peek_len -= copy_len;

        return (ssize_t)copy_len;
    }

    /* Performance: Check if data is available before recv call */
    if (OTCountDataBytes(info->endpoint, &avail) == noErr && avail == 0) {
        /* No data available - check for events */
        look_result = OTLook(info->endpoint);
        if (look_result == T_DISCONNECT || look_result == T_ORDREL) {
            if (handle_disconnect_event(info, look_result)) {
                return 0;  /* Connection closed cleanly */
            }
            errno = ECONNRESET;
            return -1;
        }
        errno = EAGAIN;
        return -1;
    }

    /* Read from network */
    result = OTRcv(info->endpoint, buf, len, &ot_flags);

    if (result > 0) {
        return (ssize_t)result;
    }

    if (result == 0) {
        return 0;  /* Connection closed */
    }

    /* Handle errors */
    if (result == kOTNoDataErr) {
        errno = EAGAIN;
        return -1;
    }

    if (result == kOTLookErr) {
        look_result = OTLook(info->endpoint);
        if (handle_disconnect_event(info, look_result)) {
            return 0;  /* Connection closed */
        }
        errno = ECONNRESET;
        return -1;
    }

    console_printf("OTRcv error: %ld\r", (long)result);
    errno = ECONNRESET;
    return -1;
}

int close(int fd) {
    int idx;
    OTStreamInfo *info;

    idx = fd_to_index(fd);
    if (idx < 0 || !g_streams[idx].in_use) {
        return 0;
    }

    info = &g_streams[idx];

    if (info->endpoint != kOTInvalidEndpointRef) {
        if (info->is_connected && !info->orderly_disconnect_sent) {
            /* Initiate orderly disconnect */
            OTSndOrderlyDisconnect(info->endpoint);
            info->orderly_disconnect_sent = 1;

            /* Drain remaining data */
            drain_endpoint(info->endpoint);
        }

        OTUnbind(info->endpoint);
        OTCloseProvider(info->endpoint);
        info->endpoint = kOTInvalidEndpointRef;
    }

    info->in_use = 0;
    return 0;
}

int shutdown(int sockfd, int how) {
    int idx;
    OTStreamInfo *info;

    (void)how;

    idx = fd_to_index(sockfd);
    if (idx < 0 || !g_streams[idx].in_use) {
        return 0;
    }

    info = &g_streams[idx];

    if (info->is_connected && info->endpoint != kOTInvalidEndpointRef) {
        if (!info->orderly_disconnect_sent) {
            OTSndOrderlyDisconnect(info->endpoint);
            info->orderly_disconnect_sent = 1;
        }
        info->is_connected = 0;
    }

    return 0;
}

/* Cleanup Open Transport - call on exit */
void cleanup_open_transport(void) {
    int i;
    if (g_ot_initialized) {
        /* Close all endpoints with orderly disconnect */
        for (i = 0; i < MAX_STREAMS; i++) {
            if (g_streams[i].in_use && g_streams[i].endpoint != kOTInvalidEndpointRef) {
                if (g_streams[i].is_connected && !g_streams[i].orderly_disconnect_sent) {
                    OTSndOrderlyDisconnect(g_streams[i].endpoint);
                    drain_endpoint(g_streams[i].endpoint);
                }
                OTUnbind(g_streams[i].endpoint);
                OTCloseProvider(g_streams[i].endpoint);
            }
        }
        CloseOpenTransport();
        g_ot_initialized = 0;
        console_print("Open Transport closed.\r");
    }
}

/* Alias for cleanup - keep compatibility with old code */
void cleanup_mactcp(void) {
    cleanup_open_transport();
}

/* Common functions */

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int idx;
    OTStreamInfo *info;
    const struct sockaddr_in *sin;
    OSStatus err;
    TCall snd_call;
    InetAddress inet_addr;

    (void)addrlen;

    idx = fd_to_index(sockfd);
    if (idx < 0 || !g_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_streams[idx];
    sin = (const struct sockaddr_in *)addr;

    /* Bind if not already bound */
    if (OTGetEndpointState(info->endpoint) == T_UNBND) {
        err = OTBind(info->endpoint, NULL, NULL);
        if (err != noErr) {
            console_printf("OTBind for connect failed: %ld\r", (long)err);
            errno = ECONNREFUSED;
            return -1;
        }
    }

    /* Set up connection address */
    OTInitInetAddress(&inet_addr, sin->sin_port, sin->sin_addr.s_addr);

    OTMemzero(&snd_call, sizeof(TCall));
    snd_call.addr.buf = (UInt8 *)&inet_addr;
    snd_call.addr.len = sizeof(InetAddress);

    /* Temporarily set blocking for connect */
    OTSetBlocking(info->endpoint);

    err = OTConnect(info->endpoint, &snd_call, NULL);

    /* Restore non-blocking */
    OTSetNonBlocking(info->endpoint);

    if (err != noErr) {
        console_printf("OTConnect failed: %ld\r", (long)err);
        errno = ECONNREFUSED;
        return -1;
    }

    info->is_connected = 1;
    memcpy(&info->remote_addr, &inet_addr, sizeof(InetAddress));

    return 0;
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    (void)sockfd; (void)level; (void)optname; (void)optval; (void)optlen;
    /* Most socket options not directly supported by OT - just succeed */
    return 0;
}

int fcntl(int fd, int cmd, ...) {
    (void)fd;
    if (cmd == F_GETFL) {
        return O_NONBLOCK;  /* We're always non-blocking */
    }
    return 0;
}

uint16_t htons(uint16_t hostshort) {
    /* 68k is big-endian, same as network byte order */
    return hostshort;
}

uint32_t htonl(uint32_t hostlong) {
    /* 68k is big-endian, same as network byte order */
    return hostlong;
}

uint16_t ntohs(uint16_t netshort) {
    return netshort;
}

uint32_t ntohl(uint32_t netlong) {
    return netlong;
}

/* Task yield for cooperative multitasking - polls Mac events */
void task_yield(void) {
    console_poll_events();
}

#endif /* MAC68K_PLATFORM */
