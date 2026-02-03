/*
 * mac68k_net.c - Networking for 68k Macintosh using MacTCP
 *
 * This file provides BSD socket-like functions implemented on top of
 * MacTCP networking stack via MacTCPHelper library.
 *
 * Requirements:
 *   - Mac OS with MacTCP installed (System 7+)
 *   - MacTCPHelper library
 */

#ifdef MAC68K_PLATFORM

#include <MacTCP.h>
#include <string.h>

#include <mactcp/TCPHi.h>
#include <mactcp/CvtAddr.h>
#include <mactcp/TCPRoutines.h>
#include "mac68k_net.h"
#include "mac68k_console.h"
#include "globals.h"

/* Global errno for error reporting */
int errno = 0;

/* Maximum number of connections we can track */
#define MAX_STREAMS 34  /* 1 listener + MAX_PLAYERS clients + margin */

/* MacTCP stream receive buffer size */
#define STREAM_BUFFER_SIZE 4096

/* Peek buffer size - enough for protocol header checks */
#define PEEK_BUFFER_SIZE 16

/* Stream state */
typedef struct {
    unsigned long stream;       /* MacTCP stream identifier */
    int in_use;                 /* Is this slot in use? */
    int is_listener;            /* Is this a listening socket? */
    int is_connected;           /* Is connection established? */
    int is_async_pending;       /* Async operation in progress? */
    TCPiopb *async_pb;          /* Parameter block for async ops */
    short local_port;           /* Port we're listening on (for listener) */
    long remote_host;           /* Remote host address */
    short remote_port;          /* Remote port */
    bool cancel_flag;           /* Cancel flag for async operations */
    /* Peek buffer for MSG_PEEK support (MacTCP doesn't have native peek) */
    unsigned char peek_buf[PEEK_BUFFER_SIZE];
    int peek_len;               /* How many bytes in peek buffer */
} MacTCPStreamInfo;

/* File descriptor to stream mapping */
/* FD 0-2 reserved for stdin/stdout/stderr, start at 3 */
#define FD_BASE 3
static MacTCPStreamInfo g_streams[MAX_STREAMS];
static int g_mactcp_initialized = 0;

/* GiveTime callback for cooperative multitasking */
static void give_time_callback(void)
{
    console_poll_events();
}

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
            memset(&g_streams[i], 0, sizeof(MacTCPStreamInfo));
            g_streams[i].in_use = 1;
            g_streams[i].cancel_flag = false;
            return i + FD_BASE;
        }
    }
    return -1;
}

/* Initialize MacTCP - called automatically on first socket() */
static int init_mactcp(void) {
    OSErr err;

    if (g_mactcp_initialized) return 0;

    console_print("Initializing MacTCP...\r");

    err = InitNetwork();
    if (err != noErr) {
        console_printf("ERROR: InitNetwork failed: %d\r", err);
        console_print("MacTCP may not be installed.\r");
        return -1;
    }

    memset(g_streams, 0, sizeof(g_streams));
    g_mactcp_initialized = 1;
    console_print("MacTCP initialized.\r");
    return 0;
}

int socket(int domain, int type, int protocol) {
    OSErr err;
    int fd;
    MacTCPStreamInfo *info;

    (void)domain; (void)protocol;

    if (!g_mactcp_initialized) {
        if (init_mactcp() < 0) {
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

    /* Create the TCP stream */
    err = CreateStream(&info->stream, STREAM_BUFFER_SIZE,
                       (GiveTimePtr)give_time_callback, &info->cancel_flag);
    if (err != noErr) {
        console_printf("ERROR: CreateStream failed: %d\r", err);
        info->in_use = 0;
        errno = ENOTSOCK;
        return -1;
    }

    info->is_listener = 0;
    info->is_connected = 0;
    info->is_async_pending = 0;

    return fd;
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int idx;
    MacTCPStreamInfo *info;
    const struct sockaddr_in *sin;

    (void)addrlen;

    idx = fd_to_index(sockfd);
    if (idx < 0 || !g_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_streams[idx];
    sin = (const struct sockaddr_in *)addr;

    /* MacTCP doesn't have explicit bind - just store the port for listen */
    info->local_port = sin->sin_port;

    console_printf("Bound to port %u\r", (unsigned)sin->sin_port);
    return 0;
}

int listen(int sockfd, int backlog) {
    int idx;
    MacTCPStreamInfo *info;

    (void)backlog;

    idx = fd_to_index(sockfd);
    if (idx < 0 || !g_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_streams[idx];
    info->is_listener = 1;

    /* Start async listen for first connection */
    AsyncWaitForConnection(
        info->stream,
        0,  /* timeout: 0 = no timeout */
        info->local_port,
        0,  /* any remote host */
        0,  /* any remote port */
        &info->async_pb,
        (GiveTimePtr)give_time_callback,
        &info->cancel_flag
    );
    info->is_async_pending = 1;

    console_printf("Listening on port %u...\r", (unsigned)info->local_port);
    return 0;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int idx;
    MacTCPStreamInfo *info;
    int new_fd;
    MacTCPStreamInfo *new_info;
    OSErr err;

    idx = fd_to_index(sockfd);
    if (idx < 0 || !g_streams[idx].in_use || !g_streams[idx].is_listener) {
        errno = EBADF;
        return -1;
    }

    info = &g_streams[idx];

    /* Check if async listen has completed */
    if (!info->is_async_pending || info->async_pb == NULL) {
        errno = EAGAIN;
        return -1;
    }

    /* Check if the async operation is still in progress */
    if (info->async_pb->ioResult > 0) {
        /* Still waiting - give time and return EAGAIN */
        give_time_callback();
        errno = EAGAIN;
        return -1;
    }

    /* Operation completed - check result */
    if (info->async_pb->ioResult != noErr) {
        /* Error or timeout - restart the listen */
        info->is_async_pending = 0;
        AsyncWaitForConnection(
            info->stream,
            0,
            info->local_port,
            0, 0,
            &info->async_pb,
            (GiveTimePtr)give_time_callback,
            &info->cancel_flag
        );
        info->is_async_pending = 1;
        errno = EAGAIN;
        return -1;
    }

    /* Connection established! Get connection data */
    err = AsyncGetConnectionData(info->async_pb, &info->remote_host, &info->remote_port);
    info->is_async_pending = 0;

    if (err != noErr) {
        /* Restart listen */
        AsyncWaitForConnection(
            info->stream,
            0,
            info->local_port,
            0, 0,
            &info->async_pb,
            (GiveTimePtr)give_time_callback,
            &info->cancel_flag
        );
        info->is_async_pending = 1;
        errno = EAGAIN;
        return -1;
    }

    /* Allocate a new fd for this connection */
    new_fd = alloc_stream_slot();
    if (new_fd < 0) {
        /* No slots available - close connection and restart listen */
        CloseConnection(info->stream, (GiveTimePtr)give_time_callback, &info->cancel_flag);
        ReleaseStream(info->stream, (GiveTimePtr)give_time_callback, &info->cancel_flag);

        /* Re-create the listener stream */
        err = CreateStream(&info->stream, STREAM_BUFFER_SIZE,
                           (GiveTimePtr)give_time_callback, &info->cancel_flag);
        if (err == noErr) {
            AsyncWaitForConnection(
                info->stream,
                0,
                info->local_port,
                0, 0,
                &info->async_pb,
                (GiveTimePtr)give_time_callback,
                &info->cancel_flag
            );
            info->is_async_pending = 1;
        }
        errno = EMFILE;
        return -1;
    }

    /* Transfer the connected stream to the new fd */
    new_info = &g_streams[new_fd - FD_BASE];
    new_info->stream = info->stream;
    new_info->is_connected = 1;
    new_info->is_listener = 0;
    new_info->remote_host = info->remote_host;
    new_info->remote_port = info->remote_port;

    /* Fill in the address if requested */
    if (addr != NULL && addrlen != NULL) {
        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
        sin->sin_family = AF_INET;
        sin->sin_port = info->remote_port;
        sin->sin_addr.s_addr = info->remote_host;
        *addrlen = sizeof(struct sockaddr_in);
    }

    /* Create a new stream for the listener to continue listening */
    err = CreateStream(&info->stream, STREAM_BUFFER_SIZE,
                       (GiveTimePtr)give_time_callback, &info->cancel_flag);
    if (err != noErr) {
        console_printf("ERROR: CreateStream for listener failed: %d\r", err);
        /* Listener is broken, but we can still return the accepted connection */
        info->in_use = 0;
    } else {
        /* Start listening again */
        AsyncWaitForConnection(
            info->stream,
            0,
            info->local_port,
            0, 0,
            &info->async_pb,
            (GiveTimePtr)give_time_callback,
            &info->cancel_flag
        );
        info->is_async_pending = 1;
    }

    console_print("Client connected!\r");
    return new_fd;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    int idx;
    MacTCPStreamInfo *info;
    OSErr err;

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

    /* MacTCP SendData - use non-retry mode for non-blocking behavior */
    err = SendData(
        info->stream,
        (Ptr)buf,
        (unsigned short)len,
        false,  /* don't retry on timeout */
        (GiveTimePtr)give_time_callback,
        &info->cancel_flag
    );

    if (err != noErr) {
        if (err == commandTimeout) {
            errno = EAGAIN;
            return -1;
        }
        console_printf("SendData error: %d\r", err);
        errno = ECONNRESET;
        return -1;
    }

    return (ssize_t)len;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    int idx;
    MacTCPStreamInfo *info;
    OSErr err;
    unsigned short recv_len;
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

        /* Need to read more data into peek buffer */
        if (info->peek_len < PEEK_BUFFER_SIZE) {
            recv_len = (unsigned short)(PEEK_BUFFER_SIZE - info->peek_len);
            if (recv_len > len) recv_len = (unsigned short)len;

            err = RecvData(
                info->stream,
                (Ptr)(info->peek_buf + info->peek_len),
                &recv_len,
                false,
                (GiveTimePtr)give_time_callback,
                &info->cancel_flag
            );

            if (err == noErr && recv_len > 0) {
                info->peek_len += recv_len;
            } else if (err != noErr && err != commandTimeout) {
                if (err == connectionClosing || err == connectionTerminated) {
                    return 0;
                }
                errno = ECONNRESET;
                return -1;
            }
        }

        /* Return what we have in peek buffer */
        if (info->peek_len > 0) {
            copy_len = (info->peek_len < (int)len) ? info->peek_len : len;
            memcpy(buf, info->peek_buf, copy_len);
            return (ssize_t)copy_len;
        }

        errno = EAGAIN;
        return -1;
    }

    /* Normal recv (not peek) - first drain peek buffer */
    if (info->peek_len > 0) {
        copy_len = (info->peek_len < (int)len) ? info->peek_len : len;
        memcpy(buf, info->peek_buf, copy_len);

        /* Shift remaining peek data */
        if (copy_len < info->peek_len) {
            memmove(info->peek_buf, info->peek_buf + copy_len, info->peek_len - copy_len);
        }
        info->peek_len -= copy_len;

        return (ssize_t)copy_len;
    }

    /* No peek data, read from network */
    recv_len = (unsigned short)len;

    err = RecvData(
        info->stream,
        (Ptr)buf,
        &recv_len,
        false,
        (GiveTimePtr)give_time_callback,
        &info->cancel_flag
    );

    if (err != noErr) {
        if (err == commandTimeout) {
            errno = EAGAIN;
            return -1;
        }
        if (err == connectionClosing || err == connectionTerminated) {
            return 0;
        }
        console_printf("RecvData error: %d\r", err);
        errno = ECONNRESET;
        return -1;
    }

    if (recv_len == 0) {
        errno = EAGAIN;
        return -1;
    }

    return (ssize_t)recv_len;
}

int close(int fd) {
    int idx;
    MacTCPStreamInfo *info;

    idx = fd_to_index(fd);
    if (idx < 0 || !g_streams[idx].in_use) {
        return 0;
    }

    info = &g_streams[idx];

    if (info->stream != 0) {
        if (info->is_connected) {
            CloseConnection(info->stream,
                            (GiveTimePtr)give_time_callback, &info->cancel_flag);
        }
        ReleaseStream(info->stream,
                      (GiveTimePtr)give_time_callback, &info->cancel_flag);
        info->stream = 0;
    }

    info->in_use = 0;
    return 0;
}

int shutdown(int sockfd, int how) {
    int idx;
    MacTCPStreamInfo *info;

    (void)how;

    idx = fd_to_index(sockfd);
    if (idx < 0 || !g_streams[idx].in_use) {
        return 0;
    }

    info = &g_streams[idx];
    if (info->is_connected && info->stream != 0) {
        CloseConnection(info->stream,
                        (GiveTimePtr)give_time_callback, &info->cancel_flag);
        info->is_connected = 0;
    }

    return 0;
}

/* Cleanup MacTCP - call on exit */
void cleanup_mactcp(void) {
    int i;
    if (g_mactcp_initialized) {
        for (i = 0; i < MAX_STREAMS; i++) {
            if (g_streams[i].in_use && g_streams[i].stream != 0) {
                if (g_streams[i].is_connected) {
                    CloseConnection(g_streams[i].stream,
                                    (GiveTimePtr)give_time_callback,
                                    &g_streams[i].cancel_flag);
                }
                ReleaseStream(g_streams[i].stream,
                              (GiveTimePtr)give_time_callback,
                              &g_streams[i].cancel_flag);
            }
        }
        g_mactcp_initialized = 0;
        console_print("MacTCP closed.\r");
    }
}

/* Alias for cleanup - keep compatibility with old OT code */
void cleanup_open_transport(void) {
    cleanup_mactcp();
}

/* Common functions */

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int idx;
    MacTCPStreamInfo *info;
    const struct sockaddr_in *sin;
    OSErr err;

    (void)addrlen;

    idx = fd_to_index(sockfd);
    if (idx < 0 || !g_streams[idx].in_use) {
        errno = EBADF;
        return -1;
    }

    info = &g_streams[idx];
    sin = (const struct sockaddr_in *)addr;

    err = OpenConnection(
        info->stream,
        sin->sin_addr.s_addr,
        sin->sin_port,
        30,  /* 30 second timeout */
        (GiveTimePtr)give_time_callback,
        &info->cancel_flag
    );

    if (err != noErr) {
        console_printf("OpenConnection failed: %d\r", err);
        errno = ECONNREFUSED;
        return -1;
    }

    info->is_connected = 1;
    info->remote_host = sin->sin_addr.s_addr;
    info->remote_port = sin->sin_port;

    return 0;
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    (void)sockfd; (void)level; (void)optname; (void)optval; (void)optlen;
    /* MacTCP doesn't support most socket options - just succeed */
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
