/* 主机替身：lwip/sockets.h。主机上直接用 POSIX socket，名字差异只有 closesocket。 */
#ifndef GW_STUB_LWIP_SOCKETS_H
#define GW_STUB_LWIP_SOCKETS_H
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>   /* struct timeval：gw_timesync 拿它做 SO_RCVTIMEO */
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
#endif
