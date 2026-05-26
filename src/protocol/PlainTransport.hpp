/*
 * PlainTransport.hpp - Plain TCP Socket Transport Implementation
 * 
 * Implements the Transport interface using standard blocking TCP sockets.
 * Platform abstraction: AF_INET/AF_INET6 dual-stack. Windows Winsock2 vs POSIX sys/socket.h
 * isolated here. No other component in the codebase may include socket headers directly.
 */

#ifndef FTPCLIENT_PLAIN_TRANSPORT_HPP
#define FTPCLIENT_PLAIN_TRANSPORT_HPP

#include "Transport.hpp"
#include <string>
#include <sys/types.h>
#include <sys/socket.h>

namespace ftpclient { namespace protocol {

/**
 * Plain TCP Transport Implementation
 * 
 * Uses standard blocking TCP sockets with configurable buffer sizes and timeouts.
 * Implements NAT traversal intelligence for PASV mode (uses control host IP if
 * server returns private IP).
 */
class PlainTransport : public Transport {
public:
    PlainTransport();
    ~PlainTransport() override;

    // Prevent copying
    PlainTransport(const PlainTransport&) = delete;
    PlainTransport& operator=(const PlainTransport&) = delete;

    int32_t connect(const char* host, uint16_t port) override;
    int32_t read(void* buffer, uint32_t length) override;
    int32_t write(const void* buffer, uint32_t length) override;
    int32_t shutdown() override;
    bool is_connected() const override;

    /**
     * Get the remote host this transport connected to
     */
    const std::string& get_host() const { return host_; }

    /**
     * Get the remote port this transport connected to
     */
    uint16_t get_port() const { return port_; }

private:
    // Platform-specific socket handle
#ifdef _WIN32
    using SocketHandle = intptr_t;
    static constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
#else
    using SocketHandle = int;
    static constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
#endif

    SocketHandle socket_;
    std::string host_;
    uint16_t port_;
    bool connected_;

    // Internal helper methods
    int32_t resolve_host(const char* host, struct sockaddr_storage* addr, socklen_t* addr_len);
    int32_t configure_socket(SocketHandle sock);
};

}} // namespace ftpclient::protocol

#endif /* FTPCLIENT_PLAIN_TRANSPORT_HPP */
