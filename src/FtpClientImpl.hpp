/*
 * FtpClientImpl.hpp - Internal C++ Implementation Header
 * 
 * This header is NOT exposed to the C ABI. It contains internal C++ classes
 * and implementation details hidden behind the opaque ftp_client_t handle.
 */

#ifndef FTPCLIENT_IMPL_HPP
#define FTPCLIENT_IMPL_HPP

#include <string>
#include <cstdint>
#include <memory>
#include <mutex>
#include <atomic>

namespace ftpclient {

/**
 * Client state machine states as per Phase 1 spec Section 3.2
 */
enum class ClientState : int32_t {
    UNINITIALIZED = 0,
    ALLOCATED = 1,
    CONNECTED = 2,
    DISCONNECTED = 3,
    DESTROYED = 4
};

/**
 * Secure credential storage
 * 
 * Credentials are copied into secure memory on connect and zeroed on disconnect.
 * No std::string for password field to avoid heap fragmentation and ensure secure cleanup.
 */
struct SecureCredentials {
    std::string host;
    uint16_t port;
    std::string username;
    // Password stored in a way that can be securely zeroed
    std::unique_ptr<char[]> password_data;
    size_t password_len;
    int32_t use_tls;
    int32_t verify_cert;
    std::string ca_bundle_path;
    
    SecureCredentials() : port(0), password_data(nullptr), password_len(0), 
                          use_tls(0), verify_cert(1) {}
    
    // Securely zero all credential data including strings
    void secure_clear() {
        // Securely zero password data first
        if (password_data && password_len > 0) {
            volatile char* p = password_data.get();
            for (size_t i = 0; i < password_len; ++i) {
                p[i] = 0;
            }
        }
        password_data.reset();
        password_len = 0;
        
        // Clear strings securely by replacing with empty and shrinking capacity
        // Note: std::string doesn't guarantee zeroing, so we use assign + shrink_to_fit
        std::string().swap(host);
        std::string().swap(username);
        std::string().swap(ca_bundle_path);
    }
    
    ~SecureCredentials() {
        secure_clear();
    }
};

/**
 * Client configuration options
 */
struct ClientConfig {
    uint64_t buffer_size;       /* Default: 256KB */
    uint32_t timeout_connect;   /* Default: 5000ms */
    uint32_t timeout_command;   /* Default: 30000ms */
    
    ClientConfig() : buffer_size(256 * 1024), timeout_connect(5000), timeout_command(30000) {}
};

/**
 * Main FTP client implementation class
 * 
 * Thread Safety: This class is NOT thread-safe. Concurrent calls from multiple
 * threads result in undefined behavior as per ABI spec Section 8.
 */
class FtpClientImpl {
public:
    FtpClientImpl();
    ~FtpClientImpl();
    
    // Prevent copying
    FtpClientImpl(const FtpClientImpl&) = delete;
    FtpClientImpl& operator=(const FtpClientImpl&) = delete;
    
    /**
     * Get current client state
     */
    ClientState getState() const { return state_.load(std::memory_order_acquire); }
    
    /**
     * Set client state atomically
     */
    void setState(ClientState s) { state_.store(s, std::memory_order_release); }
    
    /**
     * Get mutable configuration
     */
    ClientConfig& getConfig() { return config_; }
    const ClientConfig& getConfig() const { return config_; }
    
    /**
     * Get mutable credentials (for secure storage during connection)
     */
    SecureCredentials& getCredentials() { return creds_; }
    
    /**
     * Check if handle is valid (not destroyed)
     */
    bool isValid() const {
        return state_.load(std::memory_order_acquire) != ClientState::DESTROYED;
    }
    
    /**
     * Mark as destroyed - called once before destruction
     * Returns true if this is the first destroy call, false if already destroyed
     */
    bool markDestroyed() {
        ClientState expected = getState();
        if (expected == ClientState::DESTROYED) {
            return false;  /* Already destroyed */
        }
        setState(ClientState::DESTROYED);
        return true;  /* Successfully marked */
    }

private:
    std::atomic<ClientState> state_;
    ClientConfig config_;
    SecureCredentials creds_;
    std::mutex mutex_;  /* For future thread-safety extensions */
};

// Inline implementations to ensure symbols are available
inline FtpClientImpl::FtpClientImpl() 
    : state_(ClientState::UNINITIALIZED) {
}

inline FtpClientImpl::~FtpClientImpl() {
    creds_.secure_clear();
}

} // namespace ftpclient

#endif /* FTPCLIENT_IMPL_HPP */
