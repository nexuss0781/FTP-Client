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
#include <memory>
#include <string>

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
    
    ConnectionCredentials()
        : port(21)
        , use_tls(0)
    {}
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
    int32_t upload_file(const FileManifestEntry& entry);
    
    /**
     * Download a single file (Phase 4)
     * 
     * @param entry File manifest entry
     * @return 0 on success, negative error code on failure
     */
    int32_t download_file(const FileManifestEntry& entry);
    
    /**
     * Create remote directory (Phase 4)
     * 
     * @param remote_path Remote directory path
     * @return 0 on success, negative error code on failure
     */
    int32_t create_remote_dir(const std::string& remote_path);
    
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
    
    /**
     * Set command timeout
     */
    void set_command_timeout(uint32_t ms);

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
};

inline ProtocolEngine::ProtocolEngine()
    : control_thread_(std::make_unique<ControlThread>())
    , transport_factory_(nullptr)
    , use_tls_(false)
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
    
    creds_ = creds;
    
    // Create transport
    control_transport_ = create_transport();
    
    int32_t ret = control_transport_->connect(creds.host.c_str(), creds.port);
    if (ret != 0) {
        return ret;
    }
    
    // Start control thread
    ret = control_thread_->start(std::move(control_transport_), creds.host);
    if (ret != 0) {
        return ret;
    }
    
    // Wait for greeting (first response from server)
    // The control thread handles this automatically
    
    // Authenticate
    ret = authenticate(creds.username, creds.password);
    if (ret != 0) {
        disconnect();
        return ret;
    }
    
    return 0;
}

inline int32_t ProtocolEngine::disconnect() {
    if (!is_connected()) {
        return 0;  // Already disconnected
    }
    
    if (control_thread_) {
        control_thread_->disconnect();
        control_thread_->stop();
        control_thread_.reset();
    }
    
    control_thread_ = std::make_unique<ControlThread>();
    creds_ = ConnectionCredentials();
    
    return 0;
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

inline int32_t ProtocolEngine::upload_file(const FileManifestEntry& entry) {
    // Phase 4 stub
    (void)entry;
    return -203;  // FTP_ERR_INVALID_STATE - not implemented yet
}

inline int32_t ProtocolEngine::download_file(const FileManifestEntry& entry) {
    // Phase 4 stub
    (void)entry;
    return -203;  // FTP_ERR_INVALID_STATE - not implemented yet
}

inline int32_t ProtocolEngine::create_remote_dir(const std::string& remote_path) {
    // Phase 4 stub
    (void)remote_path;
    return -203;  // FTP_ERR_INVALID_STATE - not implemented yet
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
        if (ret == 0) {
            return 0;  // Some servers accept anonymous with just USER
        }
        if (ret != -301) {  // Not FTP_ERR_AUTH_FAILED expecting PASS
            return ret;
        }
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
