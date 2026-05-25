/*
 * StateMachine.hpp - FTP Protocol State Machine
 * 
 * Formal state definitions and transition validation per Phase 2 spec Section 5.
 * The state machine is the single source of truth for valid command execution.
 */

#ifndef FTPCLIENT_STATE_MACHINE_HPP
#define FTPCLIENT_STATE_MACHINE_HPP

#include <cstdint>

namespace ftpclient { namespace protocol {

/**
 * Protocol State Machine States
 * 
 * Per Phase 2 spec Section 5.1
 */
enum class ProtocolState : int32_t {
    DISCONNECTED = 0,       // Initial. No socket.
    TCP_CONNECTED = 1,      // TCP 3-way complete. Awaiting greeting.
    GREETING_WAIT = 2,      // Reading 220 banner.
    AUTH_IN_PROGRESS = 3,   // USER sent, awaiting 331/230.
    AUTHENTICATED = 4,      // Logged in. Ready for file system commands.
    CWD_PENDING = 5,        // CWD sent, awaiting 250.
    PASV_PENDING = 6,       // PASV/EPSV sent, awaiting 227/229.
    DATA_CONNECTING = 7,    // Parsing PASV, opening data socket.
    DATA_READY = 8,         // Data channel established. Ready for STOR/RETR/LIST.
    DATA_TRANSFERRING = 9,  // STOR/RETR/LIST in progress. Data socket active.
    DATA_FINALIZING = 10,   // Data socket closed. Awaiting 226/250 on control.
    DISCONNECTING = 11,     // QUIT sent or closing.
    ERROR = 12              // Unrecoverable. All future commands fail fast.
};

/**
 * Check if authenticated helper - fixed operator precedence
 */
inline bool is_state_authenticated(ProtocolState state) {
    return state == ProtocolState::AUTHENTICATED ||
           state == ProtocolState::CWD_PENDING ||
           (state >= ProtocolState::PASV_PENDING && state <= ProtocolState::DATA_FINALIZING);
}

/**
 * FTP Commands for state transition validation
 */
enum class FtpCommand : int32_t {
    CONNECT = 0,    // Initial connection
    USER = 1,       // Username
    PASS = 2,       // Password
    QUIT = 3,       // Disconnect
    NOOP = 4,       // Keep-alive
    PWD = 5,        // Print working directory
    CWD = 6,        // Change working directory
    CDUP = 7,       // Change to parent directory
    PASV = 8,       // Passive mode (IPv4)
    EPSV = 9,       // Extended passive mode (IPv6)
    PORT = 10,      // Active mode (not used in Phase 2)
    EPRT = 11,      // Extended active mode (not used in Phase 2)
    LIST = 12,      // Directory listing
    NLST = 13,      // Name list
    RETR = 14,      // Retrieve (download)
    STOR = 15,      // Store (upload)
    APPE = 16,      // Append
    DELE = 17,      // Delete
    MKD = 18,       // Make directory
    RMD = 19,       // Remove directory
    RNFR = 20,      // Rename from
    RNTO = 21,      // Rename to
    SIZE = 22,      // File size
    REST = 23,      // Restart marker
    FEAT = 24,      // Features
    TYPE = 25,      // Representation type
    SYST = 26,      // System type
    OPTS = 27,      // Options
    MDTM = 28,      // Modification time
    SITE = 29,      // Site-specific commands
    XCUP = 30,      // Extended change to parent
    XPWD = 31,      // Extended print working directory
    XCWD = 32,      // Extended change working directory
    XMKD = 33,      // Extended make directory
    XRMD = 34       // Extended remove directory
};

/**
 * State transition result
 */
enum class TransitionResult : int32_t {
    SUCCESS = 0,            // Transition allowed
    INVALID_STATE = -203,   // Command not valid in current state
    INVALID_COMMAND = -501, // Unknown command
    PROTOCOL_ERROR = -501   // Protocol violation
};

/**
 * FTP Protocol State Machine
 * 
 * Validates command execution based on current state.
 * Thread-safe for read operations.
 */
class StateMachine {
public:
    StateMachine() : state_(ProtocolState::DISCONNECTED) {}
    
    /**
     * Get current state
     */
    ProtocolState get_state() const { return state_; }
    
    /**
     * Check if in a specific state
     */
    bool is_in_state(ProtocolState s) const { return state_ == s; }
    
    /**
     * Check if connected (any state >= TCP_CONNECTED and < ERROR)
     */
    bool is_connected() const {
        return state_ >= ProtocolState::TCP_CONNECTED && state_ < ProtocolState::ERROR;
    }
    
    /**
     * Check if authenticated
     */
    bool is_authenticated() const {
        return is_state_authenticated(state_);
    }
    
    /**
     * Validate and execute state transition
     * 
     * @param cmd Command being executed
     * @param reply_code FTP server reply code (for post-execution transitions)
     * @return TransitionResult indicating success or failure
     */
    TransitionResult transition(FtpCommand cmd, uint16_t reply_code = 0) {
        switch (state_) {
            case ProtocolState::DISCONNECTED:
                return handle_disconnected(cmd, reply_code);
            case ProtocolState::TCP_CONNECTED:
                return handle_tcp_connected(cmd, reply_code);
            case ProtocolState::GREETING_WAIT:
                return handle_greeting_wait(cmd, reply_code);
            case ProtocolState::AUTH_IN_PROGRESS:
                return handle_auth_in_progress(cmd, reply_code);
            case ProtocolState::AUTHENTICATED:
                return handle_authenticated(cmd, reply_code);
            case ProtocolState::CWD_PENDING:
                return handle_cwd_pending(cmd, reply_code);
            case ProtocolState::PASV_PENDING:
                return handle_pasv_pending(cmd, reply_code);
            case ProtocolState::DATA_CONNECTING:
                return handle_data_connecting(cmd, reply_code);
            case ProtocolState::DATA_READY:
                return handle_data_ready(cmd, reply_code);
            case ProtocolState::DATA_TRANSFERRING:
                return handle_data_transferring(cmd, reply_code);
            case ProtocolState::DATA_FINALIZING:
                return handle_data_finalizing(cmd, reply_code);
            case ProtocolState::DISCONNECTING:
                return handle_disconnecting(cmd, reply_code);
            case ProtocolState::ERROR:
                return handle_error(cmd, reply_code);
            default:
                return TransitionResult::PROTOCOL_ERROR;
        }
    }
    
    /**
     * Force transition to error state
     */
    void set_error() { state_ = ProtocolState::ERROR; }
    
    /**
     * Set state directly (for initialization/testing)
     */
    void set_state(ProtocolState s) { state_ = s; }

private:
    ProtocolState state_;
    
    TransitionResult handle_disconnected(FtpCommand cmd, uint16_t) {
        if (cmd == FtpCommand::CONNECT) {
            state_ = ProtocolState::TCP_CONNECTED;
            return TransitionResult::SUCCESS;
        }
        return TransitionResult::INVALID_STATE;
    }
    
    TransitionResult handle_tcp_connected(FtpCommand cmd, uint16_t reply_code) {
        if (cmd == FtpCommand::USER) {
            // After sending USER, we get a reply (220 greeting already received, now 331 or 230)
            if (reply_code == 331 || reply_code == 230) {
                state_ = ProtocolState::AUTH_IN_PROGRESS;
                return TransitionResult::SUCCESS;
            }
        }
        if (cmd == FtpCommand::CONNECT) {
            // Already connected
            return TransitionResult::SUCCESS;
        }
        return TransitionResult::INVALID_STATE;
    }
    
    TransitionResult handle_greeting_wait(FtpCommand cmd, uint16_t reply_code) {
        if (cmd == FtpCommand::USER) {
            if (reply_code >= 200 && reply_code < 400) {
                state_ = ProtocolState::AUTH_IN_PROGRESS;
                return TransitionResult::SUCCESS;
            }
        }
        return TransitionResult::INVALID_STATE;
    }
    
    TransitionResult handle_auth_in_progress(FtpCommand cmd, uint16_t reply_code) {
        if (cmd == FtpCommand::PASS) {
            if (reply_code == 230) {
                // Login successful
                state_ = ProtocolState::AUTHENTICATED;
                return TransitionResult::SUCCESS;
            } else if (reply_code == 331) {
                // Need password (already sent, waiting for more)
                return TransitionResult::SUCCESS;
            } else if (reply_code >= 400) {
                // Auth failed
                state_ = ProtocolState::ERROR;
                return TransitionResult::PROTOCOL_ERROR;
            }
        }
        if (cmd == FtpCommand::USER && reply_code == 230) {
            // Some servers allow login with just USER (anonymous)
            state_ = ProtocolState::AUTHENTICATED;
            return TransitionResult::SUCCESS;
        }
        return TransitionResult::INVALID_STATE;
    }
    
    TransitionResult handle_authenticated(FtpCommand cmd, uint16_t reply_code) {
        switch (cmd) {
            case FtpCommand::QUIT:
                if (reply_code >= 200 && reply_code < 300) {
                    state_ = ProtocolState::DISCONNECTING;
                    return TransitionResult::SUCCESS;
                }
                break;
            case FtpCommand::NOOP:
            case FtpCommand::PWD:
            case FtpCommand::FEAT:
            case FtpCommand::SYST:
            case FtpCommand::TYPE:
                if (reply_code >= 200 && reply_code < 300) {
                    return TransitionResult::SUCCESS;  // Stay in AUTHENTICATED
                }
                break;
            case FtpCommand::CWD:
            case FtpCommand::CDUP:
            case FtpCommand::XCWD:
            case FtpCommand::XCUP:
                if (reply_code >= 200 && reply_code < 300) {
                    return TransitionResult::SUCCESS;  // Stay in AUTHENTICATED
                }
                break;
            case FtpCommand::PASV:
            case FtpCommand::EPSV:
                if (reply_code == 227 || reply_code == 229) {
                    state_ = ProtocolState::DATA_CONNECTING;
                    return TransitionResult::SUCCESS;
                }
                break;
            case FtpCommand::LIST:
            case FtpCommand::NLST:
            case FtpCommand::RETR:
            case FtpCommand::STOR:
            case FtpCommand::APPE:
                // Need PASV first
                return TransitionResult::INVALID_STATE;
            case FtpCommand::MKD:
            case FtpCommand::XMKD:
            case FtpCommand::DELE:
            case FtpCommand::RMD:
            case FtpCommand::XRMD:
            case FtpCommand::RNFR:
            case FtpCommand::SIZE:
            case FtpCommand::MDTM:
            case FtpCommand::OPTS:
            case FtpCommand::SITE:
                if (reply_code >= 200 && reply_code < 300) {
                    return TransitionResult::SUCCESS;
                }
                break;
            default:
                break;
        }
        
        if (reply_code >= 400) {
            state_ = ProtocolState::ERROR;
            return TransitionResult::PROTOCOL_ERROR;
        }
        
        return TransitionResult::SUCCESS;
    }
    
    TransitionResult handle_cwd_pending(FtpCommand, uint16_t) {
        // After CWD completes, return to AUTHENTICATED
        state_ = ProtocolState::AUTHENTICATED;
        return TransitionResult::SUCCESS;
    }
    
    TransitionResult handle_pasv_pending(FtpCommand, uint16_t) {
        return TransitionResult::INVALID_STATE;
    }
    
    TransitionResult handle_data_connecting(FtpCommand cmd, uint16_t /*reply_code*/) {
        if (cmd == FtpCommand::LIST || cmd == FtpCommand::NLST ||
            cmd == FtpCommand::RETR || cmd == FtpCommand::STOR ||
            cmd == FtpCommand::APPE) {
            state_ = ProtocolState::DATA_TRANSFERRING;
            return TransitionResult::SUCCESS;
        }
        return TransitionResult::INVALID_STATE;
    }
    
    TransitionResult handle_data_ready(FtpCommand, uint16_t) {
        return TransitionResult::INVALID_STATE;
    }
    
    TransitionResult handle_data_transferring(FtpCommand, uint16_t) {
        // Data transfer in progress
        return TransitionResult::SUCCESS;
    }
    
    TransitionResult handle_data_finalizing(FtpCommand cmd, uint16_t reply_code) {
        if (cmd == FtpCommand::LIST || cmd == FtpCommand::RETR || 
            cmd == FtpCommand::STOR || cmd == FtpCommand::APPE) {
            if (reply_code == 226 || reply_code == 250) {
                state_ = ProtocolState::AUTHENTICATED;
                return TransitionResult::SUCCESS;
            }
        }
        return TransitionResult::INVALID_STATE;
    }
    
    TransitionResult handle_disconnecting(FtpCommand, uint16_t) {
        state_ = ProtocolState::DISCONNECTED;
        return TransitionResult::SUCCESS;
    }
    
    TransitionResult handle_error(FtpCommand, uint16_t) {
        // In error state, all commands fail
        return TransitionResult::INVALID_STATE;
    }
};

}} // namespace ftpclient::protocol

#endif /* FTPCLIENT_STATE_MACHINE_HPP */
