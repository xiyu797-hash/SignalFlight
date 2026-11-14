#pragma once
// Cross-platform IPv4 UDP socket primitives. Linux keeps epoll/recvmmsg in sf_engine;
// native Windows PowerShell builds use Winsock2 + select and nonblocking recvfrom.
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// inet_pton/inet_ntop and other Winsock interfaces are available on modern
// Windows. Declare the minimum target before including winsock2/ws2tcpip.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif
namespace sf::net {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid = INVALID_SOCKET;
inline int last_error() { return WSAGetLastError(); }
inline bool would_block(int e) { return e == WSAEWOULDBLOCK; }
inline bool interrupted(int e) { return e == WSAEINTR; }
inline void close_socket(Socket s) { if (s != invalid) ::closesocket(s); }
inline bool nonblocking(Socket s) { u_long yes = 1; return ::ioctlsocket(s, FIONBIO, &yes) == 0; }
struct Runtime {
    Runtime() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed"); }
    ~Runtime() { WSACleanup(); }
    Runtime(const Runtime&) = delete; Runtime& operator=(const Runtime&) = delete;
};
#else
using Socket = int;
constexpr Socket invalid = -1;
inline int last_error() { return errno; }
inline bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
inline bool interrupted(int e) { return e == EINTR; }
inline void close_socket(Socket s) { if (s != invalid) ::close(s); }
inline bool nonblocking(Socket s) { int flags = fcntl(s, F_GETFL, 0); return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0; }
struct Runtime { Runtime() = default; };
#endif
// Socket owner prevents descriptor leaks when validation throws.
class OwnedSocket {
    Socket s_ = invalid;
public:
    explicit OwnedSocket(Socket s = invalid): s_(s) {}
    ~OwnedSocket() { close_socket(s_); }
    OwnedSocket(const OwnedSocket&) = delete; OwnedSocket& operator=(const OwnedSocket&) = delete;
    Socket get() const { return s_; }
};
inline void require_socket(Socket s, const std::string& op) {
    if (s == invalid) throw std::runtime_error(op + " failed, socket error=" + std::to_string(last_error()));
}
}
