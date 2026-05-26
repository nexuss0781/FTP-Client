/*
 * TransferEngine.hpp - Main Transfer Engine for Phase 4
 * 
 * Orchestrates concurrent directory uploads using thread pool.
 * Per Phase 4 spec Section 3.
 */

#ifndef FTPCLIENT_TRANSFER_ENGINE_HPP
#define FTPCLIENT_TRANSFER_ENGINE_HPP

#include "ThreadPool.hpp"
#include "BufferPool.hpp"
#include "ResultAggregator.hpp"
#include "Task.hpp"
#include "../protocol/ProtocolEngine.hpp"
#include <functional>
#include <chrono>

namespace ftpclient { namespace transfer {

/* Forward declare C ABI types */
typedef void (*ftp_progress_cb_t)(
    const char* local_path,
    const char* remote_path,
    uint64_t    bytes_current,
    uint64_t    bytes_total,
    double      bytes_per_second,
    void*       user_data
);

/**
 * Transfer Engine Configuration
 */
struct TransferConfig {
    uint32_t max_parallel;          /* Max concurrent uploads (0=default 4) */
    int32_t resume_enabled;         /* 0=overwrite, 1=resume partial */
    int32_t create_remote_dirs;     /* 0=fail if missing, 1=auto-create */
    uint32_t buffer_size;           /* Buffer size in bytes */
    
    TransferConfig()
        : max_parallel(0)
        , resume_enabled(0)
        , create_remote_dirs(1)
        , buffer_size(256 * 1024)
    {}
};

/**
 * Progress tracking per file
 */
struct ProgressState {
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_callback_time;
    uint64_t bytes_current;
    uint64_t bytes_total;
    double bytes_per_second;
    
    ProgressState()
        : bytes_current(0)
        , bytes_total(0)
        , bytes_per_second(0.0)
    {
        start_time = std::chrono::steady_clock::now();
        last_callback_time = start_time;
    }
};

/**
 * Transfer Engine
 * 
 * Main orchestration component for Phase 4 directory uploads.
 * Implements the algorithm from spec Section 5.1.
 */
class TransferEngine {
public:
    /**
     * Construct transfer engine
     * 
     * @param protocol_engine Protocol engine for control channel operations
     * @param config Transfer configuration
     */
    TransferEngine(protocol::ProtocolEngine& protocol_engine, 
                   const TransferConfig& config = TransferConfig());
    
    /**
     * Destructor - waits for all transfers to complete
     */
    ~TransferEngine();
    
    /* Disable copying */
    TransferEngine(const TransferEngine&) = delete;
    TransferEngine& operator=(const TransferEngine&) = delete;
    
    /**
     * Upload a directory tree
     * 
     * @param local_root Local directory path
     * @param remote_root Remote directory path
     * @param progress_cb Progress callback (may be nullptr)
     * @param progress_user_data User data for progress callback
     * @return 0 on success (FTP_OK), negative error code on failure
     */
    int32_t upload_directory(
        const std::string& local_root,
        const std::string& remote_root,
        ftp_progress_cb_t progress_cb,
        void* progress_user_data
    );
    
    /**
     * Get result aggregator for post-upload statistics
     */
    const ResultAggregator& get_result_aggregator() const { return result_aggregator_; }
    
    /**
     * Fill C ABI result structure
     * 
     * @param out_result Pointer to C result structure
     * @param out_file_results Pointer to vector of C file results
     */
    void fill_result(void* out_result, void* out_file_results);

private:
    protocol::ProtocolEngine& protocol_engine_;
    TransferConfig config_;
    ThreadPool thread_pool_;
    BufferPool buffer_pool_;
    ResultAggregator result_aggregator_;
    
    std::atomic<bool> cancel_flag_{false};
    std::mutex control_mutex_;  /* For control channel access */
    
    /**
     * Worker function for upload tasks
     */
    void execute_upload_task(Task& task);
    
    /**
     * Worker function for mkdir tasks
     */
    int32_t execute_mkdir_task(const std::string& remote_dir);
    
    /**
     * Invoke progress callback with throttling (10Hz max)
     */
    void invoke_progress_callback(
        const std::string& local_path,
        const std::string& remote_path,
        uint64_t bytes_current,
        uint64_t bytes_total,
        ftp_progress_cb_t progress_cb,
        void* user_data,
        ProgressState& state
    );
    
    /**
     * Sort files by size descending (largest first)
     */
    void sort_files_largest_first(std::vector<protocol::FileManifestEntry>& files);
};

}} // namespace ftpclient::transfer

#endif /* FTPCLIENT_TRANSFER_ENGINE_HPP */
