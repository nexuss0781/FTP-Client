/*
 * Transport.hpp - Transport Abstraction Layer (TAL)
 * 
 * This interface is the critical seam between Phase 2 (protocol) and Phase 3 (security/TLS).
 * All socket I/O goes through this interface. No other component may include <sys/socket.h>
 * or <winsock2.h> directly.
 * 
 * Phase 3 Extension Point: TlsTransport will implement this same interface, wrapping OpenSSL
 * or mbedTLS. The Protocol Engine holds a std::unique_ptr<Transport> and is agnostic to encryption.
 */

#ifndef FTPCLIENT_TRANSPORT_HPP
#define FTPCLIENT_TRANSPORT_HPP

#include <cstdint>
#include <memory>

namespace ftpclient { namespace protocol {

/**
 * Transport Interface
 * 
 * Abstract base class for all transport implementations (plain TCP, TLS, etc.)
 * All methods return 0 on success, negative errno-style codes on failure.
 */
class Transport {
public:
    virtual ~Transport() = default;

    /**
     * Establish connection to host:port
     * @param host UTF-8 hostname or IP address
     * @param port Port number in host byte order
     * @return 0 on success, negative error code on failure
     */
    virtual int32_t connect(const char* host, uint16_t port) = 0;

    /**
     * Blocking read from transport
     * @param buffer Destination buffer
     * @param length Maximum bytes to read
     * @return Bytes read (>0), 0 on orderly close, negative on error
     */
    virtual int32_t read(void* buffer, uint32_t length) = 0;

    /**
     * Blocking write to transport
     * @param buffer Source buffer
     * @param length Bytes to write
     * @return Bytes written (>0), negative on error
     */
    virtual int32_t write(const void* buffer, uint32_t length) = 0;

    /**
     * Graceful shutdown and close
     * @return 0 on success, negative on error
     */
    virtual int32_t shutdown() = 0;

    /**
     * Check if transport is connected
     * @return true if connected, false otherwise
     */
    virtual bool is_connected() const = 0;
};

/**
 * Factory function type for creating transports
 * Used by ProtocolEngine to inject different transport types (Phase 3 TLS)
 */
using TransportFactory = std::unique_ptr<Transport>(*)();

}} // namespace ftpclient::protocol

#endif /* FTPCLIENT_TRANSPORT_HPP */
