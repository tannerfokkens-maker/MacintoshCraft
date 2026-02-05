/*
 * mac68k_net.h - Open Transport networking for 68k Macintosh
 *
 * Provides BSD socket-like API for classic Mac OS using Open Transport.
 */

#ifndef MAC68K_NET_H
#define MAC68K_NET_H

#ifdef MAC68K_PLATFORM

#include <stdint.h>
#include <stddef.h>

/* ssize_t is not available in classic Mac toolchains */
typedef long ssize_t;

/* Socket types */
#define AF_INET       2
#define SOCK_STREAM   1
#define IPPROTO_TCP   6

#define SOL_SOCKET    0xFFFF
#define SO_REUSEADDR  0x0004

#define INADDR_ANY    0x00000000

#define MSG_PEEK      0x02
#define MSG_NOSIGNAL  0x4000

#ifndef O_NONBLOCK
#define O_NONBLOCK    0x0004
#endif
#ifndef F_GETFL
#define F_GETFL       3
#endif
#ifndef F_SETFL
#define F_SETFL       4
#endif

/* Shutdown constants */
#ifndef SHUT_RD
#define SHUT_RD       0
#endif
#ifndef SHUT_WR
#define SHUT_WR       1
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR     2
#endif

/* Error codes */
#define EAGAIN        35
#define EWOULDBLOCK   EAGAIN
#define EINTR         4
#define ECONNRESET    54
#define EBADF         9
#define EMFILE        24
#define EADDRINUSE    48
#define ENOTSOCK      38
#define ENOTCONN      57
#define ECONNREFUSED  61

/* Socket address structures */
struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    uint8_t         sin_len;
    uint8_t         sin_family;
    uint16_t        sin_port;
    struct in_addr  sin_addr;
    char            sin_zero[8];
};

struct sockaddr {
    uint8_t  sa_len;
    uint8_t  sa_family;
    char     sa_data[14];
};

typedef uint32_t socklen_t;

/* Socket function declarations */
int socket(int domain, int type, int protocol);
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
int close(int fd);
int shutdown(int sockfd, int how);
int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
int fcntl(int fd, int cmd, ...);

/* Network byte order conversion - 68k is big-endian, same as network order */
uint16_t htons(uint16_t hostshort);
uint32_t htonl(uint32_t hostlong);
uint16_t ntohs(uint16_t netshort);
uint32_t ntohl(uint32_t netlong);

/* Open Transport cleanup */
void cleanup_open_transport(void);

/* Legacy cleanup alias (calls cleanup_open_transport) */
void cleanup_mactcp(void);

/* Network stack selection */
int net_is_open_transport_available(void);
int net_is_using_open_transport(void);
int net_get_selected_stack(void);   /* Get user's selected stack (1=OT, 0=MacTCP) */
int net_set_stack(int use_ot);      /* 1 for OT, 0 for MacTCP */
void net_shutdown(void);            /* Stop server to allow stack switch */
int net_needs_restart(void);        /* Check if server needs restart */
void net_clear_restart(void);       /* Clear restart flag after restarting */

#endif /* MAC68K_PLATFORM */

#endif /* MAC68K_NET_H */
