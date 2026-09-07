#pragma once

#include <cstddef>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

namespace bedrock::detail {
#if defined(_WIN32)
using Socket = SOCKET;
inline constexpr Socket invalidSocket = INVALID_SOCKET;
inline void ensureSockets() {
    struct Runtime {
        Runtime() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed"); }
        ~Runtime() { WSACleanup(); }
    };
    static const Runtime runtime;
}
inline void closeSocket(Socket socket) { closesocket(socket); }
inline bool socketInterrupted() { return WSAGetLastError() == WSAEINTR; }
inline int selectWidth(Socket) { return 0; } // Winsock ignores nfds; SOCKET is pointer-sized.
#else
using Socket = int;
inline constexpr Socket invalidSocket = -1;
inline void ensureSockets() {}
inline void closeSocket(Socket socket) { close(socket); }
inline bool socketInterrupted() { return errno == EINTR; }
inline int selectWidth(Socket socket) { return socket + 1; }
#endif
inline auto sendDatagram(Socket socket, const void* data, std::size_t size, int flags, const sockaddr* address, int length) {
    return sendto(socket, static_cast<const char*>(data), static_cast<int>(size), flags, address, length);
}
inline auto receiveDatagram(Socket socket, void* data, std::size_t size) {
    return recvfrom(socket, static_cast<char*>(data), static_cast<int>(size), 0, nullptr, nullptr);
}
inline void enableBroadcast(Socket socket) {
    const int enabled = 1;
    (void)setsockopt(socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
}
} // namespace bedrock::detail
