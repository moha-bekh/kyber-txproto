// Copied and adapted from scrcpy

#include "net.h"

#include <stdio.h>

#ifdef _WIN32
#include <ws2tcpip.h>
  typedef int socklen_t;
#else
# include <sys/types.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <unistd.h>
# include <fcntl.h>
# define SOCKET_ERROR -1
  typedef struct sockaddr_in SOCKADDR_IN;
  typedef struct sockaddr SOCKADDR;
  typedef struct in_addr IN_ADDR;
#endif

bool
net_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    int res = WSAStartup(MAKEWORD(2, 2), &wsa) < 0;
    if (res < 0) {
        return false;
    }
#endif
    return true;
}

void
net_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

#ifndef HAVE_SOCK_CLOEXEC
// If SOCK_CLOEXEC does not exist, the flag must be set manually once the
// socket is created
static bool
set_cloexec_flag(socket_t sock) {
#ifndef _WIN32
    if (fcntl(sock, F_SETFD, FD_CLOEXEC) == -1) {
        perror("fcntl F_SETFD");
        return false;
    }
#else
    if (!SetHandleInformation((HANDLE) sock, HANDLE_FLAG_INHERIT, 0)) {
        perror("SetHandleInformation socket failed");
        return false;
    }
#endif
    return true;
}
#endif

static void
net_perror(const char *s) {
#ifdef _WIN32
    int error = WSAGetLastError();
    char *wsa_message;
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
            NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (char *) &wsa_message, 0, NULL);
    // no explicit '\n', wsa_message already contains a trailing '\n'
    fprintf(stderr, "%s: [%d] %s", s, error, wsa_message);
    LocalFree(wsa_message);
#else
    perror(s);
#endif
}

socket_t
net_socket(void) {
#ifdef HAVE_SOCK_CLOEXEC
    socket_t sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock != INVALID_SOCKET && !set_cloexec_flag(sock)) {
        net_close(sock);
        return INVALID_SOCKET;
    }
#endif

    if (sock == INVALID_SOCKET)
        net_perror("socket");

    return sock;
}

bool
net_connect(socket_t sock, uint32_t addr, uint16_t port) {
    SOCKADDR_IN sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(addr);
    sin.sin_port = htons(port);

    if (connect(sock, (SOCKADDR *) &sin, sizeof(sin)) == SOCKET_ERROR) {
        net_perror("connect");
        net_close(sock);
        return INVALID_SOCKET;
    }

    return true;
}

bool
net_listen(socket_t sock, uint32_t addr, uint16_t port, int backlog) {
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const void *) &reuse,
                   sizeof(reuse)) == -1) {
        net_perror("setsockopt(SO_REUSEADDR)");
    }

    SOCKADDR_IN sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(addr); // htonl() harmless on INADDR_ANY
    sin.sin_port = htons(port);

    if (bind(sock, (SOCKADDR *) &sin, sizeof(sin)) == SOCKET_ERROR) {
        net_perror("bind");
        net_close(sock);
        return false;
    }

    if (listen(sock, backlog) == SOCKET_ERROR) {
        net_perror("listen");
        net_close(sock);
        return false;
    }

    return true;
}

socket_t
net_accept(socket_t server_socket) {
    SOCKADDR_IN csin;
    socklen_t sinsize = sizeof(csin);
    return accept(server_socket, (SOCKADDR *) &csin, &sinsize);
}

ssize_t
net_recv(socket_t socket, void *buf, size_t len) {
    return recv(socket, buf, len, 0);
}

ssize_t
net_recv_all(socket_t socket, void *buf, size_t len) {
    return recv(socket, buf, len, MSG_WAITALL);
}

ssize_t
net_send(socket_t socket, const void *buf, size_t len) {
    return send(socket, buf, len, 0);
}

ssize_t
net_send_all(socket_t socket, const void *buf, size_t len) {
    size_t copied = 0;
    while (len > 0) {
        ssize_t w = net_send(socket, buf, len);
        if (w == -1) {
            return copied ? (ssize_t) copied : -1;
        }
        len -= w;
        buf = (char *) buf + w;
        copied += w;
    }
    return copied;
}

bool
net_shutdown(socket_t socket, int how) {
    return !shutdown(socket, how);
}

bool
net_close(socket_t socket) {
#ifdef _WIN32
    return !closesocket(socket);
#else
    return !close(socket);
#endif
}

bool
net_parse_ipv4(const char *s, uint32_t *ipv4) {
    struct in_addr addr;
    if (!inet_pton(AF_INET, s, &addr)) {
        return false;
    }

    *ipv4 = ntohl(addr.s_addr);
    return true;
}
