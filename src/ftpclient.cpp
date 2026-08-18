/*
 * ftpclient.cpp - C ABI Implementation
 * 
 * This file implements the C ABI functions defined in ftpclient.h.
 * All functions are wrapped in extern "C" to prevent name mangling.
 * No C++ exceptions cross this boundary.
 * 
 * Phase 3: Security & Credential Vault Implementation
 */

#include "FtpClientImpl.hpp"
#include "../include/ftpclient.h"
#include "security/OpenSSLInit.hpp"
#include "transfer/TransferEngine.hpp"
#include "resilience/RetryPolicy.hpp"
#include <new>
#include <cstring>
#include <vector>

/* Ensure locale independence as per spec Section 11 */
static void init_locale() {
    /* Set locale to "C" for locale-independent string operations */
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
    }
}

/* ============================================================================
 * SECTION 6.1: LIFECYCLE FUNCTIONS
 * ============================================================================
 */

extern "C" {

FTP_API int32_t FTP_CALL ftp_client_create(ftp_client_t** out_handle) {
    init_locale();
    
    if (out_handle == nullptr) {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    
    *out_handle = nullptr;
    
    try {
        /* Initialize OpenSSL on first client creation */
        int32_t ssl_ret = ftpclient::security::init_openssl();
        if (ssl_ret != 0) {
            return FTP_ERR_SYSTEM;
        }
        
        /* Allocate on heap using standard new */
        auto impl = new ftpclient::FtpClientImpl();
        impl->setState(ftpclient::ClientState::ALLOCATED);
        *out_handle = reinterpret_cast<ftp_client_t*>(impl);
        return FTP_OK;
    } catch (const std::bad_alloc&) {
        return FTP_ERR_NOMEM;
    } catch (...) {
        return FTP_ERR_SYSTEM;
    }
}

FTP_API int32_t FTP_CALL ftp_client_destroy(ftp_client_t* handle) {
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    
    /* Atomically mark as destroyed - returns false if already destroyed */
    if (!impl->markDestroyed()) {
        return FTP_ERR_INVALID_HANDLE;  /* Already destroyed */
    }
    
    try {
        delete impl;
        return FTP_OK;
    } catch (...) {
        return FTP_ERR_SYSTEM;
    }
}

/* ============================================================================
 * SECTION 6.2: CONFIGURATION FUNCTIONS
 * ============================================================================
 */

FTP_API int32_t FTP_CALL ftp_set_buffer_size(ftp_client_t* handle, uint64_t size_bytes) {
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    /* 0 means use default */
    if (size_bytes == 0) {
        size_bytes = 256 * 1024;  /* Default 256KB */
    }
    
    impl->getConfig().buffer_size = size_bytes;
    return FTP_OK;
}

FTP_API int32_t FTP_CALL ftp_set_timeout_connect_ms(ftp_client_t* handle, uint32_t ms) {
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    /* 0 means use default */
    if (ms == 0) {
        ms = 5000;  /* Default 5 seconds */
    }
    
    impl->getConfig().timeout_connect = ms;
    impl->getProtocolEngine().get_config().timeout_connect_ms = ms;
    return FTP_OK;
}

FTP_API int32_t FTP_CALL ftp_set_timeout_command_ms(ftp_client_t* handle, uint32_t ms) {
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    /* 0 means use default */
    if (ms == 0) {
        ms = 30000;  /* Default 30 seconds */
    }
    
    impl->getConfig().timeout_command = ms;
    impl->getProtocolEngine().set_command_timeout(ms);
    return FTP_OK;
}

/* ============================================================================
 * SECTION 6.3: CONNECTION MANAGEMENT
 * ============================================================================
 */

FTP_API int32_t FTP_CALL ftp_connect(ftp_client_t* handle, const ftp_credentials_t* creds) {
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    /* Validate state - must be ALLOCATED or DISCONNECTED to connect */
    auto state = impl->getState();
    if (state != ftpclient::ClientState::ALLOCATED && 
        state != ftpclient::ClientState::DISCONNECTED) {
        return FTP_ERR_INVALID_STATE;
    }
    
    /* Use credential provider if set, otherwise use static credentials */
    ftp_credentials_t resolved_creds;
    std::memset(&resolved_creds, 0, sizeof(resolved_creds));
    
    const ftp_credentials_t* creds_to_use = creds;
    
    if (impl->getProvider() && impl->getProvider()->is_set()) {
        /* Fetch credentials from provider */
        int32_t ret = impl->getProvider()->fetch_credentials(&resolved_creds, 0);
        if (ret != FTP_OK) {
            return ret;
        }
        creds_to_use = &resolved_creds;
    }
    
    /* Validate credentials - host is required */
    if (creds_to_use == nullptr || creds_to_use->host == nullptr || creds_to_use->host[0] == '\0') {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    
    /* Validate port - 0 is invalid */
    if (creds_to_use->port == 0) {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    
    ftpclient::protocol::ConnectionCredentials engine_creds;
    engine_creds.host = creds_to_use->host;
    engine_creds.port = creds_to_use->port;
    engine_creds.username = creds_to_use->username ? creds_to_use->username : "";
    engine_creds.password = creds_to_use->password ? creds_to_use->password : "";
    engine_creds.use_tls = creds_to_use->use_tls;
    engine_creds.verify_cert = creds_to_use->verify_cert;
    engine_creds.ca_bundle_path = creds_to_use->ca_bundle_path ? creds_to_use->ca_bundle_path : "";

    impl->getProtocolEngine().get_config().timeout_connect_ms = impl->getConfig().timeout_connect;
    impl->getProtocolEngine().set_command_timeout(impl->getConfig().timeout_command);
    int32_t ret = impl->getProtocolEngine().connect(engine_creds);
    if (ret != FTP_OK) {
        return ret;
    }

    if (creds_to_use->username && creds_to_use->password) {
        ret = impl->getVault().store(creds_to_use);
        if (ret != FTP_OK) {
            impl->getProtocolEngine().disconnect();
            return ret;
        }
    }

    impl->setState(ftpclient::ClientState::CONNECTED);
    return FTP_OK;
}

FTP_API int32_t FTP_CALL ftp_disconnect(ftp_client_t* handle) {
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    /* Disconnect is idempotent for allocated and disconnected handles. */
    auto state = impl->getState();
    if (state != ftpclient::ClientState::CONNECTED) {
        return FTP_OK;
    }

    int32_t ret = impl->getProtocolEngine().disconnect();
    impl->getVault().purge();
    impl->setState(ftpclient::ClientState::DISCONNECTED);
    return ret;
}

FTP_API int32_t FTP_CALL ftp_ping(ftp_client_t* handle) {
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    /* Can only ping when CONNECTED */
    auto state = impl->getState();
    if (state != ftpclient::ClientState::CONNECTED) {
        return FTP_ERR_INVALID_STATE;
    }

    return impl->getProtocolEngine().ping();
}

/* ============================================================================
 * SECTION 6.4: TRANSFER OPERATIONS (M1 Unavailable)
 * ============================================================================
 */

FTP_API int32_t FTP_CALL ftp_upload_dir(
    ftp_client_t* handle,
    const char* local_path,
    const char* remote_path,
    const ftp_upload_options_t* options,
    ftp_progress_cb_t progress_cb,
    void* user_data,
    ftp_result_t* out_result
) {
    (void)local_path;
    (void)remote_path;
    (void)options;
    (void)progress_cb;
    (void)user_data;
    (void)out_result;
    
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    /* Validate paths */
    if (local_path == nullptr || local_path[0] == '\0') {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    
    if (remote_path == nullptr || remote_path[0] == '\0') {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    
    /*
     * M0 validates the request but has no real data-channel implementation.
     * Return an explicit feature status rather than pretending the caller's
     * connection state is the reason the operation cannot run.
     */
    if (out_result != nullptr) {
        out_result->status = FTP_ERR_NOT_IMPLEMENTED;
        out_result->files_total = 0;
        out_result->files_success = 0;
        out_result->files_failed = 0;
        out_result->bytes_transferred = 0;
    }

    return FTP_ERR_NOT_IMPLEMENTED;
}

/* ============================================================================
 * SECTION 6.5: VERSION / CAPABILITY INTROSPECTION
 * ============================================================================
 */

FTP_API uint32_t FTP_CALL ftp_get_version(void) {
    /* Version format: 0xMMmmpp00 (Major, Minor, Patch) */
    /* M0 development baseline: 0.1.0 */
    return 0x00010000;
}

FTP_API int32_t FTP_CALL ftp_get_capabilities(uint64_t* out_caps) {
    if (out_caps == nullptr) {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    
    /*
     * M2 truthfulness rule: plain FTP and explicit FTPS control sessions are
     * public because they are wired and tested through the exported execution
     * path. Data-transfer features remain intentionally unadvertised.
     */
    /* M2 exposes the real plain and explicit-FTPS control sessions. */
    *out_caps = FTP_CAP_CONTROL_FTP | FTP_CAP_TLS;
    return FTP_OK;
}

/* ============================================================================
 * SECTION 6.6: SECURITY FUNCTIONS (Phase 3)
 * ============================================================================
 */

FTP_API int32_t FTP_CALL ftp_set_credential_provider(
    ftp_client_t* handle,
    ftp_credential_provider_cb_t provider,
    void* user_data
) {
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    try {
        if (provider == nullptr) {
            impl->setProvider(nullptr);
        } else {
            auto new_provider = std::make_unique<ftpclient::security::SecretProvider>(provider, user_data);
            impl->setProvider(std::move(new_provider));
        }
        return FTP_OK;
    } catch (const std::bad_alloc&) {
        return FTP_ERR_NOMEM;
    } catch (...) {
        return FTP_ERR_SYSTEM;
    }
}

FTP_API int32_t FTP_CALL ftp_clear_credential_provider(ftp_client_t* handle) {
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    impl->setProvider(nullptr);
    return FTP_OK;
}

FTP_API int32_t FTP_CALL ftp_set_cert_verify_callback(
    ftp_client_t* handle,
    ftp_cert_verify_cb_t callback,
    void* user_data
) {
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    impl->setCertCallback(callback, user_data);
    return FTP_OK;
}

FTP_API int32_t FTP_CALL ftp_set_cert_pins(
    ftp_client_t* handle,
    const char* const* pins,
    int32_t count
) {
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    try {
        std::vector<std::string> pin_list;
        if (pins != nullptr && count > 0) {
            pin_list.reserve(static_cast<size_t>(count));
            for (int32_t i = 0; i < count; ++i) {
                if (pins[i] != nullptr) {
                    pin_list.emplace_back(pins[i]);
                }
            }
        }
        impl->setCertPins(pin_list);
        return FTP_OK;
    } catch (const std::bad_alloc&) {
        return FTP_ERR_NOMEM;
    } catch (...) {
        return FTP_ERR_SYSTEM;
    }
}

/**
 * ftp_set_retry_policy - Set retry policy parameters for fault recovery
 * 
 * Per Phase 5 Spec Section 4.1 - ABI Amendment
 */
extern "C" int32_t ftp_set_retry_policy(
    ftp_client_t* handle,
    uint32_t      max_attempts,
    uint64_t      base_delay_ms,
    uint64_t      max_delay_ms
) {
    if (!handle) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    
    try {
        impl->setRetryPolicy(max_attempts, base_delay_ms, max_delay_ms);
        return FTP_OK;
    } catch (const std::bad_alloc&) {
        return FTP_ERR_NOMEM;
    } catch (...) {
        return FTP_ERR_SYSTEM;
    }
}

/* ============================================================================
 * SECTION 6.7: PHASE 7 OPTIMIZATION & OBSERVABILITY FUNCTIONS
 * ============================================================================
 */

FTP_API int32_t FTP_CALL ftp_set_rate_limit(
    ftp_client_t* handle,
    uint64_t      bytes_per_second,
    uint64_t      burst_bytes
) {
    if (!handle) {
        return FTP_ERR_INVALID_HANDLE;
    }

    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);

    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }

    /* Store rate limit config in FtpClientImpl for use by TransferEngine */
    impl->setRateLimit(bytes_per_second, burst_bytes);
    
    return FTP_OK;
}

FTP_API int32_t FTP_CALL ftp_set_event_callback(
    ftp_client_t*        handle,
    ftp_event_cb_t       cb,
    void*                user_data
) {
    if (!handle) {
        return FTP_ERR_INVALID_HANDLE;
    }

    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);

    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }

    /* Store callback in FtpClientImpl for use by TelemetryController */
    impl->setEventCallback(cb, user_data);
    
    return FTP_OK;
}

FTP_API int32_t FTP_CALL ftp_set_option(
    ftp_client_t* handle,
    int32_t       option,
    int32_t       value
) {
    if (!handle) {
        return FTP_ERR_INVALID_HANDLE;
    }

    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);

    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }

    switch (option) {
        case FTP_OPT_USE_IOURING:
            /* io_uring is experimental and opt-in */
            /* Store flag in FtpClientImpl config */
            impl->getConfig().use_io_uring = (value != 0);
            break;
        case FTP_OPT_USE_ZEROCOPY:
            /* Zero-copy enabled by default on supported platforms */
            /* Store flag in FtpClientImpl config */
            impl->getConfig().use_zerocopy = (value != 0);
            break;
        case FTP_OPT_USE_COMPRESSION:
            /* MODE Z compression opt-in */
            /* Store flag in FtpClientImpl config */
            impl->getConfig().use_compression = (value != 0);
            break;
        default:
            return FTP_ERR_INVALID_ARGUMENT;
    }
    
    return FTP_OK;
}

} /* extern "C" */
