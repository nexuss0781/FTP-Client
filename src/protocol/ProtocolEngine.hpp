/*
 * ProtocolEngine.hpp - FTP Protocol Engine Facade
 * 
 * Internal C++ API exposed to Phase 3/4/5.
 * Integrates Transport, ControlThread, DataChannel, and DirectoryWalker.
 * Per Phase 2 spec Section 11.
 */

#ifndef FTPCLIENT_PROTOCOL_ENGINE_HPP
#define FTPCLIENT_PROTOCOL_ENGINE_HPP

#include "Transport.hpp"
#include "PlainTransport.hpp"
#include "ControlThread.hpp"
#include "DataChannel.hpp"
#include "DirectoryWalker.hpp"
#include "ErrorMap.hpp"
#include "ReplyParser.hpp"
#include "../security/TlsTransport.hpp"
#include "../security/OpenSSLInit.hpp"
#include "Integrity.hpp"
#include <memory>
#include <string>
#include <cstring>
#include <fstream>
#include <functional>
#include <filesystem>
#include <system_error>
#include <atomic>
#include <chrono>
#include <algorithm>

namespace ftpclient { namespace protocol {

/**
 * Protocol Engine Configuration
 */
struct ProtocolEngineConfig {
    uint64_t buffer_size;           // Default: 256KB
    uint32_t timeout_connect_ms;    // Default: 5000ms
    uint32_t timeout_command_ms;    // Default: 30000ms
    
    ProtocolEngineConfig()
        : buffer_size(256 * 1024)
        , timeout_connect_ms(5000)
        , timeout_command_ms(30000)
    {}
};

/**
 * Connection credentials (internal representation)
 */
struct ConnectionCredentials {
    std::string host;
    uint16_t port;
    std::string username;
    std::string password;
    int32_t use_tls;
    int32_t verify_cert;
    std::string ca_bundle_path;
    
    ConnectionCredentials()
        : port(21)
        , use_tls(0)
        , verify_cert(2)
    {}
};

struct TransferOptions {
    std::shared_ptr<std::atomic<bool>> cancel_token;
    uint32_t stall_timeout_ms = 0;
    bool resume_enabled = false;
    bool resume_metadata_enabled = false;
    std::string expected_sha256;
};

/**
 * Protocol Engine
 * 
 * Main facade for FTP protocol operations.
 * This class is the primary interface used by the C ABI layer (ftpclient.cpp).
 * 
 * Thread Safety: This class is NOT thread-safe. The caller must ensure
 * serialized access from a single thread.
 */
class ProtocolEngine {
public:
    ProtocolEngine();
    ~ProtocolEngine();
    
    // Prevent copying
    ProtocolEngine(const ProtocolEngine&) = delete;
    ProtocolEngine& operator=(const ProtocolEngine&) = delete;
    
    /* ========================================================================
     * Phase 2 Core Functions
     * ========================================================================
     */
    
    /**
     * Establish control connection and authenticate
     * 
     * @param creds Connection credentials
     * @return 0 on success, negative error code on failure
     */
    int32_t connect(const ConnectionCredentials& creds);
    
    /**
     * Disconnect from server gracefully
     * 
     * @return 0 on success, negative error code on failure
     */
    int32_t disconnect();
    
    /**
     * Send NOOP keep-alive
     * 
     * @return 0 on success, negative error code on failure
     */
    int32_t ping();
    
    /**
     * Check if connected and authenticated
     */
    bool is_connected() const;
    
    /**
     * Check if authenticated
     */
    bool is_authenticated() const;
    
    /* ========================================================================
     * Phase 2: Directory Traversal
     * ========================================================================
     */
    
    /**
     * Traverse local directory tree
     * 
     * @param local_path Local directory path
     * @param remote_path Remote base path
     * @param config Traversal configuration
     * @param[out] manifest Output manifest
     * @return WalkError code
     */
    WalkError traverse_local(const std::string& local_path, 
                             const std::string& remote_path,
                             const TraversalConfig& config,
                             FileManifest& manifest);
    
    /* ========================================================================
     * Phase 3 Extension Points (stubs for now)
     * ========================================================================
     */
    
    /**
     * Set transport factory for TLS injection (Phase 3)
     * 
     * @param factory Function pointer to create Transport instances
     */
    void set_control_transport_factory(TransportFactory factory);
    
    /**
     * Enable TLS mode (Phase 3)
     * 
     * @param use_tls 0=none, 1=explicit, 2=implicit
     */
    void set_tls_mode(int32_t use_tls);
    
    /* ========================================================================
     * Phase 4 Extension Points (stubs for now)
     * ========================================================================
     */
    
    /**
     * Upload a single file (Phase 4)
     * 
     * @param entry File manifest entry
     * @return 0 on success, negative error code on failure
     */
    using ProgressCallback = std::function<void(uint64_t bytes_current, uint64_t bytes_total)>;

    int32_t upload_file(const FileManifestEntry& entry,
                        uint64_t* out_bytes_sent = nullptr,
                        const ProgressCallback& progress = ProgressCallback(),
                        uint64_t restart_offset = 0,
                        const TransferOptions& options = TransferOptions());
    
    /**
     * Download a single file (Phase 4)
     * 
     * @param entry File manifest entry
     * @return 0 on success, negative error code on failure
     */
    int32_t download_file(const FileManifestEntry& entry,
                          uint64_t* out_bytes_received = nullptr,
                          const ProgressCallback& progress = ProgressCallback(),
                          const TransferOptions& options = TransferOptions());
    
    /**
     * Create remote directory (Phase 4)
     * 
     * @param remote_path Remote directory path
     * @return 0 on success, negative error code on failure
     */
    int32_t create_remote_dir(const std::string& remote_path);
    int32_t change_directory(const std::string& remote_path);
    int32_t parent_directory();
    int32_t delete_remote_file(const std::string& remote_path);
    int32_t remove_remote_dir(const std::string& remote_path);
    int32_t rename_remote_path(const std::string& from_path,
                               const std::string& to_path);

    /** Query a remote file size; returns -502 when the file is absent. */
    int32_t get_remote_file_size(const std::string& remote_path, uint64_t* out_size);
    
    /* ========================================================================
     * Configuration
     * ========================================================================
     */
    
    /**
     * Get current configuration
     */
    const ProtocolEngineConfig& get_config() const { return config_; }
    
    /**
     * Get mutable configuration
     */
    ProtocolEngineConfig& get_config() { return config_; }

    /** Get a read-only copy source for worker-session construction. */
    const ConnectionCredentials& get_credentials() const { return creds_; }

    /**
     * Set command timeout
     */
    void set_command_timeout(uint32_t ms);

    /** Share a cooperative cancellation token with this protocol session. */
    void set_cancel_token(const std::shared_ptr<std::atomic<bool>>& token) {
        cancel_token_ = token;
    }

    bool is_cancelled() const {
        return cancel_token_ && cancel_token_->load(std::memory_order_acquire);
    }

private:
    /**
     * Perform FTP authentication sequence
     */
    int32_t authenticate(const std::string& username, const std::string& password);
    
    /**
     * Create appropriate transport based on TLS settings
     */
    std::unique_ptr<Transport> create_transport();
    
    ProtocolEngineConfig config_;
    ConnectionCredentials creds_;
    std::unique_ptr<Transport> control_transport_;
    std::unique_ptr<ControlThread> control_thread_;
    TransportFactory transport_factory_;
    bool use_tls_;
    std::shared_ptr<std::atomic<bool>> cancel_token_;

    void abort_control_session() {
        if (control_thread_) {
            control_thread_->stop();
            control_thread_.reset();
        }
        control_thread_ = std::make_unique<ControlThread>();
        control_transport_.reset();
    }
};

inline ProtocolEngine::ProtocolEngine()
    : control_thread_(std::make_unique<ControlThread>())
    , transport_factory_(nullptr)
    , use_tls_(false)
    , cancel_token_(std::make_shared<std::atomic<bool>>(false))
{
}

inline ProtocolEngine::~ProtocolEngine() {
    disconnect();
}

inline int32_t ProtocolEngine::connect(const ConnectionCredentials& creds) {
    if (is_connected()) {
        return -203;  // FTP_ERR_INVALID_STATE
    }
    if (creds.host.empty() || creds.port == 0) {
        return -202;  // FTP_ERR_INVALID_ARGUMENT
    }
    if (creds.use_tls == 2) {
        return -204;  // Implicit FTPS is a later milestone
    }
    if (creds.use_tls != 0 && creds.use_tls != 1) {
        return -202;  // FTP_ERR_INVALID_ARGUMENT
    }

    creds_ = creds;
    auto plain = std::make_unique<PlainTransport>();
    plain->set_timeouts(config_.timeout_connect_ms, config_.timeout_command_ms);
    int32_t ret = plain->connect(creds.host.c_str(), creds.port);
    if (ret != 0) {
        return ret;
    }

    if (creds.use_tls == 1) {
        // Explicit FTPS starts with the normal FTP greeting on the plaintext
        // control socket, followed by AUTH TLS.
        std::string greeting_buffer;
        greeting_buffer.reserve(4096);
        FtpReply greeting_reply;
        size_t greeting_consumed = 0;
        char greeting_read_buffer[4096];
        for (;;) {
            int32_t greeting_read = plain->read(greeting_read_buffer, sizeof(greeting_read_buffer));
            if (greeting_read <= 0) {
                return greeting_read == 0 ? -403 : greeting_read;
            }
            greeting_buffer.append(greeting_read_buffer, static_cast<size_t>(greeting_read));
            ParseResult greeting_parsed = ReplyParser::parse(
                greeting_buffer.data(), greeting_buffer.size(), greeting_reply, greeting_consumed);
            if (greeting_parsed == ParseResult::COMPLETE) {
                break;
            }
            if (greeting_parsed == ParseResult::MALFORMED || greeting_parsed == ParseResult::BUFFER_OVERFLOW) {
                return -501;
            }
        }
        while (greeting_reply.code >= 100 && greeting_reply.code < 200) {
            greeting_buffer.clear();
            greeting_consumed = 0;
            int32_t greeting_read = plain->read(greeting_read_buffer, sizeof(greeting_read_buffer));
            if (greeting_read <= 0) {
                return greeting_read == 0 ? -403 : greeting_read;
            }
            greeting_buffer.append(greeting_read_buffer, static_cast<size_t>(greeting_read));
            ParseResult greeting_parsed = ReplyParser::parse(
                greeting_buffer.data(), greeting_buffer.size(), greeting_reply, greeting_consumed);
            if (greeting_parsed != ParseResult::COMPLETE) {
                return -501;
            }
        }
        if (greeting_reply.code < 200 || greeting_reply.code >= 400) {
            return map_ftp_code_to_error(greeting_reply.code);
        }

        // Explicit FTPS: negotiate AUTH TLS on the plaintext control socket.
        const char auth_tls[] = "AUTH TLS\r\n";
        if (plain->write(auth_tls, static_cast<uint32_t>(sizeof(auth_tls) - 1)) < 0) {
            return -401;  // FTP_ERR_CONNECT
        }

        std::string response_buffer;
        response_buffer.reserve(4096);
        FtpReply reply;
        size_t consumed = 0;
        char buffer[4096];
        for (;;) {
            int32_t read_count = plain->read(buffer, sizeof(buffer));
            if (read_count <= 0) {
                return read_count == 0 ? -403 : read_count;
            }
            response_buffer.append(buffer, static_cast<size_t>(read_count));
            ParseResult parsed = ReplyParser::parse(response_buffer.data(), response_buffer.size(), reply, consumed);
            if (parsed == ParseResult::COMPLETE) {
                break;
            }
            if (parsed == ParseResult::MALFORMED || parsed == ParseResult::BUFFER_OVERFLOW) {
                return -501;  // FTP_ERR_PROTOCOL
            }
        }
        if (reply.code != 234) {
            return -302;  // FTP_ERR_AUTH_TLS_REQUIRED
        }

        security::TlsConfig tls_config;
        tls_config.server_name = creds.host;
        tls_config.ca_bundle_path = creds.ca_bundle_path;
        if (creds.verify_cert == 0) {
            tls_config.verify_mode = security::CertVerifyMode::NONE;
        } else if (creds.verify_cert == 1) {
            tls_config.verify_mode = security::CertVerifyMode::PEER;
        } else {
            tls_config.verify_mode = security::CertVerifyMode::HOST;
        }

        void* shared_ctx = security::get_shared_ssl_ctx();
        if (shared_ctx == nullptr) {
            return -102;  // FTP_ERR_SYSTEM
        }
        auto tls = std::make_unique<security::TlsTransport>(
            static_cast<SSL_CTX*>(shared_ctx), tls_config);
        int socket_fd = plain->release_socket();
        if (socket_fd < 0) {
            return -401;
        }
        ret = tls->adopt_socket(socket_fd, creds.host.c_str(), creds.port);
        if (ret != 0) {
            return ret;
        }
        tls->set_timeouts(config_.timeout_connect_ms, config_.timeout_command_ms);
        ret = tls->handshake();
        if (ret != 0) {
            return ret;
        }

        auto send_tls_command = [&](const char* command) -> int32_t {
            if (tls->write(command, static_cast<uint32_t>(std::strlen(command))) < 0) {
                return -401;
            }
            std::string command_buffer;
            command_buffer.reserve(4096);
            FtpReply command_reply;
            size_t command_consumed = 0;
            char command_read_buffer[4096];
            for (;;) {
                int32_t command_read = tls->read(command_read_buffer, sizeof(command_read_buffer));
                if (command_read <= 0) {
                    return command_read == 0 ? -403 : command_read;
                }
                command_buffer.append(command_read_buffer, static_cast<size_t>(command_read));
                ParseResult command_parsed = ReplyParser::parse(
                    command_buffer.data(), command_buffer.size(), command_reply, command_consumed);
                if (command_parsed == ParseResult::COMPLETE) {
                    break;
                }
                if (command_parsed == ParseResult::MALFORMED || command_parsed == ParseResult::BUFFER_OVERFLOW) {
                    return -501;
                }
            }
            return is_ftp_success(command_reply.code) ? 0 : map_ftp_code_to_error(command_reply.code);
        };

        // RFC 4217 requires PBSZ before PROT; TLS uses a streaming PBSZ of 0.
        ret = send_tls_command("PBSZ 0\r\n");
        if (ret != 0) {
            return ret;
        }
        ret = send_tls_command("PROT P\r\n");
        if (ret != 0) {
            return ret;
        }

        control_transport_ = std::move(tls);
        ret = control_thread_->start(std::move(control_transport_), creds.host, false);
    } else {
        control_transport_ = std::move(plain);
        ret = control_thread_->start(std::move(control_transport_), creds.host, true);
    }
    if (ret != 0) {
        disconnect();
        return ret;
    }

    ret = authenticate(creds.username, creds.password);
    if (ret != 0) {
        disconnect();
        return ret;
    }
    return 0;
}

inline int32_t ProtocolEngine::disconnect() {
    int32_t ret = 0;
    if (control_thread_) {
        if (control_thread_->is_running()) {
            ret = control_thread_->disconnect();
        }
        control_thread_->stop();
        control_thread_.reset();
    }

    control_thread_ = std::make_unique<ControlThread>();
    creds_ = ConnectionCredentials();
    return ret;
}

inline int32_t ProtocolEngine::ping() {
    if (!is_authenticated()) {
        return -203;  // FTP_ERR_INVALID_STATE
    }
    
    auto future = control_thread_->enqueue_command("NOOP", "");
    
    if (future.valid()) {
        future.wait();
        return future.get();
    }
    
    return -203;
}

inline bool ProtocolEngine::is_connected() const {
    return control_thread_ && control_thread_->is_running();
}

inline bool ProtocolEngine::is_authenticated() const {
    return control_thread_ && control_thread_->is_authenticated();
}

inline WalkError ProtocolEngine::traverse_local(const std::string& local_path,
                                                 const std::string& remote_path,
                                                 const TraversalConfig& config,
                                                 FileManifest& manifest) {
    return DirectoryWalker::traverse(local_path, remote_path, config, manifest);
}

inline void ProtocolEngine::set_control_transport_factory(TransportFactory factory) {
    transport_factory_ = factory;
}

inline void ProtocolEngine::set_tls_mode(int32_t use_tls) {
    use_tls_ = (use_tls != 0);
}

inline bool transfer_cancelled(const TransferOptions& options) {
    return options.cancel_token && options.cancel_token->load(std::memory_order_acquire);
}

inline uint32_t transfer_io_timeout(const ProtocolEngineConfig& config,
                                    const TransferOptions& options) {
    if (options.stall_timeout_ms == 0) {
        return config.timeout_command_ms;
    }
    return std::max<uint32_t>(1, std::min(config.timeout_command_ms,
                                          options.stall_timeout_ms));
}

inline bool transfer_deadline_elapsed(
    const std::chrono::steady_clock::time_point& last_progress,
    uint32_t timeout_ms) {
    return timeout_ms != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_progress).count() >= timeout_ms;
}

inline int32_t ProtocolEngine::upload_file(const FileManifestEntry& entry,
                                            uint64_t* out_bytes_sent,
                                            const ProgressCallback& progress,
                                            uint64_t restart_offset,
                                            const TransferOptions& options) {
    if (out_bytes_sent != nullptr) {
        *out_bytes_sent = 0;
    }
    if (!is_authenticated() || !control_thread_) {
        return -203;  // FTP_ERR_INVALID_STATE
    }
    if (entry.local_absolute_path.empty() || entry.remote_relative_path.empty()) {
        return -202;  // FTP_ERR_INVALID_ARGUMENT
    }
    if (transfer_cancelled(options)) {
        return -604;
    }
    if (!options.expected_sha256.empty() &&
        !integrity::is_sha256_hex(options.expected_sha256)) {
        return -202;
    }

    auto type_future = control_thread_->enqueue_command("TYPE", "I");
    if (!type_future.valid()) {
        return -401;
    }
    int32_t ret = type_future.get();
    if (ret != 0) {
        return ret;
    }

    // EPSV is preferred because it avoids server-supplied address data. If a
    // server rejects it, retain the authenticated session and fall back to
    // classic IPv4 PASV.
    if (restart_offset > 0) {
        auto rest_future = control_thread_->enqueue_command(
            "REST", std::to_string(restart_offset));
        if (!rest_future.valid()) {
            return -401;
        }
        ret = rest_future.get();
        if (ret != 0) {
            return ret;
        }
    }

    auto passive_future = control_thread_->enqueue_command_with_reply("EPSV", "");
    if (!passive_future.valid()) {
        return -401;
    }
    CommandReply passive_reply = passive_future.get();
    PassiveModeResult passive;
    if (passive_reply.status == 0 && passive_reply.code == 229) {
        passive = DataChannel::parse_epsv(passive_reply.message.empty()
            ? std::string("229") : std::string("229 ") + passive_reply.message);
        passive.ip = control_thread_->get_host();
    }

    if (passive.port == 0) {
        control_thread_->reset_after_data_error();
        auto pasv_future = control_thread_->enqueue_command_with_reply("PASV", "");
        if (!pasv_future.valid()) {
            return -401;
        }
        passive_reply = pasv_future.get();
        if (passive_reply.status != 0 || passive_reply.code != 227) {
            return passive_reply.status != 0 ? passive_reply.status : -503;
        }
        passive = DataChannel::parse_pasv(
            passive_reply.message.empty()
                ? std::string("227") : std::string("227 ") + passive_reply.message,
            control_thread_->get_host());
    }

    if (passive.ip.empty() || passive.port == 0) {
        control_thread_->reset_after_data_error();
        return -501;  // FTP_ERR_PROTOCOL
    }

    auto data_plain = std::make_unique<PlainTransport>();
    data_plain->set_timeouts(config_.timeout_connect_ms,
                                transfer_io_timeout(config_, options));
    ret = data_plain->connect(passive.ip.c_str(), passive.port);
    if (ret != 0) {
        control_thread_->reset_after_data_error();
        return ret;
    }

    // STOR must be accepted with 125/150 before application bytes are sent.
    auto stor_future = control_thread_->enqueue_command_with_reply("STOR", entry.remote_relative_path);
    if (!stor_future.valid()) {
        data_plain->shutdown();
        control_thread_->reset_after_data_error();
        return -401;
    }
    CommandReply stor_reply = stor_future.get();
    if (stor_reply.status != 0 || (stor_reply.code != 125 && stor_reply.code != 150)) {
        data_plain->shutdown();
        control_thread_->reset_after_data_error();
        return stor_reply.status != 0 ? stor_reply.status : -503;
    }

    std::unique_ptr<Transport> data_transport;
    if (creds_.use_tls == 1) {
        security::TlsConfig tls_config;
        tls_config.server_name = creds_.host;
        tls_config.ca_bundle_path = creds_.ca_bundle_path;
        if (creds_.verify_cert == 0) {
            tls_config.verify_mode = security::CertVerifyMode::NONE;
        } else if (creds_.verify_cert == 1) {
            tls_config.verify_mode = security::CertVerifyMode::PEER;
        } else {
            tls_config.verify_mode = security::CertVerifyMode::HOST;
        }
        void* shared_ctx = security::get_shared_ssl_ctx();
        if (shared_ctx == nullptr) {
            data_plain->shutdown();
            control_thread_->reset_after_data_error();
            return -102;
        }
        auto data_tls = std::make_unique<security::TlsTransport>(
            static_cast<SSL_CTX*>(shared_ctx), tls_config);
        int socket_fd = data_plain->release_socket();
        ret = data_tls->adopt_socket(socket_fd, creds_.host.c_str(), passive.port);
        if (ret == 0) {
            data_tls->set_timeouts(config_.timeout_connect_ms,
                                  transfer_io_timeout(config_, options));
            ret = data_tls->handshake();
        }
        if (ret != 0) {
            data_tls->shutdown();
            control_thread_->reset_after_data_error();
            return ret;
        }
        data_transport = std::move(data_tls);
    } else {
        data_transport = std::move(data_plain);
    }

    std::ifstream file(entry.local_absolute_path, std::ios::binary);
    if (!file) {
        data_transport->shutdown();
        control_thread_->reset_after_data_error();
        return -601;  // FTP_ERR_LOCAL_IO
    }

    if (restart_offset > 0) {
        file.seekg(static_cast<std::streamoff>(restart_offset), std::ios::beg);
        if (!file) {
            data_transport->shutdown();
            control_thread_->reset_after_data_error();
            return -601;
        }
    }

    char buffer[256 * 1024];
    uint64_t bytes_sent = restart_offset;
    auto last_progress = std::chrono::steady_clock::now();
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        if (transfer_cancelled(options)) {
            data_transport->shutdown();
            abort_control_session();
            return -604;
        }
        std::streamsize bytes = file.gcount();
        size_t offset = 0;
        while (offset < static_cast<size_t>(bytes)) {
            if (transfer_cancelled(options)) {
                data_transport->shutdown();
                abort_control_session();
                return -604;
            }
            int32_t written = data_transport->write(
                buffer + offset, static_cast<uint32_t>(bytes) - static_cast<uint32_t>(offset));
            if (written <= 0) {
                const bool timed_out = written == -402;
                data_transport->shutdown();
                if (transfer_cancelled(options)) {
                    abort_control_session();
                    return -604;
                }
                if (timed_out && transfer_deadline_elapsed(last_progress, options.stall_timeout_ms)) {
                    abort_control_session();
                    return -605;
                }
                auto failed_final_future = control_thread_->enqueue_final_transfer_reply();
                if (failed_final_future.valid()) {
                    (void)failed_final_future.get();
                }
                control_thread_->reset_after_data_error();
                return written < 0 ? written : -403;
            }
            offset += static_cast<size_t>(written);
            bytes_sent += static_cast<uint64_t>(written);
            last_progress = std::chrono::steady_clock::now();
            if (progress) {
                progress(bytes_sent, entry.size_bytes);
            }
        }
    }
    data_transport->shutdown();

    // Only a positive final reply makes the upload successful. The control
    // worker reads it after the data transport has sent its close indication.
    auto final_future = control_thread_->enqueue_final_transfer_reply();
    if (!final_future.valid()) {
        return -401;
    }
    CommandReply final_reply = final_future.get();
    if (final_reply.status != 0 || (final_reply.code != 226 && final_reply.code != 250)) {
        control_thread_->reset_after_data_error();
        return final_reply.status != 0 ? final_reply.status : -503;
    }

    if (!options.expected_sha256.empty()) {
        std::string actual_sha256;
        if (!integrity::sha256_file(entry.local_absolute_path, actual_sha256) ||
            actual_sha256 != integrity::normalize_sha256(options.expected_sha256)) {
            return -606;
        }
    }

    if (progress) {
        progress(entry.size_bytes, entry.size_bytes);
    }
    if (out_bytes_sent != nullptr) {
        *out_bytes_sent = bytes_sent;
    }
    return 0;
}

inline int32_t ProtocolEngine::download_file(const FileManifestEntry& entry,
                                              uint64_t* out_bytes_received,
                                              const ProgressCallback& progress,
                                              const TransferOptions& options) {
    if (out_bytes_received != nullptr) {
        *out_bytes_received = 0;
    }
    if (!is_authenticated() || !control_thread_ ||
        entry.local_absolute_path.empty() || entry.remote_relative_path.empty()) {
        return -203;  // FTP_ERR_INVALID_STATE
    }
    if (transfer_cancelled(options)) {
        return -604;
    }
    if (!options.expected_sha256.empty() &&
        !integrity::is_sha256_hex(options.expected_sha256)) {
        return -202;
    }

    namespace fs = std::filesystem;
    fs::path final_path(entry.local_absolute_path);
    std::error_code fs_error;
    if (!final_path.parent_path().empty()) {
        fs::create_directories(final_path.parent_path(), fs_error);
        if (fs_error) {
            return -601;  // FTP_ERR_LOCAL_IO
        }
    }
    fs::path temp_path = final_path;
    temp_path += ".ftpclient.part";
    fs::remove(temp_path, fs_error);
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return -601;  // FTP_ERR_LOCAL_IO
    }

    auto type_future = control_thread_->enqueue_command("TYPE", "I");
    if (!type_future.valid()) {
        output.close();
        fs::remove(temp_path, fs_error);
        return -401;
    }
    int32_t ret = type_future.get();
    if (ret != 0) {
        output.close();
        fs::remove(temp_path, fs_error);
        return ret;
    }

    auto passive_future = control_thread_->enqueue_command_with_reply("EPSV", "");
    if (!passive_future.valid()) {
        output.close();
        fs::remove(temp_path, fs_error);
        return -401;
    }
    CommandReply passive_reply = passive_future.get();
    PassiveModeResult passive;
    if (passive_reply.status == 0 && passive_reply.code == 229) {
        passive = DataChannel::parse_epsv(passive_reply.message.empty()
            ? std::string("229") : std::string("229 ") + passive_reply.message);
        passive.ip = control_thread_->get_host();
    }
    if (passive.port == 0) {
        control_thread_->reset_after_data_error();
        auto pasv_future = control_thread_->enqueue_command_with_reply("PASV", "");
        if (!pasv_future.valid()) {
            output.close();
            fs::remove(temp_path, fs_error);
            return -401;
        }
        passive_reply = pasv_future.get();
        if (passive_reply.status != 0 || passive_reply.code != 227) {
            output.close();
            fs::remove(temp_path, fs_error);
            return passive_reply.status != 0 ? passive_reply.status : -503;
        }
        passive = DataChannel::parse_pasv(
            passive_reply.message.empty()
                ? std::string("227") : std::string("227 ") + passive_reply.message,
            control_thread_->get_host());
    }
    if (passive.ip.empty() || passive.port == 0) {
        output.close();
        fs::remove(temp_path, fs_error);
        control_thread_->reset_after_data_error();
        return -501;
    }

    auto data_plain = std::make_unique<PlainTransport>();
    data_plain->set_timeouts(config_.timeout_connect_ms,
                                transfer_io_timeout(config_, options));
    ret = data_plain->connect(passive.ip.c_str(), passive.port);
    if (ret != 0) {
        output.close();
        fs::remove(temp_path, fs_error);
        control_thread_->reset_after_data_error();
        return ret;
    }

    auto retr_future = control_thread_->enqueue_command_with_reply("RETR", entry.remote_relative_path);
    if (!retr_future.valid()) {
        data_plain->shutdown();
        output.close();
        fs::remove(temp_path, fs_error);
        control_thread_->reset_after_data_error();
        return -401;
    }
    CommandReply retr_reply = retr_future.get();
    if (retr_reply.status != 0 || (retr_reply.code != 125 && retr_reply.code != 150)) {
        data_plain->shutdown();
        output.close();
        fs::remove(temp_path, fs_error);
        control_thread_->reset_after_data_error();
        return retr_reply.status != 0 ? retr_reply.status : -503;
    }

    std::unique_ptr<Transport> data_transport;
    if (creds_.use_tls == 1) {
        security::TlsConfig tls_config;
        tls_config.server_name = creds_.host;
        tls_config.ca_bundle_path = creds_.ca_bundle_path;
        tls_config.verify_mode = static_cast<security::CertVerifyMode>(creds_.verify_cert);
        void* shared_ctx = security::get_shared_ssl_ctx();
        if (shared_ctx == nullptr) {
            data_plain->shutdown();
            output.close();
            fs::remove(temp_path, fs_error);
            control_thread_->reset_after_data_error();
            return -102;
        }
        auto data_tls = std::make_unique<security::TlsTransport>(
            static_cast<SSL_CTX*>(shared_ctx), tls_config);
        int socket_fd = data_plain->release_socket();
        ret = data_tls->adopt_socket(socket_fd, creds_.host.c_str(), passive.port);
        if (ret == 0) {
            data_tls->set_timeouts(config_.timeout_connect_ms,
                                  transfer_io_timeout(config_, options));
            ret = data_tls->handshake();
        }
        if (ret != 0) {
            data_tls->shutdown();
            output.close();
            fs::remove(temp_path, fs_error);
            control_thread_->reset_after_data_error();
            return ret;
        }
        data_transport = std::move(data_tls);
    } else {
        data_transport = std::move(data_plain);
    }

    uint64_t bytes_received = 0;
    char buffer[256 * 1024];
    auto last_progress = std::chrono::steady_clock::now();
    for (;;) {
        if (transfer_cancelled(options)) {
            data_transport->shutdown();
            output.close();
            abort_control_session();
            return -604;
        }
        int32_t count = data_transport->read(buffer, sizeof(buffer));
        if (count < 0) {
            const bool timed_out = count == -402;
            data_transport->shutdown();
            if (transfer_cancelled(options)) {
                output.close();
                abort_control_session();
                return -604;
            }
            if (timed_out && transfer_deadline_elapsed(last_progress, options.stall_timeout_ms)) {
                output.close();
                abort_control_session();
                return -605;
            }
            output.close();
            fs::remove(temp_path, fs_error);
            control_thread_->reset_after_data_error();
            return count;
        }
        if (count == 0) {
            break;
        }
        output.write(buffer, count);
        if (!output) {
            data_transport->shutdown();
            output.close();
            fs::remove(temp_path, fs_error);
            control_thread_->reset_after_data_error();
            return -601;
        }
        bytes_received += static_cast<uint64_t>(count);
        last_progress = std::chrono::steady_clock::now();
        if (progress) {
            progress(bytes_received, entry.size_bytes);
        }
    }
    data_transport->shutdown();
    output.flush();
    output.close();
    if (!output) {
        fs::remove(temp_path, fs_error);
        control_thread_->reset_after_data_error();
        return -601;
    }

    auto final_future = control_thread_->enqueue_final_transfer_reply();
    if (!final_future.valid()) {
        fs::remove(temp_path, fs_error);
        return -401;
    }
    CommandReply final_reply = final_future.get();
    if (final_reply.status != 0 || (final_reply.code != 226 && final_reply.code != 250)) {
        fs::remove(temp_path, fs_error);
        control_thread_->reset_after_data_error();
        return final_reply.status != 0 ? final_reply.status : -503;
    }

    if (!options.expected_sha256.empty()) {
        std::string actual_sha256;
        if (!integrity::sha256_file(temp_path.string(), actual_sha256) ||
            actual_sha256 != integrity::normalize_sha256(options.expected_sha256)) {
            fs::remove(temp_path, fs_error);
            return -606;
        }
    }

    fs::remove(final_path, fs_error);
    fs_error.clear();
    fs::rename(temp_path, final_path, fs_error);
    if (fs_error) {
        fs::remove(temp_path, fs_error);
        control_thread_->reset_after_data_error();
        return -601;
    }
    if (progress) {
        progress(bytes_received, entry.size_bytes);
    }
    if (out_bytes_received != nullptr) {
        *out_bytes_received = bytes_received;
    }
    return 0;
}

inline int32_t ProtocolEngine::change_directory(const std::string& remote_path) {
    if (!is_authenticated() || remote_path.empty() || !control_thread_) {
        return -203;
    }
    auto future = control_thread_->enqueue_command("CWD", remote_path);
    return future.valid() ? future.get() : -401;
}

inline int32_t ProtocolEngine::parent_directory() {
    if (!is_authenticated() || !control_thread_) {
        return -203;
    }
    auto future = control_thread_->enqueue_command("CDUP", "");
    return future.valid() ? future.get() : -401;
}

inline int32_t ProtocolEngine::delete_remote_file(const std::string& remote_path) {
    if (!is_authenticated() || remote_path.empty() || !control_thread_) {
        return -203;
    }
    auto future = control_thread_->enqueue_command("DELE", remote_path);
    return future.valid() ? future.get() : -401;
}

inline int32_t ProtocolEngine::remove_remote_dir(const std::string& remote_path) {
    if (!is_authenticated() || remote_path.empty() || !control_thread_) {
        return -203;
    }
    auto future = control_thread_->enqueue_command("RMD", remote_path);
    return future.valid() ? future.get() : -401;
}

inline int32_t ProtocolEngine::rename_remote_path(const std::string& from_path,
                                                   const std::string& to_path) {
    if (!is_authenticated() || from_path.empty() || to_path.empty() || !control_thread_) {
        return -203;
    }
    auto from_future = control_thread_->enqueue_command("RNFR", from_path);
    if (!from_future.valid()) return -401;
    int32_t ret = from_future.get();
    if (ret != 0) return ret;
    auto to_future = control_thread_->enqueue_command("RNTO", to_path);
    return to_future.valid() ? to_future.get() : -401;
}

inline int32_t ProtocolEngine::create_remote_dir(const std::string& remote_path) {
    if (!is_authenticated() || remote_path.empty() || !control_thread_) {
        return -203;  // FTP_ERR_INVALID_STATE
    }
    auto future = control_thread_->enqueue_command("MKD", remote_path);
    if (!future.valid()) {
        return -401;  // FTP_ERR_CONNECT
    }
    return future.get();
}

inline int32_t ProtocolEngine::get_remote_file_size(const std::string& remote_path, uint64_t* out_size) {
    if (!is_authenticated() || remote_path.empty() || out_size == nullptr || !control_thread_) {
        return -202;  // FTP_ERR_INVALID_ARGUMENT
    }
    *out_size = 0;
    auto future = control_thread_->enqueue_command_with_reply("SIZE", remote_path);
    if (!future.valid()) {
        return -401;
    }
    CommandReply reply = future.get();
    if (reply.status != 0) {
        return reply.status;
    }
    if (reply.code != 213) {
        return -501;
    }
    const char* begin = reply.message.c_str();
    char* end = nullptr;
    unsigned long long value = std::strtoull(begin, &end, 10);
    if (end == begin) {
        return -501;
    }
    *out_size = static_cast<uint64_t>(value);
    return 0;
}

inline void ProtocolEngine::set_command_timeout(uint32_t ms) {
    config_.timeout_command_ms = ms;
    if (control_thread_) {
        control_thread_->set_command_timeout(ms);
    }
}

inline int32_t ProtocolEngine::authenticate(const std::string& username, const std::string& password) {
    // Send USER command
    {
        auto future = control_thread_->enqueue_command("USER", username);
        if (!future.valid()) {
            return -401;  // FTP_ERR_CONNECT
        }
        
        int32_t ret = future.get();
        if (ret != 0 && ret != -301) {
            return ret;
        }
        if (control_thread_->is_authenticated()) {
            return 0;  // Some servers accept anonymous with just USER
        }
        // USER/331 leaves the state AUTH_IN_PROGRESS, so continue with PASS.
    }
    
    // Send PASS command
    {
        auto future = control_thread_->enqueue_command("PASS", password);
        if (!future.valid()) {
            return -401;  // FTP_ERR_CONNECT
        }
        
        return future.get();
    }
}

inline std::unique_ptr<Transport> ProtocolEngine::create_transport() {
    if (transport_factory_) {
        return transport_factory_();
    }
    
    // Default to plain transport
    return std::make_unique<PlainTransport>();
}

}} // namespace ftpclient::protocol

#endif /* FTPCLIENT_PROTOCOL_ENGINE_HPP */
