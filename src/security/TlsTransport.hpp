/*
 * TlsTransport.hpp - TLS Transport Implementation
 * 
 * Implements the Transport interface using OpenSSL for FTPS.
 * Supports both Explicit FTPS (AUTH TLS on port 21) and Implicit FTPS (port 990).
 * 
 * Per Phase 3 Spec Section 4
 */

#ifndef FTPCLIENT_TLS_TRANSPORT_HPP
#define FTPCLIENT_TLS_TRANSPORT_HPP

#include "../protocol/Transport.hpp"
#include "TlsConfig.hpp"
#include <string>
#include <memory>

// Forward declare OpenSSL types to avoid including openssl headers here
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct bio_st BIO;

namespace ftpclient { namespace security {

/**
 * TLS Transport Implementation
 * 
 * Wraps a plain TCP socket with OpenSSL TLS encryption.
 * Used for both control channel and data channel encryption.
 */
class TlsTransport : public protocol::Transport {
public:
    /**
     * Constructor
     * 
     * @param shared_ctx Shared SSL_CTX (must outlive this transport)
     * @param config TLS configuration options
     */
    TlsTransport(SSL_CTX* shared_ctx, const TlsConfig& config);

    /**
     * Destructor - performs TLS shutdown and closes socket
     */
    ~TlsTransport() override;

    // Prevent copying
    TlsTransport(const TlsTransport&) = delete;
    TlsTransport& operator=(const TlsTransport&) = delete;

    // Transport interface implementation
    int32_t connect(const char* host, uint16_t port) override;
    int32_t read(void* buffer, uint32_t length) override;
    int32_t write(const void* buffer, uint32_t length) override;
    int32_t shutdown() override;
    bool is_connected() const override;

    /**
     * Perform TLS handshake after TCP connection
     * Called internally by connect() for implicit mode,
     * or explicitly after AUTH TLS command for explicit mode.
     * 
     * @return 0 on success, negative error code on failure
     */
    int32_t handshake();

    /**
     * Graceful TLS shutdown (send close_notify)
     * Called before TCP shutdown
     * 
     * @return 0 on success, negative error code on failure
     */
    int32_t shutdown_tls();

    /**
     * Set implicit FTPS mode
     * Must be called before connect()
     */
    void set_implicit_mode(bool implicit);

    /**
     * Get the underlying socket file descriptor
     * Used for select/poll in event-driven architectures
     */
    int get_socket_fd() const;

    /**
     * Check if TLS is established
     */
    bool is_tls_established() const;

private:
    SSL_CTX* ssl_ctx_;          // Shared context (not owned)
    SSL* ssl_;                  // SSL connection object (owned)
    BIO* bio_;                  // Socket BIO (owned)
    TlsConfig config_;
    
    std::string host_;
    uint16_t port_;
    int socket_fd_;
    bool connected_;
    bool tls_established_;
    bool implicit_mode_;

    /**
     * Initialize OpenSSL SSL object and BIO
     */
    int32_t init_ssl();

    /**
     * Verify certificate chain and hostname
     */
    int32_t verify_certificate();

    /**
     * Check if hostname matches certificate CN/SAN
     */
    bool verify_hostname(const char* hostname);
};

}} // namespace ftpclient::security

#endif /* FTPCLIENT_TLS_TRANSPORT_HPP */
