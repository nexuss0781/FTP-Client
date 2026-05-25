/*
 * OpenSSLInit.hpp - Global OpenSSL Initialization/Finalization
 * 
 * Thread-safe OpenSSL library initialization.
 * Must be called once at process startup before any TLS operations.
 * 
 * Per Phase 3 Spec Section 11 (Deliverables)
 */

#ifndef FTPCLIENT_OPENSSL_INIT_HPP
#define FTPCLIENT_OPENSSL_INIT_HPP

#include <cstdint>

namespace ftpclient { namespace security {

/**
 * Initialize OpenSSL library
 * 
 * Thread-safe, idempotent initialization.
 * Call once at process startup before any TLS operations.
 * 
 * @return 0 on success, negative error code on failure
 */
int32_t init_openssl();

/**
 * Cleanup OpenSSL library
 * 
 * Call once at process shutdown.
 * After this call, no further TLS operations are possible.
 */
void cleanup_openssl();

/**
 * Get shared SSL_CTX for client connections
 * 
 * Creates a properly configured SSL_CTX with:
 * - TLS 1.2 minimum
 * - Secure cipher suites
 * - Default CA loading
 * 
 * The returned context is owned by the library and must not be freed by caller.
 * It lives for the lifetime of the process.
 * 
 * @return Pointer to shared SSL_CTX, or nullptr if initialization failed
 */
void* get_shared_ssl_ctx();

}} // namespace ftpclient::security

#endif /* FTPCLIENT_OPENSSL_INIT_HPP */
