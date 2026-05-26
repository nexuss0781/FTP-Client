/*
 * ResultAggregator.hpp - Per-File Result Tracking for Phase 4
 * 
 * Tracks per-file results and aggregates statistics.
 * Per Phase 4 spec Section 9.
 */

#ifndef FTPCLIENT_TRANSFER_RESULT_AGGREGATOR_HPP
#define FTPCLIENT_TRANSFER_RESULT_AGGREGATOR_HPP

#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace ftpclient { namespace transfer {

/**
 * Per-file result structure (C++ internal)
 * Matches ftp_file_result_t from C ABI
 * Per spec Section 9.1
 */
struct FileResult {
    std::string local_path;
    std::string remote_path;
    int32_t status;             /* FTP_OK or error code */
    uint64_t bytes_sent;
    
    FileResult() : status(0), bytes_sent(0) {}
};

/**
 * Result Aggregator
 * 
 * Thread-safe aggregation of per-file results and overall statistics.
 * Per spec Section 9.
 */
class ResultAggregator {
public:
    ResultAggregator();
    
    /**
     * Record a file result
     * 
     * @param local_path Local file path
     * @param remote_path Remote file path  
     * @param status FTP_OK or error code
     * @param bytes_sent Bytes successfully transferred
     */
    void record_result(const std::string& local_path, 
                       const std::string& remote_path,
                       int32_t status,
                       uint64_t bytes_sent);
    
    /**
     * Add to total bytes transferred counter
     */
    void add_bytes_transferred(uint64_t bytes);
    
    /**
     * Get total files count
     */
    uint64_t get_files_total() const { return files_total_.load(std::memory_order_acquire); }
    
    /**
     * Get success count
     */
    uint64_t get_files_success() const { return files_success_.load(std::memory_order_acquire); }
    
    /**
     * Get failed count
     */
    uint64_t get_files_failed() const { return files_failed_.load(std::memory_order_acquire); }
    
    /**
     * Get total bytes transferred
     */
    uint64_t get_bytes_transferred() const { return bytes_transferred_.load(std::memory_order_acquire); }
    
    /**
     * Get worst error status
     */
    int32_t get_worst_status() const { return worst_status_.load(std::memory_order_acquire); }
    
    /**
     * Set expected total files (for progress tracking)
     */
    void set_files_total(uint64_t total) { files_total_.store(total, std::memory_order_release); }
    
    /**
     * Get all file results (thread-safe copy)
     */
    std::vector<FileResult> get_results() const;
    
    /**
     * Clear all results (for reuse)
     */
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<FileResult> results_;
    std::atomic<uint64_t> files_total_{0};
    std::atomic<uint64_t> files_success_{0};
    std::atomic<uint64_t> files_failed_{0};
    std::atomic<uint64_t> bytes_transferred_{0};
    std::atomic<int32_t> worst_status_{0};
};

}} // namespace ftpclient::transfer

#endif /* FTPCLIENT_TRANSFER_RESULT_AGGREGATOR_HPP */
