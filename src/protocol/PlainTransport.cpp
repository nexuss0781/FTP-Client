/*
 * PlainTransport.cpp - Plain TCP Socket Transport Implementation
 * 
 * Implements the Transport interface using standard blocking TCP sockets.
 * Platform abstraction: AF_INET/AF_INET6 dual-stack. Windows Winsock2 vs POSIX sys/socket.h
 * isolated here. No other component in the codebase may include socket headers directly.
 */

#include "PlainTransport.hpp"
#include <cstring>
#include <cerrno>

// Platform-specific socket headers - ONLY this file should include these
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
#endif

namespace ftpclient { namespace protocol {

// Error codes matching Phase 1 ABI
static constexpr int32_t ERR_CONNECT = -401;
static constexpr int32_t ERR_TIMEOUT = -402;
static constexpr int32_t ERR_NETWORK_RESET = -403;
static constexpr int32_t ERR_DNS = -404;

PlainTransport::PlainTransport()
    : socket_(INVALID_SOCKET_HANDLE)
    , host_()
    , port_(0)
    , connected_(false)
{
}

PlainTransport::~PlainTransport() {
    shutdown();
}

int32_t PlainTransport::connect(const char* host, uint16_t port) {
    if (host == nullptr || port == 0) {
        return -EINVAL;
    }
    
    // Close any existing connection
    if (connected_) {
        shutdown();
    }
    
    host_ = host;
    port_ = port;
    
    // Resolve hostname
    struct sockaddr_storage addr{};
    socklen_t addr_len = 0;
    int32_t ret = resolve_host(host, &addr, &addr_len);
    if (ret != 0) {
        return ret;
    }
    
    // Create socket
    socket_ = ::socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET_HANDLE) {
        return ERR_CONNECT;
    }
    
    // Configure socket options
    ret = configure_socket(socket_);
    if (ret != 0) {
        ::close(socket_);
        socket_ = INVALID_SOCKET_HANDLE;
        return ret;
    }
    
    // Connect to server
    ret = ::connect(socket_, reinterpret_cast<struct sockaddr*>(&addr), addr_len);
    if (ret != 0) {
        int err = errno;
        ::close(socket_);
        socket_ = INVALID_SOCKET_HANDLE;
        
        if (err == ETIMEDOUT) {
            return ERR_TIMEOUT;
        } else if (err == ECONNREFUSED) {
            return ERR_CONNECT;
        }
        return ERR_NETWORK_RESET;
    }
    
    connected_ = true;
    return 0;
}

int32_t PlainTransport::read(void* buffer, uint32_t length) {
    if (!connected_ || buffer == nullptr || length == 0) {
        return -EINVAL;
    }
    
#ifdef _WIN32
    int result = ::recv(socket_, static_cast<char*>(buffer), length, 0);
#else
    ssize_t result = ::recv(socket_, buffer, length, 0);
#endif
    
    if (result < 0) {
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAECONNRESET || err == WSAECONNABORTED) {
            connected_ = false;
            return ERR_NETWORK_RESET;
        }
#else
        if (errno == ECONNRESET || errno == EPIPE) {
            connected_ = false;
            return ERR_NETWORK_RESET;
        }
#endif
        return ERR_NETWORK_RESET;
    }
    
    if (result == 0) {
        // Orderly close by peer
        connected_ = false;
        return 0;
    }
    
    return static_cast<int32_t>(result);
}

int32_t PlainTransport::write(const void* buffer, uint32_t length) {
    if (!connected_ || buffer == nullptr || length == 0) {
        return -EINVAL;
    }
    
    uint32_t total_written = 0;
    const char* ptr = static_cast<const char*>(buffer);
    
    while (total_written < length) {
#ifdef _WIN32
        int result = ::send(socket_, ptr + total_written, length - total_written, 0);
#else
        ssize_t result = ::send(socket_, ptr + total_written, length - total_written, 0);
#endif
        
        if (result < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAECONNRESET || err == WSAECONNABORTED) {
                connected_ = false;
                return ERR_NETWORK_RESET;
            }
#else
            if (errno == ECONNRESET || errno == EPIPE) {
                connected_ = false;
                return ERR_NETWORK_RESET;
            }
#endif
            return ERR_NETWORK_RESET;
        }
        
        if (result == 0) {
            // Should not happen with TCP, but handle gracefully
            return ERR_NETWORK_RESET;
        }
        
        total_written += static_cast<uint32_t>(result);
    }
    
    return static_cast<int32_t>(total_written);
}

int32_t PlainTransport::shutdown() {
    if (!connected_ && socket_ == INVALID_SOCKET_HANDLE) {
        return 0;  // Already closed
    }
    
    connected_ = false;
    
    if (socket_ != INVALID_SOCKET_HANDLE) {
#ifdef _WIN32
        ::shutdown(socket_, SD_BOTH);
        ::closesocket(socket_);
#else
        ::shutdown(socket_, SHUT_RDWR);
        ::close(socket_);
#endif
        socket_ = INVALID_SOCKET_HANDLE;
    }
    
    host_.clear();
    port_ = 0;
    return 0;
}

bool PlainTransport::is_connected() const {
    return connected_;
}

int32_t PlainTransport::resolve_host(const char* host, struct sockaddr_storage* addr, socklen_t* addr_len) {
    struct addrinfo hints{};
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;      // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    
    struct addrinfo* result = nullptr;
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", port_);
    
    int ret = getaddrinfo(host, port_str, &hints, &result);
    if (ret != 0) {
        return ERR_DNS;
    }
    
    // Use first result
    if (result->ai_addrlen > sizeof(struct sockaddr_storage)) {
        freeaddrinfo(result);
        return ERR_DNS;
    }
    
    std::memcpy(addr, result->ai_addr, result->ai_addrlen);
    *addr_len = static_cast<socklen_t>(result->ai_addrlen);
    freeaddrinfo(result);
    
    return 0;
}

int32_t PlainTransport::configure_socket(SocketHandle sock) {
    // TCP_NODELAY - disable Nagle for control channel responsiveness
    int flag = 1;
#ifdef _WIN32
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
#else
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#endif
    
    // SO_KEEPALIVE - enable OS-level keepalive probes
#ifdef _WIN32
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&flag), sizeof(flag));
#else
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
#endif
    
    // SO_RCVBUF - large receive buffer (256KB per spec Section 9.1)
    int rcvbuf = 256 * 1024;
#ifdef _WIN32
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
#else
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
#endif
    
    // SO_SNDBUF - large send buffer (256KB per spec Section 9.1)
    int sndbuf = 256 * 1024;
#ifdef _WIN32
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
#else
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
#endif
    
    return 0;
}

}} // namespace ftpclient::protocol
