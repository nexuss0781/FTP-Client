/*
 * ControlThread.hpp - FTP Control Channel Thread Management
 * 
 * Manages the dedicated control channel thread, command queue, and read loop.
 * Per Phase 2 spec Section 4.
 */

#ifndef FTPCLIENT_CONTROL_THREAD_HPP
#define FTPCLIENT_CONTROL_THREAD_HPP

#include "Transport.hpp"
#include "StateMachine.hpp"
#include "ReplyParser.hpp"
#include "ErrorMap.hpp"
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <future>
#include <atomic>
#include <chrono>

namespace ftpclient { namespace protocol {

/**
 * Command structure for the control channel queue
 * Per spec Section 4.2
 */
struct Command {
    std::string verb;                    // e.g., "USER", "PASV", "STOR"
    std::string arg;                     // Payload, may be empty
    std::promise<int32_t> result;        // Fulfilled by control thread with error code
    
    Command() = default;
    Command(const std::string& v, const std::string& a) : verb(v), arg(a) {}
};

/**
 * Control Channel Thread
 * 
 * Dedicated thread for managing FTP control channel communication.
 * Handles command queuing, response parsing, and state machine transitions.
 */
class ControlThread {
public:
    /**
     * Default idle timeout before sending NOOP (30 seconds per spec Section 9.2)
     */
    static constexpr uint32_t DEFAULT_IDLE_TIMEOUT_MS = 30000;
    
    /**
     * Default command timeout (30 seconds per spec Section 6)
     */
    static constexpr uint32_t DEFAULT_COMMAND_TIMEOUT_MS = 30000;
    
    ControlThread();
    ~ControlThread();
    
    // Prevent copying
    ControlThread(const ControlThread&) = delete;
    ControlThread& operator=(const ControlThread&) = delete;
    
    /**
     * Start the control thread with an existing transport
     * 
     * @param transport Transport instance for control channel (must be connected)
     * @param initial_host Host IP for NAT detection in data channels
     * @return 0 on success, negative error code on failure
     */
    int32_t start(std::unique_ptr<Transport> transport, const std::string& initial_host);
    
    /**
     * Stop the control thread gracefully
     */
    void stop();
    
    /**
     * Enqueue a command for execution
     * 
     * @param verb FTP command verb
     * @param arg Command argument (may be empty)
     * @param timeout_ms Timeout in milliseconds (0 = use default)
     * @return Future that will contain result code
     */
    std::future<int32_t> enqueue_command(const std::string& verb, const std::string& arg, 
                                          uint32_t timeout_ms = 0);
    
    /**
     * Send QUIT and close connection gracefully
     * 
     * @return Result code
     */
    int32_t disconnect();
    
    /**
     * Check if control thread is running
     */
    bool is_running() const { return running_.load(std::memory_order_acquire); }
    
    /**
     * Get current protocol state
     */
    ProtocolState get_state() const;
    
    /**
     * Check if authenticated
     */
    bool is_authenticated() const;
    
    /**
     * Set command timeout
     */
    void set_command_timeout(uint32_t ms) { command_timeout_ms_ = ms; }
    
    /**
     * Get the host IP for this connection (for NAT detection)
     */
    const std::string& get_host() const { return host_; }

private:
    /**
     * Main control thread loop
     */
    void run_loop();
    
    /**
     * Execute a single command
     */
    int32_t execute_command(Command& cmd);
    
    /**
     * Read and parse server response
     */
    int32_t read_response(FtpReply& reply);
    
    /**
     * Send raw command to server
     */
    int32_t send_command(const std::string& verb, const std::string& arg);
    
    /**
     * Map FTP command verb to state machine enum
     */
    FtpCommand map_verb_to_command(const std::string& verb);
    
    std::unique_ptr<Transport> transport_;
    std::thread thread_;
    std::queue<Command> command_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::atomic<bool> running_;
    std::atomic<bool> stopped_;
    
    StateMachine state_machine_;
    std::string host_;
    uint32_t command_timeout_ms_;
    uint32_t idle_timeout_ms_;
    
    // Receive buffer for response parsing
    std::string recv_buffer_;
    static constexpr size_t RECV_BUFFER_SIZE = 4096;
};

inline ControlThread::ControlThread()
    : running_(false)
    , stopped_(false)
    , command_timeout_ms_(DEFAULT_COMMAND_TIMEOUT_MS)
    , idle_timeout_ms_(DEFAULT_IDLE_TIMEOUT_MS)
{
}

inline ControlThread::~ControlThread() {
    stop();
}

inline int32_t ControlThread::start(std::unique_ptr<Transport> transport, const std::string& initial_host) {
    if (running_.load(std::memory_order_acquire)) {
        return -1;  // Already running
    }
    
    if (!transport || !transport->is_connected()) {
        return -401;  // FTP_ERR_CONNECT
    }
    
    transport_ = std::move(transport);
    host_ = initial_host;
    stopped_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    
    // Pre-allocate receive buffer
    recv_buffer_.reserve(RECV_BUFFER_SIZE);
    
    try {
        thread_ = std::thread(&ControlThread::run_loop, this);
        return 0;
    } catch (...) {
        running_.store(false, std::memory_order_release);
        return -102;  // FTP_ERR_SYSTEM
    }
}

inline void ControlThread::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    
    // Signal stop
    running_.store(false, std::memory_order_release);
    queue_cv_.notify_one();
    
    // Wait for thread to finish
    if (thread_.joinable()) {
        thread_.join();
    }
    
    stopped_.store(true, std::memory_order_release);
    
    // Close transport
    if (transport_) {
        transport_->shutdown();
        transport_.reset();
    }
}

inline std::future<int32_t> ControlThread::enqueue_command(const std::string& verb, 
                                                            const std::string& arg,
                                                            uint32_t timeout_ms) {
    (void)timeout_ms;  // Phase 2: timeout not yet implemented
    
    Command cmd(verb, arg);
    auto future = cmd.result.get_future();
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        command_queue_.push(std::move(cmd));
    }
    
    queue_cv_.notify_one();
    return future;
}

inline int32_t ControlThread::disconnect() {
    auto future = enqueue_command("QUIT", "");
    
    if (future.valid()) {
        future.wait();
        return future.get();
    }
    
    return -203;  // FTP_ERR_INVALID_STATE
}

inline ProtocolState ControlThread::get_state() const {
    return state_machine_.get_state();
}

inline bool ControlThread::is_authenticated() const {
    return state_machine_.is_authenticated();
}

inline void ControlThread::run_loop() {
    while (running_.load(std::memory_order_acquire)) {
        Command cmd;
        bool has_command = false;
        
        // Wait for command with timeout
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            
            if (queue_cv_.wait_for(lock, std::chrono::milliseconds(idle_timeout_ms_),
                                   [this] { return !command_queue_.empty() || !running_.load(); })) {
                if (!command_queue_.empty()) {
                    cmd = std::move(command_queue_.front());
                    command_queue_.pop();
                    has_command = true;
                }
            }
        }
        
        // If no command and still running, check if we should send NOOP
        if (!has_command && running_.load(std::memory_order_acquire)) {
            // Only send NOOP if authenticated (per spec Section 9.2)
            if (state_machine_.is_authenticated()) {
                cmd.verb = "NOOP";
                cmd.arg = "";
                has_command = true;
            } else {
                continue;
            }
        }
        
        if (!has_command) {
            continue;
        }
        
        // Execute command
        int32_t result = execute_command(cmd);
        
        // Set promise value
        try {
            cmd.result.set_value(result);
        } catch (...) {
            // Promise already satisfied or broken
        }
        
        // Check for fatal errors
        if (result < 0 && state_machine_.get_state() == ProtocolState::ERROR) {
            break;
        }
    }
}

inline int32_t ControlThread::execute_command(Command& cmd) {
    // Validate state
    FtpCommand ftp_cmd = map_verb_to_command(cmd.verb);
    
    // Send command
    int32_t ret = send_command(cmd.verb, cmd.arg);
    if (ret != 0) {
        state_machine_.set_error();
        return ret;
    }
    
    // Read response
    FtpReply reply;
    ret = read_response(reply);
    if (ret != 0) {
        state_machine_.set_error();
        return ret;
    }
    
    // Update state machine
    auto transition = state_machine_.transition(ftp_cmd, reply.code);
    
    if (transition == TransitionResult::INVALID_STATE) {
        return -203;  // FTP_ERR_INVALID_STATE
    }
    
    // Map response code to error
    if (!is_ftp_success(reply.code)) {
        return map_ftp_code_to_error(reply.code);
    }
    
    return 0;
}

inline int32_t ControlThread::read_response(FtpReply& reply) {
    recv_buffer_.clear();
    char buffer[RECV_BUFFER_SIZE];
    
    while (true) {
        int32_t bytes_read = transport_->read(buffer, sizeof(buffer));
        
        if (bytes_read < 0) {
            return bytes_read;  // Error
        }
        
        if (bytes_read == 0) {
            // Connection closed
            return -403;  // FTP_ERR_NETWORK_RESET
        }
        
        recv_buffer_.append(buffer, static_cast<size_t>(bytes_read));
        
        // Try to parse
        size_t consumed = 0;
        auto result = ReplyParser::parse(recv_buffer_.c_str(), recv_buffer_.size(), reply, consumed);
        
        if (result == ParseResult::COMPLETE) {
            // Remove consumed bytes from buffer
            if (consumed < recv_buffer_.size()) {
                recv_buffer_ = recv_buffer_.substr(consumed);
            } else {
                recv_buffer_.clear();
            }
            return 0;
        } else if (result == ParseResult::NEED_MORE_DATA) {
            // Continue reading
            continue;
        } else {
            // Malformed or overflow
            return -501;  // FTP_ERR_PROTOCOL
        }
    }
}

inline int32_t ControlThread::send_command(const std::string& verb, const std::string& arg) {
    std::string command;
    
    if (arg.empty()) {
        command = verb + "\r\n";
    } else {
        command = verb + " " + arg + "\r\n";
    }
    
    int32_t ret = transport_->write(command.c_str(), static_cast<uint32_t>(command.size()));
    
    if (ret < 0) {
        return ret;
    }
    
    if (static_cast<size_t>(ret) != command.size()) {
        return -403;  // FTP_ERR_NETWORK_RESET
    }
    
    return 0;
}

inline FtpCommand ControlThread::map_verb_to_command(const std::string& verb) {
    if (verb == "USER") return FtpCommand::USER;
    if (verb == "PASS") return FtpCommand::PASS;
    if (verb == "QUIT") return FtpCommand::QUIT;
    if (verb == "NOOP") return FtpCommand::NOOP;
    if (verb == "PWD") return FtpCommand::PWD;
    if (verb == "CWD") return FtpCommand::CWD;
    if (verb == "CDUP") return FtpCommand::CDUP;
    if (verb == "PASV") return FtpCommand::PASV;
    if (verb == "EPSV") return FtpCommand::EPSV;
    if (verb == "LIST") return FtpCommand::LIST;
    if (verb == "NLST") return FtpCommand::NLST;
    if (verb == "RETR") return FtpCommand::RETR;
    if (verb == "STOR") return FtpCommand::STOR;
    if (verb == "APPE") return FtpCommand::APPE;
    if (verb == "DELE") return FtpCommand::DELE;
    if (verb == "MKD") return FtpCommand::MKD;
    if (verb == "RMD") return FtpCommand::RMD;
    if (verb == "RNFR") return FtpCommand::RNFR;
    if (verb == "RNTO") return FtpCommand::RNTO;
    if (verb == "SIZE") return FtpCommand::SIZE;
    if (verb == "REST") return FtpCommand::REST;
    if (verb == "FEAT") return FtpCommand::FEAT;
    if (verb == "TYPE") return FtpCommand::TYPE;
    if (verb == "SYST") return FtpCommand::SYST;
    if (verb == "OPTS") return FtpCommand::OPTS;
    if (verb == "MDTM") return FtpCommand::MDTM;
    
    return FtpCommand::SITE;  // Default to SITE for unknown
}

}} // namespace ftpclient::protocol

#endif /* FTPCLIENT_CONTROL_THREAD_HPP */
