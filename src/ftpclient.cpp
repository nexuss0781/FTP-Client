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
    
    /* Store credentials in secure vault */
    auto& vault = impl->getVault();
    int32_t ret = vault.store(creds_to_use);
    if (ret != 0) {
        return ret;
    }
    
    /* Transition to CONNECTED state */
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
    
    /* Can only disconnect from CONNECTED state */
    auto state = impl->getState();
    if (state != ftpclient::ClientState::CONNECTED) {
        /* Already disconnected or in wrong state - treat as success for idempotency */
        return FTP_OK;
    }
    
    /* Securely purge credentials from vault */
    impl->getVault().purge();
    
    /* Transition to DISCONNECTED state */
    impl->setState(ftpclient::ClientState::DISCONNECTED);
    
    return FTP_OK;
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
    
    return FTP_OK;
}

/* ============================================================================
 * SECTION 6.4: TRANSFER OPERATIONS (Phase 1 Stub)
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
    
    /* Must be connected to upload */
    auto state = impl->getState();
    if (state != ftpclient::ClientState::CONNECTED) {
        return FTP_ERR_INVALID_STATE;
    }
    
    /* Initialize result if provided */
    if (out_result != nullptr) {
        out_result->status = FTP_ERR_INVALID_STATE;
        out_result->files_total = 0;
        out_result->files_success = 0;
        out_result->files_failed = 0;
        out_result->bytes_transferred = 0;
    }
    
    return FTP_ERR_INVALID_STATE;
}

/* ============================================================================
 * SECTION 6.5: VERSION / CAPABILITY INTROSPECTION
 * ============================================================================
 */

FTP_API uint32_t FTP_CALL ftp_get_version(void) {
    /* Version format: 0xMMmmpp00 (Major, Minor, Patch) */
    /* Phase 3 release: 1.0.0 */
    return 0x01000000;
}

FTP_API int32_t FTP_CALL ftp_get_capabilities(uint64_t* out_caps) {
    if (out_caps == nullptr) {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    
    /* Report capabilities compiled into this build */
    uint64_t caps = 0;
    
    /* IPv6 support - always available with modern sockets */
    caps |= FTP_CAP_IPV6;
    
    /* Resume support - will be implemented in Phase 4 */
    caps |= FTP_CAP_RESUME;
    
    /* TLS - Phase 3 implemented */
    caps |= FTP_CAP_TLS;
    
    /* sendfile - platform specific, check at runtime */
#if defined(__linux__)
    caps |= FTP_CAP_SENDFILE;
#endif
    
    *out_caps = caps;
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

} /* extern "C" */
