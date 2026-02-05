/*
 * mac68k_net.c - Networking for 68k Macintosh
 *
 * This file provides BSD socket-like functions with automatic detection
 * of the available networking stack:
 *   - Open Transport (System 7.5.2+) - preferred
 *   - MacTCP (System 7.0+) - fallback
 */

#ifdef MAC68K_PLATFORM

#include <Gestalt.h>
#include <string.h>

/* Include system networking headers FIRST - they define O_NONBLOCK etc. */
#include <OpenTransport.h>
#include <OpenTransportProviders.h>
#include <MacTCP.h>
#include <mactcp/TCPHi.h>
#include <mactcp/CvtAddr.h>
#include <mactcp/TCPRoutines.h>

/* Now include our headers - they use #ifndef guards for constants */
#include "mac68k_net.h"
#include "mac68k_console.h"
#include "globals.h"

/* Global errno for error reporting */
int errno = 0;

/* Maximum number of connections we can track */
#define MAX_STREAMS 34  /* 1 listener + MAX_PLAYERS clients + margin */

/* Peek buffer size - enough for protocol header checks */
#define PEEK_BUFFER_SIZE 16

/* Which networking stack are we using? */
static int g_use_open_transport = 0;
static int g_net_initialized = 0;
static int g_ot_initialized = 0;
static int g_mactcp_initialized = 0;
static int g_net_restart_needed = 0;
static int g_user_stack_choice = -1;  /* -1 = auto, 0 = MacTCP, 1 = OT */

/* File descriptor base - 0-2 reserved for stdin/stdout/stderr */
#define FD_BASE 3

/* ============================================================================
 * OPEN TRANSPORT IMPLEMENTATION
 * ============================================================================ */

#define OPT_BUFFER_SIZE 64

typedef struct {
    EndpointRef endpoint;
    int in_use;
    int is_listener;
    int is_connected;
    InetAddress local_addr;
    InetAddress remote_addr;
    TCall pending_call;
    InetAddress pending_addr;
    int has_pending;
    unsigned char peek_buf[PEEK_BUFFER_SIZE];
    int peek_len;
    int orderly_disconnect_sent;
    int orderly_disconnect_rcvd;
} OTStreamInfo;

static OTStreamInfo g_ot_streams[MAX_STREAMS];

static int ot_fd_to_index(int fd) {
    int idx = fd - FD_BASE;
    if (idx < 0 || idx >= MAX_STREAMS) return -1;
    return idx;
}

static int ot_alloc_stream_slot(void) {
    int i;
    for (i = 0; i < MAX_STREAMS; i++) {
        if (!g_ot_streams[i].in_use) {
            memset(&g_ot_streams[i], 0, sizeof(OTStreamInfo));
            g_ot_streams[i].in_use = 1;
            g_ot_streams[i].endpoint = kOTInvalidEndpointRef;
            return i + FD_BASE;
        }
    }
    return -1;
}

static void ot_configure_endpoint_for_performance(EndpointRef endpoint) {
    UInt8 opt_buffer[OPT_BUFFER_SIZE];
    TOptMgmt opt_req, opt_ret;
    TOption *opt;

    OTMemzero(opt_buffer, sizeof(opt_buffer));
    opt = (TOption *)opt_buffer;
    opt->level = INET_TCP;
    opt->name = TCP_NODELAY;
    opt->len = kOTFourByteOptionSize;
    *(UInt32 *)opt->value = 1;

    opt_req.flags = T_NEGOTIATE;
    opt_req.opt.buf = opt_buffer;
    opt_req.opt.len = opt->len;
    opt_ret.flags = 0;
    opt_ret.opt.buf = opt_buffer;
    opt_ret.opt.maxlen = sizeof(opt_buffer);

    OTOptionManagement(endpoint, &opt_req, &opt_ret);
    OTDontAckSends(endpoint);
}

static void ot_drain_endpoint(EndpointRef endpoint) {
    char drain_buf[256];
    OTFlags flags;
    OTResult result;
    int attempts = 0;

    while (attempts < 10) {
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

static int ot_handle_disconnect_event(OTStreamInfo *info, OTResult event) {
    switch (event) {
        case T_DISCONNECT:
            OTRcvDisconnect(info->endpoint, NULL);
            info->is_connected = 0;
            return 1;
        case T_ORDREL:
            OTRcvOrderlyDisconnect(info->endpoint);
            info->orderly_disconnect_rcvd = 1;
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

static int ot_init(void) {
    OSStatus err = InitOpenTransport();
    if (err != noErr) {
        console_printf("ERROR: InitOpenTransport failed: %ld\r", (long)err);
        return -1;
    }
    memset(g_ot_streams, 0, sizeof(g_ot_streams));
    g_ot_initialized = 1;
    console_print("Open Transport initialized.\r");
    return 0;
}

static int ot_socket(void) {
    int fd;
    OTStreamInfo *info;
    OTConfigurationRef config;
    OSStatus err;

    fd = ot_alloc_stream_slot();
    if (fd < 0) {
        errno = EMFILE;
        return -1;
    }

    info = &g_ot_streams[fd - FD_BASE];
    config = OTCreateConfiguration(kTCPName);
    if (config == NULL) {
        info->in_use = 0;
        errno = ENOTSOCK;
        return -1;
    }

    info->endpoint = OTOpenEndpoint(config, 0, NULL, &err);
    if (err != noErr || info->endpoint == kOTInvalidEndpointRef) {
        info->in_use = 0;
        errno = ENOTSOCK;
        return -1;
    }

    OTSetSynchronous(info->endpoint);
    OTSetNonBlocking(info->endpoint);

    return fd;
}

static int ot_bind(int sockfd, const struct sockaddr_in *sin) {
    int idx = ot_fd_to_index(sockfd);
    OTStreamInfo *info;
    TBind req_bind, ret_bind;
    OSStatus err;

    if (idx < 0 || !g_ot_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_ot_streams[idx];
    OTInitInetAddress(&info->local_addr, sin->sin_port, sin->sin_addr.s_addr);

    OTMemzero(&req_bind, sizeof(TBind));
    req_bind.addr.buf = (UInt8 *)&info->local_addr;
    req_bind.addr.len = sizeof(InetAddress);
    req_bind.qlen = 5;

    OTMemzero(&ret_bind, sizeof(TBind));
    ret_bind.addr.buf = (UInt8 *)&info->local_addr;
    ret_bind.addr.maxlen = sizeof(InetAddress);

    err = OTBind(info->endpoint, &req_bind, &ret_bind);
    if (err != noErr) {
        errno = EADDRINUSE;
        return -1;
    }

    return 0;
}

static int ot_listen(int sockfd) {
    int idx = ot_fd_to_index(sockfd);
    if (idx < 0 || !g_ot_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }
    g_ot_streams[idx].is_listener = 1;
    g_ot_streams[idx].has_pending = 0;
    return 0;
}

static int ot_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int idx = ot_fd_to_index(sockfd);
    OTStreamInfo *info, *new_info;
    int new_fd;
    OSStatus err;
    OTResult look_result;
    OTConfigurationRef config;

    if (idx < 0 || !g_ot_streams[idx].in_use || !g_ot_streams[idx].is_listener) {
        errno = EBADF;
        return -1;
    }

    info = &g_ot_streams[idx];

    if (!info->has_pending) {
        look_result = OTLook(info->endpoint);
        if (look_result == T_LISTEN) {
            OTMemzero(&info->pending_call, sizeof(TCall));
            info->pending_call.addr.buf = (UInt8 *)&info->pending_addr;
            info->pending_call.addr.maxlen = sizeof(InetAddress);
            err = OTListen(info->endpoint, &info->pending_call);
            if (err == noErr) {
                info->has_pending = 1;
            }
        }
    }

    if (!info->has_pending) {
        console_poll_events();
        errno = EAGAIN;
        return -1;
    }

    new_fd = ot_alloc_stream_slot();
    if (new_fd < 0) {
        OTSndDisconnect(info->endpoint, &info->pending_call);
        info->has_pending = 0;
        errno = EMFILE;
        return -1;
    }

    new_info = &g_ot_streams[new_fd - FD_BASE];
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
        new_info->in_use = 0;
        OTSndDisconnect(info->endpoint, &info->pending_call);
        info->has_pending = 0;
        errno = EMFILE;
        return -1;
    }

    OTSetSynchronous(new_info->endpoint);
    OTSetNonBlocking(new_info->endpoint);

    err = OTBind(new_info->endpoint, NULL, NULL);
    if (err != noErr) {
        OTCloseProvider(new_info->endpoint);
        new_info->in_use = 0;
        OTSndDisconnect(info->endpoint, &info->pending_call);
        info->has_pending = 0;
        errno = EMFILE;
        return -1;
    }

    err = OTAccept(info->endpoint, new_info->endpoint, &info->pending_call);
    if (err != noErr) {
        OTUnbind(new_info->endpoint);
        OTCloseProvider(new_info->endpoint);
        new_info->in_use = 0;
        info->has_pending = 0;
        errno = EAGAIN;
        return -1;
    }

    info->has_pending = 0;
    new_info->is_connected = 1;
    memcpy(&new_info->remote_addr, &info->pending_addr, sizeof(InetAddress));
    ot_configure_endpoint_for_performance(new_info->endpoint);

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

static ssize_t ot_send(int sockfd, const void *buf, size_t len) {
    int idx = ot_fd_to_index(sockfd);
    OTStreamInfo *info;
    OTResult result;

    if (idx < 0 || !g_ot_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_ot_streams[idx];
    if (!info->is_connected) {
        errno = ENOTCONN;
        return -1;
    }

    result = OTSnd(info->endpoint, (void *)buf, len, 0);
    if (result >= 0) return (ssize_t)result;

    if (result == kOTLookErr) {
        ot_handle_disconnect_event(info, OTLook(info->endpoint));
        errno = ECONNRESET;
        return -1;
    }
    if (result == kOTFlowErr) {
        errno = EAGAIN;
        return -1;
    }

    errno = ECONNRESET;
    return -1;
}

static ssize_t ot_recv(int sockfd, void *buf, size_t len, int flags) {
    int idx = ot_fd_to_index(sockfd);
    OTStreamInfo *info;
    OTResult result;
    OTFlags ot_flags = 0;
    OTByteCount avail;
    size_t copy_len;

    if (idx < 0 || !g_ot_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_ot_streams[idx];
    if (!info->is_connected) {
        errno = ENOTCONN;
        return -1;
    }

    if (flags & MSG_PEEK) {
        if (info->peek_len >= (int)len) {
            memcpy(buf, info->peek_buf, len);
            return (ssize_t)len;
        }
        if (info->peek_len < PEEK_BUFFER_SIZE) {
            if (OTCountDataBytes(info->endpoint, &avail) == noErr && avail > 0) {
                size_t to_read = PEEK_BUFFER_SIZE - info->peek_len;
                if (to_read > (size_t)avail) to_read = (size_t)avail;
                result = OTRcv(info->endpoint, info->peek_buf + info->peek_len, to_read, &ot_flags);
                if (result > 0) {
                    info->peek_len += result;
                } else if (result == kOTLookErr) {
                    if (ot_handle_disconnect_event(info, OTLook(info->endpoint))) {
                        return 0;
                    }
                }
            }
        }
        if (info->peek_len > 0) {
            copy_len = ((size_t)info->peek_len < len) ? (size_t)info->peek_len : len;
            memcpy(buf, info->peek_buf, copy_len);
            return (ssize_t)copy_len;
        }
        errno = EAGAIN;
        return -1;
    }

    if (info->peek_len > 0) {
        copy_len = ((size_t)info->peek_len < len) ? (size_t)info->peek_len : len;
        memcpy(buf, info->peek_buf, copy_len);
        if (copy_len < (size_t)info->peek_len) {
            memmove(info->peek_buf, info->peek_buf + copy_len, info->peek_len - copy_len);
        }
        info->peek_len -= copy_len;
        return (ssize_t)copy_len;
    }

    if (OTCountDataBytes(info->endpoint, &avail) == noErr && avail == 0) {
        OTResult look = OTLook(info->endpoint);
        if (look == T_DISCONNECT || look == T_ORDREL) {
            ot_handle_disconnect_event(info, look);
            return 0;
        }
        errno = EAGAIN;
        return -1;
    }

    result = OTRcv(info->endpoint, buf, len, &ot_flags);
    if (result > 0) return (ssize_t)result;
    if (result == 0) return 0;
    if (result == kOTNoDataErr) {
        errno = EAGAIN;
        return -1;
    }
    if (result == kOTLookErr) {
        if (ot_handle_disconnect_event(info, OTLook(info->endpoint))) {
            return 0;
        }
    }
    errno = ECONNRESET;
    return -1;
}

static int ot_close(int fd) {
    int idx = ot_fd_to_index(fd);
    OTStreamInfo *info;

    if (idx < 0 || !g_ot_streams[idx].in_use) return 0;
    info = &g_ot_streams[idx];

    if (info->endpoint != kOTInvalidEndpointRef) {
        if (info->is_connected && !info->orderly_disconnect_sent) {
            OTSndOrderlyDisconnect(info->endpoint);
            info->orderly_disconnect_sent = 1;
            ot_drain_endpoint(info->endpoint);
        }
        OTUnbind(info->endpoint);
        OTCloseProvider(info->endpoint);
    }
    info->in_use = 0;
    return 0;
}

static int ot_shutdown(int sockfd, int how) {
    int idx = ot_fd_to_index(sockfd);
    OTStreamInfo *info;
    (void)how;

    if (idx < 0 || !g_ot_streams[idx].in_use) return 0;
    info = &g_ot_streams[idx];

    if (info->is_connected && info->endpoint != kOTInvalidEndpointRef) {
        if (!info->orderly_disconnect_sent) {
            OTSndOrderlyDisconnect(info->endpoint);
            info->orderly_disconnect_sent = 1;
        }
        info->is_connected = 0;
    }
    return 0;
}

static void ot_cleanup(void) {
    int i;
    if (!g_ot_initialized) return;

    for (i = 0; i < MAX_STREAMS; i++) {
        if (g_ot_streams[i].in_use && g_ot_streams[i].endpoint != kOTInvalidEndpointRef) {
            if (g_ot_streams[i].is_connected && !g_ot_streams[i].orderly_disconnect_sent) {
                OTSndOrderlyDisconnect(g_ot_streams[i].endpoint);
                ot_drain_endpoint(g_ot_streams[i].endpoint);
            }
            OTUnbind(g_ot_streams[i].endpoint);
            OTCloseProvider(g_ot_streams[i].endpoint);
        }
        g_ot_streams[i].in_use = 0;
    }
    CloseOpenTransport();
    g_ot_initialized = 0;
    console_print("Open Transport closed.\r");
}

/* ============================================================================
 * MACTCP IMPLEMENTATION
 * ============================================================================ */

#define STREAM_BUFFER_SIZE 4096

typedef struct {
    unsigned long stream;
    int in_use;
    int is_listener;
    int is_connected;
    int is_async_pending;
    TCPiopb *async_pb;
    short local_port;
    long remote_host;
    short remote_port;
    bool cancel_flag;
    unsigned char peek_buf[PEEK_BUFFER_SIZE];
    int peek_len;
} MacTCPStreamInfo;

static MacTCPStreamInfo g_mactcp_streams[MAX_STREAMS];

static void mactcp_give_time_callback(void) {
    console_poll_events();
}

static int mactcp_fd_to_index(int fd) {
    int idx = fd - FD_BASE;
    if (idx < 0 || idx >= MAX_STREAMS) return -1;
    return idx;
}

static int mactcp_alloc_stream_slot(void) {
    int i;
    for (i = 0; i < MAX_STREAMS; i++) {
        if (!g_mactcp_streams[i].in_use) {
            memset(&g_mactcp_streams[i], 0, sizeof(MacTCPStreamInfo));
            g_mactcp_streams[i].in_use = 1;
            g_mactcp_streams[i].cancel_flag = false;
            return i + FD_BASE;
        }
    }
    return -1;
}

static int mactcp_init(void) {
    OSErr err = InitNetwork();
    if (err != noErr) {
        console_printf("ERROR: InitNetwork failed: %d\r", err);
        return -1;
    }
    memset(g_mactcp_streams, 0, sizeof(g_mactcp_streams));
    g_mactcp_initialized = 1;
    console_print("MacTCP initialized.\r");
    return 0;
}

static int mactcp_socket(void) {
    int fd;
    MacTCPStreamInfo *info;
    OSErr err;

    fd = mactcp_alloc_stream_slot();
    if (fd < 0) {
        errno = EMFILE;
        return -1;
    }

    info = &g_mactcp_streams[fd - FD_BASE];
    err = CreateStream(&info->stream, STREAM_BUFFER_SIZE,
                       (GiveTimePtr)mactcp_give_time_callback, &info->cancel_flag);
    if (err != noErr) {
        info->in_use = 0;
        errno = ENOTSOCK;
        return -1;
    }

    return fd;
}

static int mactcp_bind(int sockfd, const struct sockaddr_in *sin) {
    int idx = mactcp_fd_to_index(sockfd);
    if (idx < 0 || !g_mactcp_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }
    g_mactcp_streams[idx].local_port = sin->sin_port;
    return 0;
}

static int mactcp_listen(int sockfd) {
    int idx = mactcp_fd_to_index(sockfd);
    MacTCPStreamInfo *info;

    if (idx < 0 || !g_mactcp_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_mactcp_streams[idx];
    info->is_listener = 1;

    AsyncWaitForConnection(info->stream, 0, info->local_port, 0, 0,
                           &info->async_pb, (GiveTimePtr)mactcp_give_time_callback,
                           &info->cancel_flag);
    info->is_async_pending = 1;

    return 0;
}

static int mactcp_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int idx = mactcp_fd_to_index(sockfd);
    MacTCPStreamInfo *info, *new_info;
    int new_fd;
    OSErr err;

    if (idx < 0 || !g_mactcp_streams[idx].in_use || !g_mactcp_streams[idx].is_listener) {
        errno = EBADF;
        return -1;
    }

    info = &g_mactcp_streams[idx];

    if (!info->is_async_pending || info->async_pb == NULL) {
        errno = EAGAIN;
        return -1;
    }

    if (info->async_pb->ioResult > 0) {
        mactcp_give_time_callback();
        errno = EAGAIN;
        return -1;
    }

    if (info->async_pb->ioResult != noErr) {
        info->is_async_pending = 0;
        AsyncWaitForConnection(info->stream, 0, info->local_port, 0, 0,
                               &info->async_pb, (GiveTimePtr)mactcp_give_time_callback,
                               &info->cancel_flag);
        info->is_async_pending = 1;
        errno = EAGAIN;
        return -1;
    }

    err = AsyncGetConnectionData(info->async_pb, &info->remote_host, &info->remote_port);
    info->is_async_pending = 0;

    if (err != noErr) {
        AsyncWaitForConnection(info->stream, 0, info->local_port, 0, 0,
                               &info->async_pb, (GiveTimePtr)mactcp_give_time_callback,
                               &info->cancel_flag);
        info->is_async_pending = 1;
        errno = EAGAIN;
        return -1;
    }

    new_fd = mactcp_alloc_stream_slot();
    if (new_fd < 0) {
        CloseConnection(info->stream, (GiveTimePtr)mactcp_give_time_callback, &info->cancel_flag);
        ReleaseStream(info->stream, (GiveTimePtr)mactcp_give_time_callback, &info->cancel_flag);
        err = CreateStream(&info->stream, STREAM_BUFFER_SIZE,
                           (GiveTimePtr)mactcp_give_time_callback, &info->cancel_flag);
        if (err == noErr) {
            AsyncWaitForConnection(info->stream, 0, info->local_port, 0, 0,
                                   &info->async_pb, (GiveTimePtr)mactcp_give_time_callback,
                                   &info->cancel_flag);
            info->is_async_pending = 1;
        }
        errno = EMFILE;
        return -1;
    }

    new_info = &g_mactcp_streams[new_fd - FD_BASE];
    new_info->stream = info->stream;
    new_info->is_connected = 1;
    new_info->remote_host = info->remote_host;
    new_info->remote_port = info->remote_port;

    if (addr != NULL && addrlen != NULL) {
        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
        sin->sin_family = AF_INET;
        sin->sin_port = info->remote_port;
        sin->sin_addr.s_addr = info->remote_host;
        *addrlen = sizeof(struct sockaddr_in);
    }

    err = CreateStream(&info->stream, STREAM_BUFFER_SIZE,
                       (GiveTimePtr)mactcp_give_time_callback, &info->cancel_flag);
    if (err == noErr) {
        AsyncWaitForConnection(info->stream, 0, info->local_port, 0, 0,
                               &info->async_pb, (GiveTimePtr)mactcp_give_time_callback,
                               &info->cancel_flag);
        info->is_async_pending = 1;
    } else {
        info->in_use = 0;
    }

    console_print("Client connected!\r");
    return new_fd;
}

static ssize_t mactcp_send(int sockfd, const void *buf, size_t len) {
    int idx = mactcp_fd_to_index(sockfd);
    MacTCPStreamInfo *info;
    OSErr err;

    if (idx < 0 || !g_mactcp_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_mactcp_streams[idx];
    if (!info->is_connected) {
        errno = ENOTCONN;
        return -1;
    }

    err = SendData(info->stream, (Ptr)buf, (unsigned short)len, false,
                   (GiveTimePtr)mactcp_give_time_callback, &info->cancel_flag);

    if (err != noErr) {
        if (err == commandTimeout) {
            errno = EAGAIN;
            return -1;
        }
        errno = ECONNRESET;
        return -1;
    }

    return (ssize_t)len;
}

static ssize_t mactcp_recv(int sockfd, void *buf, size_t len, int flags) {
    int idx = mactcp_fd_to_index(sockfd);
    MacTCPStreamInfo *info;
    OSErr err;
    unsigned short recv_len;
    size_t copy_len;

    if (idx < 0 || !g_mactcp_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_mactcp_streams[idx];
    if (!info->is_connected) {
        errno = ENOTCONN;
        return -1;
    }

    if (flags & MSG_PEEK) {
        if (info->peek_len >= (int)len) {
            memcpy(buf, info->peek_buf, len);
            return (ssize_t)len;
        }
        if (info->peek_len < PEEK_BUFFER_SIZE) {
            recv_len = (unsigned short)(PEEK_BUFFER_SIZE - info->peek_len);
            err = RecvData(info->stream, (Ptr)(info->peek_buf + info->peek_len),
                           &recv_len, false, (GiveTimePtr)mactcp_give_time_callback,
                           &info->cancel_flag);
            if (err == noErr && recv_len > 0) {
                info->peek_len += recv_len;
            } else if (err != noErr && err != commandTimeout) {
                if (err == connectionClosing || err == connectionTerminated) {
                    return 0;
                }
            }
        }
        if (info->peek_len > 0) {
            copy_len = (info->peek_len < (int)len) ? info->peek_len : len;
            memcpy(buf, info->peek_buf, copy_len);
            return (ssize_t)copy_len;
        }
        errno = EAGAIN;
        return -1;
    }

    if (info->peek_len > 0) {
        copy_len = (info->peek_len < (int)len) ? info->peek_len : len;
        memcpy(buf, info->peek_buf, copy_len);
        if (copy_len < info->peek_len) {
            memmove(info->peek_buf, info->peek_buf + copy_len, info->peek_len - copy_len);
        }
        info->peek_len -= copy_len;
        return (ssize_t)copy_len;
    }

    recv_len = (unsigned short)len;
    err = RecvData(info->stream, (Ptr)buf, &recv_len, false,
                   (GiveTimePtr)mactcp_give_time_callback, &info->cancel_flag);

    if (err != noErr) {
        if (err == commandTimeout) {
            errno = EAGAIN;
            return -1;
        }
        if (err == connectionClosing || err == connectionTerminated) {
            return 0;
        }
        errno = ECONNRESET;
        return -1;
    }

    if (recv_len == 0) {
        errno = EAGAIN;
        return -1;
    }

    return (ssize_t)recv_len;
}

static int mactcp_close(int fd) {
    int idx = mactcp_fd_to_index(fd);
    MacTCPStreamInfo *info;

    if (idx < 0 || !g_mactcp_streams[idx].in_use) return 0;
    info = &g_mactcp_streams[idx];

    if (info->stream != 0) {
        if (info->is_connected) {
            CloseConnection(info->stream, (GiveTimePtr)mactcp_give_time_callback, &info->cancel_flag);
        }
        ReleaseStream(info->stream, (GiveTimePtr)mactcp_give_time_callback, &info->cancel_flag);
        info->stream = 0;
    }
    info->in_use = 0;
    return 0;
}

static int mactcp_shutdown(int sockfd, int how) {
    int idx = mactcp_fd_to_index(sockfd);
    MacTCPStreamInfo *info;
    (void)how;

    if (idx < 0 || !g_mactcp_streams[idx].in_use) return 0;
    info = &g_mactcp_streams[idx];

    if (info->is_connected && info->stream != 0) {
        CloseConnection(info->stream, (GiveTimePtr)mactcp_give_time_callback, &info->cancel_flag);
        info->is_connected = 0;
    }
    return 0;
}

static void mactcp_cleanup(void) {
    int i;
    if (!g_mactcp_initialized) return;

    for (i = 0; i < MAX_STREAMS; i++) {
        if (g_mactcp_streams[i].in_use && g_mactcp_streams[i].stream != 0) {
            if (g_mactcp_streams[i].is_connected) {
                CloseConnection(g_mactcp_streams[i].stream,
                                (GiveTimePtr)mactcp_give_time_callback,
                                &g_mactcp_streams[i].cancel_flag);
            }
            ReleaseStream(g_mactcp_streams[i].stream,
                          (GiveTimePtr)mactcp_give_time_callback,
                          &g_mactcp_streams[i].cancel_flag);
        }
        g_mactcp_streams[i].in_use = 0;
    }
    g_mactcp_initialized = 0;
    console_print("MacTCP closed.\r");
}

/* ============================================================================
 * PUBLIC API - Dispatches to OT or MacTCP based on availability
 * ============================================================================ */

/* Check for Open Transport availability */
static int check_open_transport_available(void) {
    long response;
    OSErr err = Gestalt(gestaltOpenTpt, &response);
    if (err == noErr && (response & gestaltOpenTptPresentMask)) {
        return 1;
    }
    return 0;
}

/* Initialize networking - uses user choice or auto-detects */
static int init_networking(void) {
    int use_ot;

    if (g_net_initialized) return 0;

    /* Check if user made an explicit choice */
    if (g_user_stack_choice >= 0) {
        use_ot = g_user_stack_choice;
        console_printf("Using %s (user selected)\r", use_ot ? "Open Transport" : "MacTCP");
    } else {
        /* Auto-detect */
        console_print("Detecting network stack...\r");
        if (check_open_transport_available()) {
            console_print("Open Transport detected.\r");
            use_ot = 1;
        } else {
            console_print("Open Transport not found, using MacTCP.\r");
            use_ot = 0;
        }
    }

    /* Initialize the selected stack */
    if (use_ot) {
        if (ot_init() < 0) {
            console_print("OT init failed, trying MacTCP...\r");
            use_ot = 0;
            if (mactcp_init() < 0) {
                return -1;
            }
        }
    } else {
        if (mactcp_init() < 0) {
            return -1;
        }
    }

    g_use_open_transport = use_ot;
    g_net_initialized = 1;
    return 0;
}

int socket(int domain, int type, int protocol) {
    (void)domain; (void)protocol;

    if (!g_net_initialized) {
        if (init_networking() < 0) {
            errno = ENOTSOCK;
            return -1;
        }
    }

    if (type != SOCK_STREAM) {
        errno = ENOTSOCK;
        return -1;
    }

    return g_use_open_transport ? ot_socket() : mactcp_socket();
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    (void)addrlen;
    console_printf("Bound to port %u\r", (unsigned)sin->sin_port);
    return g_use_open_transport ? ot_bind(sockfd, sin) : mactcp_bind(sockfd, sin);
}

int listen(int sockfd, int backlog) {
    int idx = sockfd - FD_BASE;
    (void)backlog;
    if (g_use_open_transport) {
        console_printf("Listening on port %u...\r", (unsigned)g_ot_streams[idx].local_addr.fPort);
    } else {
        console_printf("Listening on port %u...\r", (unsigned)g_mactcp_streams[idx].local_port);
    }
    return g_use_open_transport ? ot_listen(sockfd) : mactcp_listen(sockfd);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return g_use_open_transport ? ot_accept(sockfd, addr, addrlen) : mactcp_accept(sockfd, addr, addrlen);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    (void)flags;
    return g_use_open_transport ? ot_send(sockfd, buf, len) : mactcp_send(sockfd, buf, len);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return g_use_open_transport ? ot_recv(sockfd, buf, len, flags) : mactcp_recv(sockfd, buf, len, flags);
}

int close(int fd) {
    return g_use_open_transport ? ot_close(fd) : mactcp_close(fd);
}

int shutdown(int sockfd, int how) {
    return g_use_open_transport ? ot_shutdown(sockfd, how) : mactcp_shutdown(sockfd, how);
}

void cleanup_open_transport(void) {
    if (!g_net_initialized) return;
    if (g_use_open_transport) {
        ot_cleanup();
    } else {
        mactcp_cleanup();
    }
    g_net_initialized = 0;
}

void cleanup_mactcp(void) {
    cleanup_open_transport();
}

/* Common utility functions */

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    /* Minimal implementation - server rarely needs outbound connections */
    (void)sockfd; (void)addr; (void)addrlen;
    errno = ECONNREFUSED;
    return -1;
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    (void)sockfd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

int fcntl(int fd, int cmd, ...) {
    (void)fd;
    if (cmd == F_GETFL) return O_NONBLOCK;
    return 0;
}

uint16_t htons(uint16_t hostshort) { return hostshort; }
uint32_t htonl(uint32_t hostlong) { return hostlong; }
uint16_t ntohs(uint16_t netshort) { return netshort; }
uint32_t ntohl(uint32_t netlong) { return netlong; }

void task_yield(void) {
    console_poll_events();
}

/* Query and control networking stack selection */

int net_is_open_transport_available(void) {
    return check_open_transport_available();
}

int net_is_using_open_transport(void) {
    return g_use_open_transport;
}

int net_get_selected_stack(void) {
    /* Returns user's selection, or current if no selection made */
    if (g_user_stack_choice >= 0) {
        return g_user_stack_choice;
    }
    return g_use_open_transport;
}

int net_set_stack(int use_ot) {
    if (use_ot && !check_open_transport_available()) {
        console_print("Open Transport not available on this system.\r");
        return -1;
    }

    g_user_stack_choice = use_ot;
    console_printf("Network stack set to: %s\r", use_ot ? "Open Transport" : "MacTCP");

    if (g_net_initialized && g_use_open_transport != use_ot) {
        console_print("Use 'Restart Server' to apply change.\r");
    }
    return 0;
}

void net_shutdown(void) {
    if (!g_net_initialized) {
        console_print("Server not running.\r");
        return;
    }

    console_print("Stopping server...\r");

    /* Close all connections using the appropriate cleanup */
    if (g_use_open_transport) {
        ot_cleanup();
    } else {
        mactcp_cleanup();
    }

    g_net_initialized = 0;
    g_net_restart_needed = 1;
    console_print("Server stopped. Select network stack, then restart.\r");
}

int net_needs_restart(void) {
    return g_net_restart_needed;
}

void net_clear_restart(void) {
    g_net_restart_needed = 0;
}

#endif /* MAC68K_PLATFORM */
