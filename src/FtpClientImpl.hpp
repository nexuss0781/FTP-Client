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
#include <vector>

#include "security/CredentialVault.hpp"
#include "security/SecretProvider.hpp"
#include "security/TlsConfig.hpp"
#include "../include/ftpclient.h"  /* For ftp_event_cb_t */

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
 * Certificate validation callback type (matches C ABI)
 */
using CertVerifyCallback = int32_t (*)(
    const char*   subject,
    const char*   issuer,
    const char*   fingerprint,
    int32_t       error_code,
    void*         user_data
);

/**
 * Client configuration options
 */
struct ClientConfig {
    uint64_t buffer_size;       /* Default: 256KB */
    uint32_t timeout_connect;   /* Default: 5000ms */
    uint32_t timeout_command;   /* Default: 30000ms */
    
    /* Phase 5 resilience configuration */
    uint32_t retry_max_attempts;     /* Default: 3 */
    uint64_t retry_base_delay_ms;    /* Default: 1000ms */
    uint64_t retry_max_delay_ms;     /* Default: 30000ms */

    /* Phase 7 optimization flags */
    bool use_io_uring;        /* Default: false - io_uring is experimental */
    bool use_zerocopy;        /* Default: true - zero-copy on supported platforms */
    bool use_compression;     /* Default: false - MODE Z compression opt-in */

    /* Phase 7 rate limiting configuration */
    uint64_t rate_limit_bps;      /* Default: 0 - unlimited */
    uint64_t rate_limit_burst;    /* Default: 0 - no burst */

    ClientConfig() : buffer_size(256 * 1024), timeout_connect(5000), timeout_command(30000),
                     retry_max_attempts(3), retry_base_delay_ms(1000), retry_max_delay_ms(30000),
                     use_io_uring(false), use_zerocopy(true), use_compression(false),
                     rate_limit_bps(0), rate_limit_burst(0) {}
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
     * Get credential vault for secure storage
     */
    security::CredentialVault& getVault() { return vault_; }
    const security::CredentialVault& getVault() const { return vault_; }
    
    /**
     * Get credential provider
     */
    security::SecretProvider* getProvider() { return provider_.get(); }
    
    /**
     * Set credential provider
     */
    void setProvider(std::unique_ptr<security::SecretProvider> p) { provider_ = std::move(p); }
    
    /**
     * Get certificate verification callback
     */
    CertVerifyCallback getCertCallback() const { return cert_callback_; }
    void* getCertCallbackUserdata() const { return cert_callback_userdata_; }
    
    /**
     * Set certificate verification callback
     */
    void setCertCallback(CertVerifyCallback cb, void* userdata) {
        cert_callback_ = cb;
        cert_callback_userdata_ = userdata;
    }
    
    /**
     * Get certificate pins
     */
    const std::vector<std::string>& getCertPins() const { return cert_pins_; }
    
    /**
     * Set certificate pins
     */
    void setCertPins(const std::vector<std::string>& pins) { cert_pins_ = pins; }
    
    /**
     * Set retry policy parameters (Phase 5 Spec Section 4.1)
     */
    void setRetryPolicy(uint32_t max_attempts, uint64_t base_delay_ms, uint64_t max_delay_ms);
    
    /**
     * Set rate limiting configuration (Phase 7)
     */
    void setRateLimit(uint64_t bytes_per_second, uint64_t burst_bytes);
    
    /**
     * Set telemetry event callback (Phase 7)
     */
    void setEventCallback(ftp_event_cb_t cb, void* user_data);
    
    /**
     * Get telemetry event callback
     */
    ftp_event_cb_t getEventCallback() const { return event_callback_; }
    void* getEventCallbackUserdata() const { return event_callback_userdata_; }
    
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
    security::CredentialVault vault_;
    std::unique_ptr<security::SecretProvider> provider_;
    CertVerifyCallback cert_callback_;
    void* cert_callback_userdata_;
    std::vector<std::string> cert_pins_;
    ftp_event_cb_t event_callback_;
    void* event_callback_userdata_;
    std::mutex mutex_;  /* For future thread-safety extensions */
};

// Inline implementations to ensure symbols are available
inline FtpClientImpl::FtpClientImpl() 
    : state_(ClientState::UNINITIALIZED)
    , cert_callback_(nullptr)
    , cert_callback_userdata_(nullptr)
    , event_callback_(nullptr)
    , event_callback_userdata_(nullptr)
{
}

inline FtpClientImpl::~FtpClientImpl() {
    // Vault destructor handles secure cleanup
}

inline void FtpClientImpl::setRetryPolicy(uint32_t max_attempts, uint64_t base_delay_ms, uint64_t max_delay_ms) {
    // Store retry policy configuration for Phase 5 resilience
    // This is used by the ResilienceController during transfer operations
    config_.retry_max_attempts = max_attempts;
    config_.retry_base_delay_ms = base_delay_ms;
    config_.retry_max_delay_ms = max_delay_ms;
}

inline void FtpClientImpl::setRateLimit(uint64_t bytes_per_second, uint64_t burst_bytes) {
    // Store rate limiting configuration for Phase 7 optimization
    // This is used by the TransferEngine to throttle bandwidth
    config_.rate_limit_bps = bytes_per_second;
    config_.rate_limit_burst = burst_bytes;
}

inline void FtpClientImpl::setEventCallback(ftp_event_cb_t cb, void* user_data) {
    // Store telemetry callback for Phase 7 observability
    // This is used by the TelemetryController to emit events
    event_callback_ = cb;
    event_callback_userdata_ = user_data;
}

} // namespace ftpclient

#endif /* FTPCLIENT_IMPL_HPP */
