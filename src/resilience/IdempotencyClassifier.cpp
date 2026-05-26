/*
 * IdempotencyClassifier.cpp - Command Safety Table Implementation for Phase 5
 * 
 * Implements idempotency classification per Phase 5 Spec Section 7.
 */

#include "IdempotencyClassifier.hpp"
#include <cstring>
#include <cctype>

namespace ftpclient { namespace resilience {

IdempotencyClassifier::IdempotencyClassifier() = default;
IdempotencyClassifier::~IdempotencyClassifier() = default;

/* Helper to convert command to uppercase for comparison */
static inline int cmd_char_upper(char c) {
    return static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(c)));
}

static bool cmd_equals(const char* cmd, const char* target) {
    while (*target) {
        if (cmd_char_upper(*cmd) != cmd_char_upper(*target)) {
            return false;
        }
        ++cmd;
        ++target;
    }
    return *cmd == '\0';
}

IdempotencyClass IdempotencyClassifier::classify_command(const char* command) {
    if (!command) {
        return IdempotencyClass::UNSAFE;
    }
    
    /* Per Phase 5 Spec Section 7.1 Static Safety Table */
    
    /* SAFE commands - no server state change on re-send */
    if (cmd_equals(command, "USER") ||
        cmd_equals(command, "PASS") ||
        cmd_equals(command, "CWD") ||
        cmd_equals(command, "PWD") ||
        cmd_equals(command, "TYPE") ||
        cmd_equals(command, "PASV") ||
        cmd_equals(command, "EPSV") ||
        cmd_equals(command, "PBSZ") ||
        cmd_equals(command, "PROT") ||
        cmd_equals(command, "NOOP") ||
        cmd_equals(command, "SIZE") ||
        cmd_equals(command, "REST") ||
        cmd_equals(command, "QUIT")) {
        return IdempotencyClass::SAFE;
    }
    
    /* CONDITIONAL commands - retry only under specific conditions */
    if (cmd_equals(command, "MKD")) {
        return IdempotencyClass::CONDITIONAL;
    }
    
    /* UNSAFE commands - never retry without special handling */
    if (cmd_equals(command, "STOR") ||
        cmd_equals(command, "APPE") ||
        cmd_equals(command, "DELE") ||
        cmd_equals(command, "RNFR") ||
        cmd_equals(command, "RNTO") ||
        cmd_equals(command, "RETR")) {
        return IdempotencyClass::UNSAFE;
    }
    
    /* Unknown commands - treat as unsafe for safety */
    return IdempotencyClass::UNSAFE;
}

bool IdempotencyClassifier::is_safe_to_retry(const char* command) {
    return classify_command(command) == IdempotencyClass::SAFE;
}

const char* IdempotencyClassifier::get_retry_rule(const char* command) {
    if (!command) {
        return "Unknown command - do not retry";
    }
    
    /* Per Phase 5 Spec Section 7.1 Static Safety Table */
    
    if (cmd_equals(command, "USER")) {
        return "Retry - No server state change on re-send";
    }
    if (cmd_equals(command, "PASS")) {
        return "Retry - Same credentials, same outcome";
    }
    if (cmd_equals(command, "CWD")) {
        return "Retry - Idempotent directory change";
    }
    if (cmd_equals(command, "PWD")) {
        return "Retry - Pure read";
    }
    if (cmd_equals(command, "TYPE")) {
        return "Retry - Idempotent mode set";
    }
    if (cmd_equals(command, "PASV") || cmd_equals(command, "EPSV")) {
        return "Retry - Allocates new ephemeral port each time";
    }
    if (cmd_equals(command, "PBSZ") || cmd_equals(command, "PROT")) {
        return "Retry - Idempotent security parameter";
    }
    if (cmd_equals(command, "NOOP")) {
        return "Retry - No-op by definition";
    }
    if (cmd_equals(command, "SIZE")) {
        return "Retry - Pure read";
    }
    if (cmd_equals(command, "REST")) {
        return "Retry - Sets offset; re-setting same offset is safe";
    }
    if (cmd_equals(command, "QUIT")) {
        return "Retry (harmless) - Connection close";
    }
    if (cmd_equals(command, "MKD")) {
        return "Conditional - Retry if 550 (already exists)";
    }
    if (cmd_equals(command, "STOR") || cmd_equals(command, "APPE")) {
        return "Unsafe - Never retry full file; must use REST resume";
    }
    if (cmd_equals(command, "DELE")) {
        return "Unsafe - Never retry; deleting twice is error";
    }
    if (cmd_equals(command, "RNFR") || cmd_equals(command, "RNTO")) {
        return "Unsafe - Never retry; rename is stateful";
    }
    if (cmd_equals(command, "RETR")) {
        return "Unsafe - Download may have partially succeeded";
    }
    
    return "Unknown command - do not retry";
}

uint64_t IdempotencyClassifier::get_stor_restart_offset(
    uint64_t bytes_confirmed, 
    bool resume_enabled) 
{
    /* 
     * Per Phase 5 Spec Section 7.2 STOR Retry Semantics:
     * - If resume_enabled and bytes_confirmed > 0, restart from confirmed offset
     * - Otherwise, restart from 0 (full re-upload)
     */
    
    if (!resume_enabled) {
        return 0;  /* Full re-upload */
    }
    
    /* Use confirmed bytes as restart point */
    return bytes_confirmed;
}

bool IdempotencyClassifier::should_retry_mkd(int32_t error_code) {
    /* 
     * Per Phase 5 Spec Section 7.1:
     * MKD is conditional - retry if 550 (directory already exists)
     * Error code -502 = FTP_ERR_SERVER_DENIED (includes "already exists")
     */
    
    /* If we got -502 (server denied), the directory might already exist */
    /* In this case, we should NOT retry MKD but continue with upload */
    /* Return false to indicate we should NOT retry MKD */
    if (error_code == -502) {
        return false;  /* Don't retry, assume dir exists */
    }
    
    /* For other errors, don't retry either */
    return false;
}

bool IdempotencyClassifier::requires_special_handling(const char* command) {
    /* 
     * Commands requiring special handling before retry:
     * - STOR/APPE: Need REST command before retry
     * - RNFR/RNTO: Stateful, cannot retry safely
     */
    
    if (!command) {
        return false;
    }
    
    if (cmd_equals(command, "STOR") ||
        cmd_equals(command, "APPE")) {
        return true;  /* Need REST for resume */
    }
    
    if (cmd_equals(command, "RNFR") ||
        cmd_equals(command, "RNTO")) {
        return true;  /* Cannot retry safely */
    }
    
    return false;
}

}} // namespace ftpclient::resilience
