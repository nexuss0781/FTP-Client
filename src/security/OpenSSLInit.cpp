/*
 * OpenSSLInit.cpp - Global OpenSSL Initialization/Finalization
 * 
 * Per Phase 3 Spec Section 11 (Deliverables)
 */

#include "OpenSSLInit.hpp"
#include <atomic>
#include <mutex>

#include <openssl/ssl.h>
#include <openssl/err.h>

namespace ftpclient { namespace security {

// Global state
static std::atomic<bool> g_initialized{false};
static std::atomic<bool> g_init_failed{false};
static SSL_CTX* g_ssl_ctx = nullptr;
static std::mutex g_init_mutex;

int32_t init_openssl() {
    // Fast path - already initialized
    if (g_initialized.load(std::memory_order_acquire)) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_init_mutex);

    // Double-check after acquiring lock
    if (g_initialized.load(std::memory_order_acquire)) {
        return 0;
    }

    // Check if previous init failed
    if (g_init_failed.load(std::memory_order_acquire)) {
        return -102;  // FTP_ERR_SYSTEM
    }

    // Initialize OpenSSL
    if (!OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | 
                          OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr)) {
        g_init_failed.store(true, std::memory_order_release);
        return -102;  // FTP_ERR_SYSTEM
    }

    // Create SSL context
    const SSL_METHOD* method = TLS_client_method();
    g_ssl_ctx = SSL_CTX_new(method);
    if (!g_ssl_ctx) {
        g_init_failed.store(true, std::memory_order_release);
        return -102;  // FTP_ERR_SYSTEM
    }

    // Set minimum TLS version to 1.2 (per spec Section 7.1)
    SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);
    
    // Set secure cipher suites (per spec Section 7.2)
    // Exclude: NULL, anonymous DH, export ciphers, RC4, MD5
    int ret = SSL_CTX_set_cipher_list(g_ssl_ctx, 
        "HIGH:!aNULL:!eNULL:!EXPORT:!RC4:!MD5:!PSK:!SRP:!CAMELLIA");
    if (ret != 1) {
        // Some older OpenSSL versions may not support this exact list
        // Fall back to simpler list
        SSL_CTX_set_cipher_list(g_ssl_ctx, "HIGH:!aNULL:!MD5:!RC4");
    }

    // Enable session resumption (per spec Section 7.4)
    SSL_CTX_set_session_cache_mode(g_ssl_ctx, SSL_SESS_CACHE_CLIENT);

    // Load default CA certificates
    ret = SSL_CTX_set_default_verify_paths(g_ssl_ctx);
    if (ret != 1) {
        // Warning but not fatal - system may not have CA store
        // Applications can provide custom CA bundle
    }

    g_initialized.store(true, std::memory_order_release);
    return 0;
}

void cleanup_openssl() {
    std::lock_guard<std::mutex> lock(g_init_mutex);

    if (g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = nullptr;
    }

    g_initialized.store(false, std::memory_order_release);
    g_init_failed.store(false, std::memory_order_release);

    OPENSSL_cleanup();
}

void* get_shared_ssl_ctx() {
    if (!g_initialized.load(std::memory_order_acquire)) {
        // Auto-initialize if not already done
        int32_t ret = init_openssl();
        if (ret != 0) {
            return nullptr;
        }
    }

    return g_ssl_ctx;
}

}} // namespace ftpclient::security
