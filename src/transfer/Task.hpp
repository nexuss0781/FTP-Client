/*
 * Task.hpp - Transfer Task Definitions for Phase 4
 * 
 * Defines task types for the transfer engine thread pool.
 * Per Phase 4 spec Section 4.2.
 */

#ifndef FTPCLIENT_TRANSFER_TASK_HPP
#define FTPCLIENT_TRANSFER_TASK_HPP

#include <string>
#include <atomic>
#include <cstdint>

namespace ftpclient { namespace transfer {

/**
 * Task type enumeration
 * Per spec Section 4.2
 */
enum class TaskType : int32_t {
    MKDIR = 0,
    UPLOAD_FILE = 1
};

/**
 * Transfer task structure
 * Per spec Section 4.2
 * 
 * Tasks are enqueued by the TransferEngine and executed by worker threads.
 * Each task carries its own completion tracking via atomic counter.
 */
struct Task {
    TaskType type;                      /* Task type: MKDIR or UPLOAD_FILE */
    
    /* For UPLOAD_FILE */
    std::string local_path;             /* Local file path (UTF-8) */
    std::string remote_path;            /* Remote file path (forward-slash separated) */
    uint64_t file_size;                 /* File size in bytes */
    
    /* For MKDIR */
    std::string remote_dir;             /* Remote directory path to create */
    
    /* Completion tracking - decremented on finish */
    std::atomic<uint32_t>* completion_counter;
    
    /* Progress callback data (for UPLOAD_FILE) */
    void* progress_cb;                  /* ftp_progress_cb_t cast to void* */
    void* progress_user_data;           /* User data for progress callback */
    
    /* Result tracking */
    int32_t result_status;              /* FTP_OK or error code */
    uint64_t bytes_sent;                /* Bytes successfully transferred */
    
    Task() 
        : type(TaskType::MKDIR)
        , file_size(0)
        , completion_counter(nullptr)
        , progress_cb(nullptr)
        , progress_user_data(nullptr)
        , result_status(0)
        , bytes_sent(0)
    {}
    
    Task(Task&& other) noexcept
        : type(other.type)
        , local_path(std::move(other.local_path))
        , remote_path(std::move(other.remote_path))
        , file_size(other.file_size)
        , remote_dir(std::move(other.remote_dir))
        , completion_counter(other.completion_counter)
        , progress_cb(other.progress_cb)
        , progress_user_data(other.progress_user_data)
        , result_status(other.result_status)
        , bytes_sent(other.bytes_sent)
    {
        other.completion_counter = nullptr;
        other.progress_cb = nullptr;
        other.progress_user_data = nullptr;
    }
    
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            type = other.type;
            local_path = std::move(other.local_path);
            remote_path = std::move(other.remote_path);
            file_size = other.file_size;
            remote_dir = std::move(other.remote_dir);
            completion_counter = other.completion_counter;
            progress_cb = other.progress_cb;
            progress_user_data = other.progress_user_data;
            result_status = other.result_status;
            bytes_sent = other.bytes_sent;
            
            other.completion_counter = nullptr;
            other.progress_cb = nullptr;
            other.progress_user_data = nullptr;
        }
        return *this;
    }
    
    /* Disable copying */
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
};

}} // namespace ftpclient::transfer

#endif /* FTPCLIENT_TRANSFER_TASK_HPP */
