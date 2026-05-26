/*
 * IdempotencyClassifier.hpp - Command Safety Table for Phase 5
 * 
 * Implements idempotency classification per Phase 5 Spec Section 7.
 * Determines which FTP commands are safe to retry.
 */

#ifndef FTPCLIENT_IDEMPOTENCY_CLASSIFIER_HPP
#define FTPCLIENT_IDEMPOTENCY_CLASSIFIER_HPP

#include <cstdint>

namespace ftpclient { namespace resilience {

/**
 * Command Idempotency Classification per Phase 5 Spec Section 7.1
 */
enum class IdempotencyClass {
    SAFE,           /* ✅ Safe to retry - no server state change */
    CONDITIONAL,    /* ⚠️ Conditional - retry only under specific conditions */
    UNSAFE          /* ❌ Unsafe - never retry without special handling */
};

/**
 * STOR Retry State for handling non-idempotent file uploads
 */
struct StorRetryState {
    bool data_started;        /* true if data channel was opened */
    uint64_t bytes_sent;      /* Bytes sent before failure */
    bool received_226;        /* true if completion code received */
    
    StorRetryState()
        : data_started(false)
        , bytes_sent(0)
        , received_226(false)
    {}
};

/**
 * Idempotency Classifier - Static Command Safety Table
 * 
 * Per Phase 5 Spec Section 7:
 * - Classifies FTP commands by idempotency
 * - Provides retry rules for each command type
 * - Handles STOR retry semantics with resume support
 * 
 * Thread Safety: Stateless and thread-safe.
 */
class IdempotencyClassifier {
public:
    /**
     * Construct idempotency classifier
     */
    IdempotencyClassifier();
    
    /**
     * Destructor
     */
    ~IdempotencyClassifier();
    
    /**
     * Classify an FTP command by idempotency
     * 
     * Per Phase 5 Spec Section 7.1 Static Safety Table
     * 
     * @param command FTP command string (e.g., "USER", "STOR", "DELE")
     * @return IdempotencyClass for the command
     */
    static IdempotencyClass classify_command(const char* command);
    
    /**
     * Check if a command is safe to retry
     * 
     * @param command FTP command string
     * @return true if safe to retry unconditionally
     */
    static bool is_safe_to_retry(const char* command);
    
    /**
     * Get retry rule description for a command
     * 
     * @param command FTP command string
     * @return Human-readable retry rule description
     */
    static const char* get_retry_rule(const char* command);
    
    /**
     * Determine STOR retry strategy after failure
     * 
     * Per Phase 5 Spec Section 7.2 STOR Retry Semantics
     * 
     * @param bytes_confirmed Bytes confirmed by server (via 226 or SIZE check)
     * @param resume_enabled Whether resume is enabled in configuration
     * @return Recommended restart offset (0 = full re-upload)
     */
    static uint64_t get_stor_restart_offset(uint64_t bytes_confirmed, bool resume_enabled);
    
    /**
     * Check if MKD failure should be retried
     * 
     * Per Phase 5 Spec Section 7.1: MKD is conditional - retry if 550 (already exists)
     * 
     * @param error_code FTP error code from MKD attempt
     * @return true if MKD should be retried
     */
    static bool should_retry_mkd(int32_t error_code);
    
    /**
     * Check if a command requires special handling before retry
     * 
     * @param command FTP command string
     * @return true if special handling required (e.g., STOR needs REST)
     */
    static bool requires_special_handling(const char* command);

private:
    /* No instance state - all methods are static */
};

}} // namespace ftpclient::resilience

#endif /* FTPCLIENT_IDEMPOTENCY_CLASSIFIER_HPP */
