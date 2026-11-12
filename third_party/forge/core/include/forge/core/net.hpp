#pragma once
// BSD-socket compatibility names for code that uses raw sockets; new code should use forge::os::socket.
#include <forge/os/os.hpp>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using forge_sock_t = SOCKET;
#define FORGE_SOCK_BAD INVALID_SOCKET
#define forge_sock_close closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using forge_sock_t = int;
#define FORGE_SOCK_BAD (-1)
#define forge_sock_close ::close
#endif
#if defined(_WIN32)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
using socklen_t = int;
#endif
namespace forge {
inline void net_init() { os::net_init(); }
} // namespace forge
