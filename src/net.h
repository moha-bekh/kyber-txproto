// Copied and adapted from scrcpy

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
# include <winsock2.h>
  #define SHUT_RD SD_RECEIVE
  #define SHUT_WR SD_SEND
  #define SHUT_RDWR SD_BOTH
  typedef SOCKET socket_t;

  // Taken from mingw-w64
  __forceinline unsigned __int64 htonll(unsigned __int64 Value) { return (((unsigned __int64)htonl(Value & 0xFFFFFFFFUL)) << 32) | htonl((u_long)(Value >> 32)); }

#else
# include <sys/socket.h>
# define INVALID_SOCKET -1
  typedef int socket_t;
#endif

#define IPV4_LOCALHOST 0x7F000001

bool
net_init(void);

void
net_cleanup(void);

socket_t
net_socket(void);

bool
net_connect(socket_t socket, uint32_t addr, uint16_t port);

bool
net_listen(socket_t socket, uint32_t addr, uint16_t port, int backlog);

socket_t
net_accept(socket_t server_socket);

// the _all versions wait/retry until len bytes have been written/read
ssize_t
net_recv(socket_t socket, void *buf, size_t len);

ssize_t
net_recv_all(socket_t socket, void *buf, size_t len);

ssize_t
net_send(socket_t socket, const void *buf, size_t len);

ssize_t
net_send_all(socket_t socket, const void *buf, size_t len);

// how is SHUT_RD (read), SHUT_WR (write) or SHUT_RDWR (both)
bool
net_shutdown(socket_t socket, int how);

bool
net_close(socket_t socket);

/**
 * Parse `ip` "xxx.xxx.xxx.xxx" to an IPv4 host representation
 */
bool
net_parse_ipv4(const char *ip, uint32_t *ipv4);
