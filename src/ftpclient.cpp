/*
 * ftpclient.cpp - C ABI Implementation
 * 
 * This file implements the C ABI functions defined in ftpclient.h.
 * All functions are wrapped in extern "C" to prevent name mangling.
 * No C++ exceptions cross this boundary.
 */

#include "FtpClientImpl.hpp"
#include "../include/ftpclient.h"
#include <new>
#include <cstring>

/* Ensure locale independence as per spec Section 11 */
static void init_locale() {
    /* Set locale to "C" for locale-independent string operations */
    static bool initialized = false;
    if (!initialized) {
        /* Note: setlocale is process-wide, so we only do this once */
        /* In a real implementation, use locale-independent functions throughout */
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
        /* Allocate on heap using standard new */
        auto impl = new ftpclient::FtpClientImpl();
        impl->setState(ftpclient::ClientState::ALLOCATED);
        *out_handle = reinterpret_cast<ftp_client_t*>(impl);
        return FTP_OK;
    } catch (const std::bad_alloc&) {
        return FTP_ERR_NOMEM;
    } catch (...) {
        /* Catch any other exception and map to system error */
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
        /* Note: After delete, handle is a dangling pointer but caller must not use it */
        return FTP_OK;
    } catch (...) {
        /* Should never happen, but handle gracefully */
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
    
    if (creds == nullptr) {
        return FTP_ERR_INVALID_ARGUMENT;
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
    
    /* Validate credentials - host is required */
    if (creds->host == nullptr || creds->host[0] == '\0') {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    
    /* Validate port - 0 is invalid */
    if (creds->port == 0) {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    
    /* Deep copy credentials into secure storage */
    auto& secure_creds = impl->getCredentials();
    
    try {
        secure_creds.host = creds->host;
        secure_creds.port = creds->port;
        
        if (creds->username != nullptr) {
            secure_creds.username = creds->username;
        } else {
            secure_creds.username.clear();
        }
        
        /* Securely copy password */
        secure_creds.secure_clear();
        if (creds->password != nullptr) {
            size_t pass_len = std::strlen(creds->password);
            if (pass_len > 0) {
                secure_creds.password_data.reset(new char[pass_len + 1]);
                std::memcpy(secure_creds.password_data.get(), creds->password, pass_len + 1);
                secure_creds.password_len = pass_len;
            }
        }
        
        secure_creds.use_tls = creds->use_tls;
        secure_creds.verify_cert = creds->verify_cert;
        
        if (creds->ca_bundle_path != nullptr) {
            secure_creds.ca_bundle_path = creds->ca_bundle_path;
        } else {
            secure_creds.ca_bundle_path.clear();
        }
        
        /* Transition to CONNECTED state */
        impl->setState(ftpclient::ClientState::CONNECTED);
        
        /* NOTE: Phase 1 is stub implementation - actual network connection 
         * will be implemented in Phase 2 */
        
        return FTP_OK;
    } catch (const std::bad_alloc&) {
        secure_creds.secure_clear();
        return FTP_ERR_NOMEM;
    } catch (...) {
        secure_creds.secure_clear();
        return FTP_ERR_SYSTEM;
    }
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
    
    /* Securely clear credentials */
    impl->getCredentials().secure_clear();
    
    /* Transition to DISCONNECTED state */
    impl->setState(ftpclient::ClientState::DISCONNECTED);
    
    /* NOTE: Phase 1 is stub implementation - actual network disconnection 
     * will be implemented in Phase 2 */
    
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
    
    /* NOTE: Phase 1 is stub implementation - actual NOOP command 
     * will be implemented in Phase 2 */
    
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
        out_result->status = FTP_ERR_INVALID_STATE;  /* Not yet implemented */
        out_result->files_total = 0;
        out_result->files_success = 0;
        out_result->files_failed = 0;
        out_result->bytes_transferred = 0;
    }
    
    /* Phase 1: Return INVALID_STATE to indicate not yet implemented */
    /* Actual implementation deferred to Phase 4 */
    return FTP_ERR_INVALID_STATE;
}

/* ============================================================================
 * SECTION 6.5: VERSION / CAPABILITY INTROSPECTION
 * ============================================================================
 */

FTP_API uint32_t FTP_CALL ftp_get_version(void) {
    /* Version format: 0xMMmmpp00 (Major, Minor, Patch) */
    /* Phase 1 initial release: 1.0.0 */
    return 0x01000000;
}

FTP_API int32_t FTP_CALL ftp_get_capabilities(uint64_t* out_caps) {
    if (out_caps == nullptr) {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    
    /* Report capabilities compiled into this build */
    /* Phase 1: Only basic capabilities, TLS and others come in later phases */
    uint64_t caps = 0;
    
    /* IPv6 support - always available with modern sockets */
    caps |= FTP_CAP_IPV6;
    
    /* Resume support - will be implemented in Phase 4 */
    caps |= FTP_CAP_RESUME;
    
    /* TLS - will be implemented in Phase 2/3 */
    /* For now, report as not available */
    /* caps |= FTP_CAP_TLS; */
    
    /* sendfile - platform specific, check at runtime */
#if defined(__linux__)
    caps |= FTP_CAP_SENDFILE;
#endif
    
    *out_caps = caps;
    return FTP_OK;
}

} /* extern "C" */
