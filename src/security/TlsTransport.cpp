/*
 * TlsTransport.cpp - TLS Transport Implementation
 * 
 * Per Phase 3 Spec Section 4
 */

#include "TlsTransport.hpp"
#include <cstring>
#include <cerrno>

// OpenSSL headers - only this file should include them
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

// Error codes matching Phase 1 ABI
static constexpr int32_t ERR_CONNECT = -401;
static constexpr int32_t ERR_TIMEOUT = -402;
static constexpr int32_t ERR_NETWORK_RESET = -403;
static constexpr int32_t ERR_CERT_VERIFY = -303;
static constexpr int32_t ERR_PROTOCOL = -501;

namespace ftpclient { namespace security {

TlsTransport::TlsTransport(SSL_CTX* shared_ctx, const TlsConfig& config)
    : ssl_ctx_(shared_ctx)
    , ssl_(nullptr)
    , bio_(nullptr)
    , config_(config)
    , host_()
    , port_(0)
    , socket_fd_(-1)
    , connected_(false)
    , tls_established_(false)
    , implicit_mode_(false)
{
}

TlsTransport::~TlsTransport() {
    shutdown();
}

void TlsTransport::set_implicit_mode(bool implicit) {
    implicit_mode_ = implicit;
}

int TlsTransport::get_socket_fd() const {
    return socket_fd_;
}

bool TlsTransport::is_tls_established() const {
    return tls_established_;
}

int32_t TlsTransport::init_ssl() {
    // Create new SSL object
    ssl_ = SSL_new(ssl_ctx_);
    if (!ssl_) {
        return ERR_PROTOCOL;
    }

    // Set SNI hostname if not an IP address
    if (!host_.empty() && !config_.server_name.empty()) {
        SSL_set_tlsext_host_name(ssl_, config_.server_name.c_str());
    }

    // Create socket BIO
    bio_ = BIO_new_socket(socket_fd_, BIO_NOCLOSE);
    if (!bio_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
        return ERR_PROTOCOL;
    }

    // Attach BIO to SSL
    SSL_set_bio(ssl_, bio_, bio_);
    
    // Set verification mode
    if (config_.verify_mode == CertVerifyMode::NONE) {
        SSL_set_verify(ssl_, SSL_VERIFY_NONE, nullptr);
    } else {
        SSL_set_verify(ssl_, SSL_VERIFY_PEER, nullptr);
    }

    return 0;
}

int32_t TlsTransport::verify_certificate() {
    if (config_.verify_mode == CertVerifyMode::NONE) {
        return 0;  // No verification requested
    }

    X509* cert = SSL_get_peer_certificate(ssl_);
    if (!cert) {
        return ERR_CERT_VERIFY;
    }

    // Verify certificate chain
    long verify_result = SSL_get_verify_result(ssl_);
    if (verify_result != X509_V_OK) {
        X509_free(cert);
        return ERR_CERT_VERIFY;
    }

    // Verify hostname if requested
    if (config_.verify_mode == CertVerifyMode::HOST) {
        if (!verify_hostname(host_.c_str())) {
            X509_free(cert);
            return ERR_CERT_VERIFY;
        }
    }

    X509_free(cert);
    return 0;
}

bool TlsTransport::verify_hostname(const char* hostname) {
    if (!hostname || hostname[0] == '\0') {
        return false;
    }

    X509* cert = SSL_get_peer_certificate(ssl_);
    if (!cert) {
        return false;
    }

    bool match = false;

    // Check Subject Alternative Names first (preferred)
    GENERAL_NAMES* san_names = static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    
    if (san_names) {
        int count = sk_GENERAL_NAME_num(san_names);
        for (int i = 0; i < count && !match; ++i) {
            GENERAL_NAME* name = sk_GENERAL_NAME_value(san_names, i);
            if (name->type == GEN_DNS) {
                ASN1_STRING* dns_name = name->d.dNSName;
                const char* san = reinterpret_cast<const char*>(ASN1_STRING_data(dns_name));
                int san_len = ASN1_STRING_length(dns_name);
                
                // Simple wildcard matching
                if (san_len > 0 && san[0] == '*') {
                    // Wildcard: *.example.com matches foo.example.com
                    const char* dot = std::strchr(hostname, '.');
                    if (dot && std::strcmp(san + 1, dot + 1) == 0) {
                        match = true;
                    }
                } else if (std::strncmp(san, hostname, san_len) == 0 && hostname[san_len] == '\0') {
                    match = true;
                }
            }
        }
        GENERAL_NAMES_free(san_names);
    }

    // Fall back to Common Name if no SAN match
    if (!match) {
        X509_NAME* subject = X509_get_subject_name(cert);
        int cn_idx = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
        if (cn_idx >= 0) {
            X509_NAME_ENTRY* cn_entry = X509_NAME_get_entry(subject, cn_idx);
            ASN1_STRING* cn_asn1 = X509_NAME_ENTRY_get_data(cn_entry);
            const char* cn = reinterpret_cast<const char*>(ASN1_STRING_data(cn_asn1));
            int cn_len = ASN1_STRING_length(cn_asn1);
            
            if (cn_len > 0 && cn[0] == '*') {
                const char* dot = std::strchr(hostname, '.');
                if (dot && std::strcmp(cn + 1, dot + 1) == 0) {
                    match = true;
                }
            } else if (std::strncmp(cn, hostname, cn_len) == 0 && hostname[cn_len] == '\0') {
                match = true;
            }
        }
    }

    X509_free(cert);
    return match;
}

int32_t TlsTransport::connect(const char* host, uint16_t port) {
    if (!host || port == 0) {
        return -EINVAL;
    }

    // Close any existing connection
    if (connected_) {
        shutdown();
    }

    host_ = host;
    port_ = port;

    // For implicit mode, we need to connect TCP and immediately do TLS handshake
    // For explicit mode, PlainTransport would have already connected TCP
    // This is called after TCP connection is established
    
    // Initialize SSL
    int32_t ret = init_ssl();
    if (ret != 0) {
        return ret;
    }

    connected_ = true;

    // For implicit mode, perform handshake immediately
    if (implicit_mode_) {
        ret = handshake();
        if (ret != 0) {
            shutdown();
            return ret;
        }
    }

    return 0;
}

int32_t TlsTransport::handshake() {
    if (!ssl_ || !connected_) {
        return ERR_CONNECT;
    }

    // Set min/max TLS versions
    SSL_set_min_proto_version(ssl_, static_cast<int>(config_.min_version));
    if (config_.max_version != static_cast<TlsVersion>(0)) {
        SSL_set_max_proto_version(ssl_, static_cast<int>(config_.max_version));
    }

    // Set cipher list if specified
    if (!config_.cipher_list.empty()) {
        SSL_set_cipher_list(ssl_, config_.cipher_list.c_str());
    }

    // Perform handshake
    int result = SSL_connect(ssl_);
    if (result != 1) {
        int ssl_error = SSL_get_error(ssl_, result);
        
        if (ssl_error == SSL_ERROR_SYSCALL) {
            return ERR_NETWORK_RESET;
        } else if (ssl_error == SSL_ERROR_SSL) {
            // Likely a certificate or protocol version error
            unsigned long err = ERR_get_error();
            if (ERR_GET_LIB(err) == ERR_LIB_SSL && 
                ERR_GET_REASON(err) == SSL_R_VERSION_TOO_LOW) {
                return ERR_CERT_VERIFY;  // Version too low
            }
            return ERR_CERT_VERIFY;
        }
        return ERR_PROTOCOL;
    }

    // Verify certificate
    int32_t verify_ret = verify_certificate();
    if (verify_ret != 0) {
        return verify_ret;
    }

    tls_established_ = true;
    return 0;
}

int32_t TlsTransport::read(void* buffer, uint32_t length) {
    if (!connected_ || !buffer || length == 0) {
        return -EINVAL;
    }

    if (!tls_established_) {
        // Fall back to plain read if TLS not established
        // (shouldn't happen in normal operation)
        return ERR_PROTOCOL;
    }

    int result = SSL_read(ssl_, buffer, static_cast<int>(length));
    if (result <= 0) {
        int ssl_error = SSL_get_error(ssl_, result);
        
        if (ssl_error == SSL_ERROR_ZERO_RETURN) {
            // Orderly close
            connected_ = false;
            return 0;
        } else if (ssl_error == SSL_ERROR_SYSCALL) {
            connected_ = false;
            return ERR_NETWORK_RESET;
        }
        return ERR_NETWORK_RESET;
    }

    return static_cast<int32_t>(result);
}

int32_t TlsTransport::write(const void* buffer, uint32_t length) {
    if (!connected_ || !buffer || length == 0) {
        return -EINVAL;
    }

    if (!tls_established_) {
        return ERR_PROTOCOL;
    }

    uint32_t total_written = 0;
    const char* ptr = static_cast<const char*>(buffer);

    while (total_written < length) {
        int result = SSL_write(ssl_, ptr + total_written, 
                               static_cast<int>(length - total_written));
        if (result <= 0) {
            int ssl_error = SSL_get_error(ssl_, result);
            
            if (ssl_error == SSL_ERROR_SYSCALL) {
                connected_ = false;
                return ERR_NETWORK_RESET;
            }
            return ERR_NETWORK_RESET;
        }

        total_written += static_cast<uint32_t>(result);
    }

    return static_cast<int32_t>(total_written);
}

int32_t TlsTransport::shutdown_tls() {
    if (ssl_ && tls_established_) {
        SSL_shutdown(ssl_);
        tls_established_ = false;
    }
    return 0;
}

int32_t TlsTransport::shutdown() {
    if (!connected_ && socket_fd_ < 0) {
        return 0;  // Already closed
    }

    // Graceful TLS shutdown
    shutdown_tls();

    // Free SSL object
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    // Note: bio_ is freed by SSL_free when attached

    connected_ = false;
    socket_fd_ = -1;
    host_.clear();
    port_ = 0;

    return 0;
}

bool TlsTransport::is_connected() const {
    return connected_;
}

}} // namespace ftpclient::security
