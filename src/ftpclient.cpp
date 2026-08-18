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
#include <filesystem>
#include <functional>
#include <algorithm>

/* Ensure locale independence as per spec Section 11 */
static void init_locale() {
    /* Set locale to "C" for locale-independent string operations */
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
    }
}

static int32_t copy_string_to_buffer(const std::string& value,
                                      char* output, uint32_t output_size) {
    if (output == nullptr || output_size == 0) return FTP_ERR_INVALID_ARGUMENT;
    if (value.size() + 1 > output_size) return FTP_ERR_INVALID_ARGUMENT;
    std::memcpy(output, value.c_str(), value.size() + 1);
    return FTP_OK;
}

static bool path_is_within_root(const std::filesystem::path& root,
                                const std::filesystem::path& candidate) {
    std::error_code error;
    const auto root_canonical = std::filesystem::weakly_canonical(root, error);
    if (error) return false;
    error.clear();
    const auto candidate_canonical = std::filesystem::weakly_canonical(candidate, error);
    if (error) return false;
    auto root_it = root_canonical.begin();
    auto candidate_it = candidate_canonical.begin();
    for (; root_it != root_canonical.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate_canonical.end() || *root_it != *candidate_it) {
            return false;
        }
    }
    return true;
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
 * SECTION 6.4: TRANSFER OPERATIONS (M3 Single-File Upload)
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
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }

    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    if (local_path == nullptr || local_path[0] == '\0' ||
        remote_path == nullptr || remote_path[0] == '\0') {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    if (out_result != nullptr) {
        std::memset(out_result, 0, sizeof(*out_result));
    }

    if (impl->getState() != ftpclient::ClientState::CONNECTED) {
        return FTP_ERR_INVALID_STATE;
    }

    ftpclient::transfer::TransferConfig config;
    const auto& client_config = impl->getConfig();
    config.buffer_size = static_cast<uint32_t>(client_config.buffer_size);
    config.retry_attempts = client_config.retry_max_attempts;
    config.retry_base_delay_ms = client_config.retry_base_delay_ms;
    config.retry_max_delay_ms = client_config.retry_max_delay_ms;
    config.stall_timeout_ms = client_config.stall_timeout_ms;
    config.cancel_token = impl->getCancelToken();
    if (options != nullptr) {
        config.max_parallel = options->max_parallel > 0
            ? static_cast<uint32_t>(options->max_parallel) : 0;
        config.resume_enabled = options->resume_enabled;
        config.create_remote_dirs = options->create_remote_dirs;
        if (options->retry_attempts > 0) {
            config.retry_attempts = static_cast<uint32_t>(options->retry_attempts);
        }
        if (options->retry_base_delay_ms > 0) {
            config.retry_base_delay_ms = options->retry_base_delay_ms;
        }
        if (options->struct_size >= sizeof(ftp_upload_options_t) &&
            options->remote_chmod != nullptr) {
            /* chmod is intentionally deferred until a later milestone. */
        }
        if (options->struct_size >= offsetof(ftp_upload_options_t, expected_sha256) +
                                    sizeof(options->expected_sha256)) {
            config.expected_sha256 = options->expected_sha256 ? options->expected_sha256 : "";
        }
        if (options->struct_size >= offsetof(ftp_upload_options_t, stall_timeout_ms) +
                                    sizeof(options->stall_timeout_ms)) {
            config.stall_timeout_ms = options->stall_timeout_ms;
        }
        if (options->struct_size >= offsetof(ftp_upload_options_t, resume_metadata_enabled) +
                                    sizeof(options->resume_metadata_enabled)) {
            config.resume_metadata_enabled = options->resume_metadata_enabled;
        }
    }

    std::vector<ftpclient::transfer::FileResult> file_results;
    int32_t overall_status = FTP_OK;
    uint64_t total_bytes = 0;

    try {
        namespace fs = std::filesystem;
        fs::path local_fs(local_path);
        if (fs::is_regular_file(local_fs)) {
            ftpclient::transfer::TransferEngine engine(
                impl->getProtocolEngine(), config);
            overall_status = engine.upload_single_file(local_fs.string(), remote_path,
                                                       progress_cb, user_data);
            const auto& aggregator = engine.get_result_aggregator();
            file_results = aggregator.get_results();
            total_bytes = aggregator.get_bytes_transferred();
        } else if (fs::is_directory(local_fs)) {
            ftpclient::transfer::TransferEngine engine(
                impl->getProtocolEngine(), config);
            overall_status = engine.upload_directory(local_path, remote_path,
                                                      progress_cb, user_data);
            const auto& aggregator = engine.get_result_aggregator();
            file_results = aggregator.get_results();
            total_bytes = aggregator.get_bytes_transferred();
        } else {
            return FTP_ERR_INVALID_ARGUMENT;
        }

        if (out_result != nullptr) {
            out_result->status = overall_status;
            out_result->files_total = file_results.size();
            out_result->files_success = 0;
            out_result->files_failed = 0;
            out_result->bytes_transferred = total_bytes;
            out_result->file_result_count = file_results.size();

            if (!file_results.empty()) {
                out_result->file_results = new ftp_file_result_t[file_results.size()]();
                for (size_t i = 0; i < file_results.size(); ++i) {
                    const auto& source = file_results[i];
                    auto& target = out_result->file_results[i];
                    char* local_copy = new char[source.local_path.size() + 1];
                    char* remote_copy = new char[source.remote_path.size() + 1];
                    std::memcpy(local_copy, source.local_path.c_str(), source.local_path.size() + 1);
                    std::memcpy(remote_copy, source.remote_path.c_str(), source.remote_path.size() + 1);
                    target.local_path = local_copy;
                    target.remote_path = remote_copy;
                    target.status = source.status;
                    target.bytes_sent = source.bytes_sent;
                    target.attempt_count = source.attempt_count;
                    target.final_error = source.final_error;
                    if (source.status == FTP_OK) {
                        ++out_result->files_success;
                    } else {
                        ++out_result->files_failed;
                    }
                }
            }
        }
        return overall_status;
    } catch (const std::bad_alloc&) {
        if (out_result != nullptr) {
            ftp_result_free(out_result);
        }
        return FTP_ERR_NOMEM;
    } catch (...) {
        if (out_result != nullptr) {
            ftp_result_free(out_result);
        }
        return FTP_ERR_SYSTEM;
    }
}

FTP_API int32_t FTP_CALL ftp_download_file(
    ftp_client_t* handle,
    const char* local_path,
    const char* remote_path,
    ftp_progress_cb_t progress_cb,
    void* user_data,
    ftp_result_t* out_result
) {
    return ftp_download_file_ex(handle, local_path, remote_path, nullptr,
                                 progress_cb, user_data, out_result);
}

FTP_API int32_t FTP_CALL ftp_download_file_ex(
    ftp_client_t* handle,
    const char* local_path,
    const char* remote_path,
    const ftp_download_options_t* options,
    ftp_progress_cb_t progress_cb,
    void* user_data,
    ftp_result_t* out_result
) {
    if (handle == nullptr) {
        return FTP_ERR_INVALID_HANDLE;
    }
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    if (!impl->isValid()) {
        return FTP_ERR_INVALID_HANDLE;
    }
    if (local_path == nullptr || local_path[0] == '\0' ||
        remote_path == nullptr || remote_path[0] == '\0') {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    if (out_result != nullptr) {
        std::memset(out_result, 0, sizeof(*out_result));
    }
    if (impl->getState() != ftpclient::ClientState::CONNECTED) {
        return FTP_ERR_INVALID_STATE;
    }

    impl->clearCancellation();
    ftpclient::protocol::TransferOptions transfer_options;
    transfer_options.cancel_token = impl->getCancelToken();
    if (options != nullptr) {
        if (options->struct_size >= offsetof(ftp_download_options_t, stall_timeout_ms) +
                                    sizeof(options->stall_timeout_ms)) {
            transfer_options.stall_timeout_ms = options->stall_timeout_ms;
        }
        if (options->struct_size >= offsetof(ftp_download_options_t, expected_sha256) +
                                    sizeof(options->expected_sha256)) {
            transfer_options.expected_sha256 = options->expected_sha256 ? options->expected_sha256 : "";
        }
        if (options->struct_size >= offsetof(ftp_download_options_t, resume_enabled) +
                                    sizeof(options->resume_enabled)) {
            transfer_options.resume_enabled = options->resume_enabled != 0;
        }
        if (options->struct_size >= offsetof(ftp_download_options_t, resume_metadata_enabled) +
                                    sizeof(options->resume_metadata_enabled) &&
            options->resume_metadata_enabled != 0) {
            transfer_options.resume_metadata_enabled = true;
        }
        if (options->struct_size >= offsetof(ftp_download_options_t, resume_allow_unverified) +
                                    sizeof(options->resume_allow_unverified) &&
            options->resume_allow_unverified != 0) {
            transfer_options.resume_metadata_enabled = false;
            transfer_options.resume_allow_unverified = true;
        }
    }

    try {
        ftpclient::protocol::FileManifestEntry entry;
        entry.local_absolute_path = local_path;
        entry.remote_relative_path = remote_path;
        entry.is_directory = false;

        ftpclient::protocol::ProtocolEngine::ProgressCallback progress;
        if (progress_cb != nullptr) {
            progress = [progress_cb, local = std::string(local_path),
                        remote = std::string(remote_path), user_data]
                       (uint64_t current, uint64_t total) {
                progress_cb(local.c_str(), remote.c_str(), current, total,
                            0.0, user_data);
            };
        }

        uint64_t bytes_received = 0;
        int32_t status = impl->getProtocolEngine().download_file(
            entry, &bytes_received, progress, transfer_options);
        if (out_result != nullptr) {
            out_result->status = status;
            out_result->files_total = 1;
            out_result->files_success = status == FTP_OK ? 1 : 0;
            out_result->files_failed = status == FTP_OK ? 0 : 1;
            out_result->bytes_transferred = bytes_received;
            out_result->file_result_count = 1;
            out_result->file_results = new ftp_file_result_t[1]();
            char* local_copy = new char[std::strlen(local_path) + 1];
            char* remote_copy = new char[std::strlen(remote_path) + 1];
            std::strcpy(local_copy, local_path);
            std::strcpy(remote_copy, remote_path);
            out_result->file_results[0].local_path = local_copy;
            out_result->file_results[0].remote_path = remote_copy;
            out_result->file_results[0].status = status;
            out_result->file_results[0].bytes_sent = bytes_received;
            out_result->file_results[0].attempt_count = 1;
            out_result->file_results[0].final_error = status;
        }
        return status;
    } catch (const std::bad_alloc&) {
        if (out_result != nullptr) {
            ftp_result_free(out_result);
        }
        return FTP_ERR_NOMEM;
    } catch (...) {
        if (out_result != nullptr) {
            ftp_result_free(out_result);
        }
        return FTP_ERR_SYSTEM;
    }
}

FTP_API int32_t FTP_CALL ftp_download_dir(
    ftp_client_t* handle,
    const char* local_path,
    const char* remote_path,
    const ftp_download_options_t* options,
    ftp_progress_cb_t progress_cb,
    void* user_data,
    ftp_result_t* out_result
) {
    if (handle == nullptr) return FTP_ERR_INVALID_HANDLE;
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    if (!impl->isValid()) return FTP_ERR_INVALID_HANDLE;
    if (local_path == nullptr || local_path[0] == '\0' ||
        remote_path == nullptr || remote_path[0] == '\0') {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    if (out_result != nullptr) std::memset(out_result, 0, sizeof(*out_result));
    if (impl->getState() != ftpclient::ClientState::CONNECTED) return FTP_ERR_INVALID_STATE;

    impl->clearCancellation();
    ftpclient::protocol::TransferOptions transfer_options;
    transfer_options.cancel_token = impl->getCancelToken();
    if (options != nullptr) {
        if (options->struct_size >= offsetof(ftp_download_options_t, stall_timeout_ms) +
                                    sizeof(options->stall_timeout_ms)) {
            transfer_options.stall_timeout_ms = options->stall_timeout_ms;
        }
        if (options->struct_size >= offsetof(ftp_download_options_t, expected_sha256) +
                                    sizeof(options->expected_sha256)) {
            transfer_options.expected_sha256 = options->expected_sha256 ? options->expected_sha256 : "";
        }
        if (options->struct_size >= offsetof(ftp_download_options_t, resume_enabled) +
                                    sizeof(options->resume_enabled)) {
            transfer_options.resume_enabled = options->resume_enabled != 0;
        }
        if (options->struct_size >= offsetof(ftp_download_options_t, resume_metadata_enabled) +
                                    sizeof(options->resume_metadata_enabled) &&
            options->resume_metadata_enabled != 0) {
            transfer_options.resume_metadata_enabled = true;
        }
        if (options->struct_size >= offsetof(ftp_download_options_t, resume_allow_unverified) +
                                    sizeof(options->resume_allow_unverified) &&
            options->resume_allow_unverified != 0) {
            transfer_options.resume_metadata_enabled = false;
            transfer_options.resume_allow_unverified = true;
        }
    }
    const ftp_download_digest_t* file_digests = nullptr;
    uint32_t file_digest_count = 0;
    if (options != nullptr &&
        options->struct_size >= offsetof(ftp_download_options_t, file_digests) +
                                sizeof(options->file_digests)) {
        file_digests = options->file_digests;
    }
    if (options != nullptr &&
        options->struct_size >= offsetof(ftp_download_options_t, file_digest_count) +
                                sizeof(options->file_digest_count)) {
        file_digest_count = options->file_digest_count;
    }
    if (!transfer_options.expected_sha256.empty() &&
        (file_digests == nullptr || file_digest_count == 0)) {
        return FTP_ERR_INVALID_ARGUMENT;
    }

    struct PendingResult {
        std::string local_path;
        std::string remote_path;
        int32_t status = FTP_OK;
        uint64_t bytes = 0;
    };
    std::vector<PendingResult> pending;
    namespace fs = std::filesystem;
    int32_t overall_status = FTP_OK;
    uint64_t total_bytes = 0;

    try {
        fs::path local_root(local_path);
        std::error_code fs_error;
        fs::create_directories(local_root, fs_error);
        if (fs_error) return FTP_ERR_LOCAL_IO;

        ftpclient::protocol::ProtocolEngine::ProgressCallback progress;
        if (progress_cb != nullptr) {
            progress = [progress_cb, user_data](uint64_t current, uint64_t total) {
                progress_cb(nullptr, nullptr, current, total, 0.0, user_data);
            };
        }

        std::function<int32_t(const std::string&, const fs::path&)> walk;
        walk = [&](const std::string& remote_dir, const fs::path& local_dir) -> int32_t {
            if (impl->getCancelToken() &&
                impl->getCancelToken()->load(std::memory_order_acquire)) {
                return FTP_ERR_CANCELLED;
            }
            std::vector<ftpclient::protocol::RemoteListingEntry> entries;
            int32_t list_status = impl->getProtocolEngine().list_directory(
                remote_dir, entries, transfer_options);
            if (list_status != FTP_OK) return list_status;
            std::sort(entries.begin(), entries.end(),
                      [](const auto& left, const auto& right) {
                          return left.name < right.name;
                      });
            for (const auto& remote_entry : entries) {
                const std::string child_remote =
                    remote_dir == "/" ? "/" + remote_entry.name
                                       : remote_dir + "/" + remote_entry.name;
                const fs::path child_local = local_dir / remote_entry.name;
                if (!path_is_within_root(local_root, child_local)) {
                    return FTP_ERR_INVALID_ARGUMENT;
                }
                if (remote_entry.type == "dir") {
                    fs::create_directories(child_local, fs_error);
                    if (fs_error) return FTP_ERR_LOCAL_IO;
                    const int32_t nested_status = walk(child_remote, child_local);
                    if (nested_status != FTP_OK) return nested_status;
                    continue;
                }
                ftpclient::protocol::FileManifestEntry file_entry;
                file_entry.local_absolute_path = child_local.string();
                file_entry.remote_relative_path = child_remote;
                file_entry.size_bytes = remote_entry.has_size ? remote_entry.size_bytes : 0;
                ftpclient::protocol::TransferOptions file_options = transfer_options;
                if (file_digests != nullptr && file_digest_count > 0) {
                    const ftp_download_digest_t* matching_digest = nullptr;
                    for (uint32_t digest_index = 0; digest_index < file_digest_count; ++digest_index) {
                        const auto& digest = file_digests[digest_index];
                        if (digest.remote_path != nullptr &&
                            child_remote == digest.remote_path) {
                            matching_digest = &digest;
                            break;
                        }
                    }
                    if (matching_digest == nullptr || matching_digest->sha256 == nullptr) {
                        return FTP_ERR_INVALID_ARGUMENT;
                    }
                    file_options.expected_sha256 = matching_digest->sha256;
                }
                uint64_t received = 0;
                ftpclient::protocol::ProtocolEngine::ProgressCallback file_progress;
                if (progress_cb != nullptr) {
                    file_progress = [progress_cb, local = child_local.string(),
                                     remote = child_remote, user_data]
                                    (uint64_t current, uint64_t total) {
                        progress_cb(local.c_str(), remote.c_str(), current, total,
                                    0.0, user_data);
                    };
                }
                const int32_t file_status = impl->getProtocolEngine().download_file(
                    file_entry, &received, file_progress, file_options);
                pending.push_back({child_local.string(), child_remote, file_status, received});
                total_bytes += received;
                if (file_status != FTP_OK && overall_status == FTP_OK) {
                    overall_status = file_status;
                }
                if (file_status == FTP_ERR_CANCELLED || file_status == FTP_ERR_STALLED) {
                    return file_status;
                }
            }
            return FTP_OK;
        };

        overall_status = walk(remote_path, local_root);
        if (out_result != nullptr) {
            out_result->status = overall_status;
            out_result->files_total = pending.size();
            out_result->files_success = 0;
            out_result->files_failed = 0;
            out_result->bytes_transferred = total_bytes;
            out_result->file_result_count = pending.size();
            if (!pending.empty()) {
                out_result->file_results = new ftp_file_result_t[pending.size()]();
                for (size_t i = 0; i < pending.size(); ++i) {
                    const auto& item = pending[i];
                    char* local_copy = new char[item.local_path.size() + 1];
                    char* remote_copy = new char[item.remote_path.size() + 1];
                    std::strcpy(local_copy, item.local_path.c_str());
                    std::strcpy(remote_copy, item.remote_path.c_str());
                    out_result->file_results[i].local_path = local_copy;
                    out_result->file_results[i].remote_path = remote_copy;
                    out_result->file_results[i].status = item.status;
                    out_result->file_results[i].bytes_sent = item.bytes;
                    out_result->file_results[i].attempt_count = 1;
                    out_result->file_results[i].final_error = item.status;
                    if (item.status == FTP_OK) ++out_result->files_success;
                    else ++out_result->files_failed;
                }
            }
        }
        return overall_status;
    } catch (const std::bad_alloc&) {
        if (out_result != nullptr) ftp_result_free(out_result);
        return FTP_ERR_NOMEM;
    } catch (...) {
        if (out_result != nullptr) ftp_result_free(out_result);
        return FTP_ERR_SYSTEM;
    }
}

FTP_API int32_t FTP_CALL ftp_cancel(ftp_client_t* handle) {
    if (handle == nullptr) return FTP_ERR_INVALID_HANDLE;
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    if (!impl->isValid()) return FTP_ERR_INVALID_HANDLE;
    if (impl->getState() != ftpclient::ClientState::CONNECTED) return FTP_ERR_INVALID_STATE;
    impl->requestCancellation();
    return FTP_OK;
}

FTP_API int32_t FTP_CALL ftp_clear_cancel(ftp_client_t* handle) {
    if (handle == nullptr) return FTP_ERR_INVALID_HANDLE;
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    if (!impl->isValid()) return FTP_ERR_INVALID_HANDLE;
    impl->clearCancellation();
    return FTP_OK;
}

FTP_API int32_t FTP_CALL ftp_change_directory(ftp_client_t* handle, const char* remote_path) {
    if (handle == nullptr) return FTP_ERR_INVALID_HANDLE;
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    if (!impl->isValid()) return FTP_ERR_INVALID_HANDLE;
    if (remote_path == nullptr || remote_path[0] == '\0') return FTP_ERR_INVALID_ARGUMENT;
    if (impl->getState() != ftpclient::ClientState::CONNECTED) return FTP_ERR_INVALID_STATE;
    return impl->getProtocolEngine().change_directory(remote_path);
}

FTP_API int32_t FTP_CALL ftp_parent_directory(ftp_client_t* handle) {
    if (handle == nullptr) return FTP_ERR_INVALID_HANDLE;
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    if (!impl->isValid()) return FTP_ERR_INVALID_HANDLE;
    if (impl->getState() != ftpclient::ClientState::CONNECTED) return FTP_ERR_INVALID_STATE;
    return impl->getProtocolEngine().parent_directory();
}

FTP_API int32_t FTP_CALL ftp_delete_remote_file(ftp_client_t* handle, const char* remote_path) {
    if (handle == nullptr) return FTP_ERR_INVALID_HANDLE;
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    if (!impl->isValid()) return FTP_ERR_INVALID_HANDLE;
    if (remote_path == nullptr || remote_path[0] == '\0') return FTP_ERR_INVALID_ARGUMENT;
    if (impl->getState() != ftpclient::ClientState::CONNECTED) return FTP_ERR_INVALID_STATE;
    return impl->getProtocolEngine().delete_remote_file(remote_path);
}

FTP_API int32_t FTP_CALL ftp_remove_remote_directory(ftp_client_t* handle, const char* remote_path) {
    if (handle == nullptr) return FTP_ERR_INVALID_HANDLE;
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    if (!impl->isValid()) return FTP_ERR_INVALID_HANDLE;
    if (remote_path == nullptr || remote_path[0] == '\0') return FTP_ERR_INVALID_ARGUMENT;
    if (impl->getState() != ftpclient::ClientState::CONNECTED) return FTP_ERR_INVALID_STATE;
    return impl->getProtocolEngine().remove_remote_dir(remote_path);
}

FTP_API int32_t FTP_CALL ftp_rename_remote(ftp_client_t* handle,
                                           const char* from_path,
                                           const char* to_path) {
    if (handle == nullptr) return FTP_ERR_INVALID_HANDLE;
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    if (!impl->isValid()) return FTP_ERR_INVALID_HANDLE;
    if (from_path == nullptr || from_path[0] == '\0' ||
        to_path == nullptr || to_path[0] == '\0') return FTP_ERR_INVALID_ARGUMENT;
    if (impl->getState() != ftpclient::ClientState::CONNECTED) return FTP_ERR_INVALID_STATE;
    return impl->getProtocolEngine().rename_remote_path(from_path, to_path);
}

FTP_API int32_t FTP_CALL ftp_get_server_capabilities(
    ftp_client_t* handle, ftp_server_capabilities_t* out_caps) {
    if (handle == nullptr) return FTP_ERR_INVALID_HANDLE;
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    if (!impl->isValid()) return FTP_ERR_INVALID_HANDLE;
    if (out_caps == nullptr || out_caps->struct_size < sizeof(uint32_t)) {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    const int32_t status = impl->getProtocolEngine().refresh_server_capabilities();
    if (status != FTP_OK) return status;
    const auto& capabilities = impl->getProtocolEngine().get_server_capabilities();
    const uint32_t caller_size = std::min<uint32_t>(out_caps->struct_size,
                                                     sizeof(*out_caps));
    std::memset(out_caps, 0, caller_size);
    out_caps->struct_size = caller_size;
#define FTP_SET_SERVER_CAP_FIELD(field, value) \
    do { \
        if (caller_size >= offsetof(ftp_server_capabilities_t, field) + sizeof(out_caps->field)) \
            out_caps->field = (value); \
    } while (0)
    FTP_SET_SERVER_CAP_FIELD(feat_supported, capabilities.feat_supported ? 1 : 0);
    FTP_SET_SERVER_CAP_FIELD(size_supported, capabilities.size_supported ? 1 : 0);
    FTP_SET_SERVER_CAP_FIELD(mdtm_supported, capabilities.mdtm_supported ? 1 : 0);
    FTP_SET_SERVER_CAP_FIELD(hash_supported, capabilities.hash_supported ? 1 : 0);
    uint32_t algorithms = 0;
    if (capabilities.hash_md5) algorithms |= FTP_HASH_ALG_MD5;
    if (capabilities.hash_sha1) algorithms |= FTP_HASH_ALG_SHA1;
    if (capabilities.hash_sha256) algorithms |= FTP_HASH_ALG_SHA256;
    if (capabilities.hash_sha512) algorithms |= FTP_HASH_ALG_SHA512;
    FTP_SET_SERVER_CAP_FIELD(hash_algorithms, algorithms);
#undef FTP_SET_SERVER_CAP_FIELD
    return FTP_OK;
}

FTP_API int32_t FTP_CALL ftp_get_remote_file_mdtm(
    ftp_client_t* handle, const char* remote_path,
    char* out_modify, uint32_t out_size) {
    if (handle == nullptr) return FTP_ERR_INVALID_HANDLE;
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    if (!impl->isValid()) return FTP_ERR_INVALID_HANDLE;
    if (remote_path == nullptr || remote_path[0] == '\0' ||
        out_modify == nullptr || out_size == 0) return FTP_ERR_INVALID_ARGUMENT;
    if (impl->getState() != ftpclient::ClientState::CONNECTED) return FTP_ERR_INVALID_STATE;
    std::string modify;
    const int32_t status = impl->getProtocolEngine().get_remote_file_mdtm(remote_path, &modify);
    if (status != FTP_OK) return status;
    return copy_string_to_buffer(modify, out_modify, out_size);
}

FTP_API int32_t FTP_CALL ftp_get_remote_file_hash(
    ftp_client_t* handle, const char* remote_path,
    const char* algorithm, char* out_hash, uint32_t out_size) {
    if (handle == nullptr) return FTP_ERR_INVALID_HANDLE;
    auto impl = reinterpret_cast<ftpclient::FtpClientImpl*>(handle);
    if (!impl->isValid()) return FTP_ERR_INVALID_HANDLE;
    if (remote_path == nullptr || remote_path[0] == '\0' ||
        algorithm == nullptr || algorithm[0] == '\0' ||
        out_hash == nullptr || out_size == 0) return FTP_ERR_INVALID_ARGUMENT;
    if (impl->getState() != ftpclient::ClientState::CONNECTED) return FTP_ERR_INVALID_STATE;
    std::string hash;
    const int32_t status = impl->getProtocolEngine().get_remote_file_hash(
        remote_path, algorithm, &hash);
    if (status != FTP_OK) return status;
    return copy_string_to_buffer(hash, out_hash, out_size);
}

FTP_API int32_t FTP_CALL ftp_result_free(ftp_result_t* result) {
    if (result == nullptr) {
        return FTP_ERR_INVALID_ARGUMENT;
    }
    if (result->file_results != nullptr) {
        for (uint64_t i = 0; i < result->file_result_count; ++i) {
            delete[] result->file_results[i].local_path;
            delete[] result->file_results[i].remote_path;
        }
        delete[] result->file_results;
    }
    std::memset(result, 0, sizeof(*result));
    return FTP_OK;
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
     * M4 truthfulness rule: control and passive data paths are public only
     * because they are wired and tested through the exported execution path.
     */
    /* M4 exposes tested control, passive data, protected data, and REST resume. */
    *out_caps = FTP_CAP_CONTROL_FTP | FTP_CAP_TLS |
                FTP_CAP_RESUME | FTP_CAP_DATA_FTP | FTP_CAP_DATA_FTPS |
                FTP_CAP_INTEGRITY;
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
