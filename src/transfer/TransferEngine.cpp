/*
 * TransferEngine.cpp - Main Transfer Engine Implementation for Phase 4
 * 
 * Implements concurrent directory upload orchestration.
 * Per Phase 4 spec Section 3 and Section 5.
 */

#include "TransferEngine.hpp"
#include "Task.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ftpclient { namespace transfer {

TransferEngine::TransferEngine(protocol::ProtocolEngine& protocol_engine,
                               const TransferConfig& config)
    : protocol_engine_(protocol_engine)
    , config_(config)
    , thread_pool_(ThreadPoolConfig{config.max_parallel})
    , buffer_pool_(BufferPoolConfig{config.buffer_size, config.max_parallel * 2})
{
    worker_sessions_.resize(thread_pool_.get_worker_count());
    thread_pool_.set_worker_callback([this](Task& task, uint32_t worker_id) {
        execute_worker_task(task, worker_id);
    });
}

TransferEngine::~TransferEngine() {
    /* Thread pool destructor waits for all tasks */
}

int32_t TransferEngine::upload_directory(
    const std::string& local_root,
    const std::string& remote_root,
    ftp_progress_cb_t progress_cb,
    void* progress_user_data
) {
    namespace fs = std::filesystem;
    
    cancel_flag_.store(false, std::memory_order_release);
    result_aggregator_.clear();
    
    /* Step 1: TRAVERSE - Produce FileManifest (Phase 2 DirectoryWalker) */
    protocol::FileManifest manifest;
    protocol::TraversalConfig traverse_config;
    traverse_config.max_depth = 0;  /* Unlimited */
    traverse_config.symlink_policy = 1;  /* FOLLOW */
    
    auto walk_error = protocol_engine_.traverse_local(local_root, remote_root, 
                                                       traverse_config, manifest);
    if (walk_error != protocol::WalkError::SUCCESS) {
        return static_cast<int32_t>(walk_error);
    }
    
    if (manifest.entries.empty()) {
        return 0;  /* Nothing to upload */
    }
    
    /* Step 2: SORT & PARTITION - Separate dirs and files */
    std::vector<protocol::FileManifestEntry> dirs;
    std::vector<protocol::FileManifestEntry> files;
    
    for (auto& entry : manifest.entries) {
        if (entry.is_directory) {
            dirs.push_back(std::move(entry));
        } else {
            files.push_back(std::move(entry));
        }
    }
    
    /* Sort directories in pre-order (parent before children) */
    std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
        return a.remote_relative_path.length() < b.remote_relative_path.length();
    });
    
    /* Sort files by size descending (largest first) per spec Section 5.3 */
    sort_files_largest_first(files);
    
    /* Step 3: BATCH MKDIR - Create remote directories */
    if (config_.create_remote_dirs) {
        for (const auto& dir : dirs) {
            int32_t ret = execute_mkdir_task(dir.remote_relative_path);
            if (ret != 0 && ret != -502) {  /* -502 = already exists, continue */
                /* Abort on permission denied or other errors */
                return ret;
            }
        }
    }
    
    /* Step 4: UPLOAD - use independent authenticated sessions when bounded
       parallelism is requested and more than one file is available. */
    result_aggregator_.set_files_total(files.size());
    const bool use_parallel_sessions = files.size() > 1 &&
                                       thread_pool_.get_worker_count() > 1;
    for (auto& file : files) {
        Task task;
        task.type = TaskType::UPLOAD_FILE;
        task.local_path = file.local_absolute_path;
        task.remote_path = file.remote_relative_path;
        task.file_size = file.size_bytes;
        task.progress_cb = reinterpret_cast<void*>(progress_cb);
        task.progress_user_data = progress_user_data;
        if (use_parallel_sessions) {
            if (!thread_pool_.enqueue(std::move(task))) {
                cancel_flag_.store(true, std::memory_order_release);
                break;
            }
        } else {
            execute_upload_task(task);
        }
    }
    if (use_parallel_sessions) {
        thread_pool_.wait_for_all();
    }

    /* Step 5: AGGREGATE RESULTS */
    return result_aggregator_.get_worst_status();
}

int32_t TransferEngine::upload_single_file(
    const std::string& local_path,
    const std::string& remote_path,
    ftp_progress_cb_t progress_cb,
    void* progress_user_data
) {
    namespace fs = std::filesystem;
    if (local_path.empty() || remote_path.empty() || !fs::is_regular_file(local_path)) {
        return -202;
    }

    result_aggregator_.clear();
    result_aggregator_.set_files_total(1);
    Task task;
    task.type = TaskType::UPLOAD_FILE;
    task.local_path = local_path;
    task.remote_path = remote_path;
    task.file_size = fs::file_size(local_path);
    task.progress_cb = reinterpret_cast<void*>(progress_cb);
    task.progress_user_data = progress_user_data;
    execute_upload_task(task);
    return result_aggregator_.get_worst_status();
}

void TransferEngine::execute_upload_task(Task& task, protocol::ProtocolEngine* session) {
    protocol::ProtocolEngine& active_session = session != nullptr ? *session : protocol_engine_;
    ProgressState state;
    state.bytes_total = task.file_size;

    protocol::FileManifestEntry entry;
    entry.local_absolute_path = task.local_path;
    entry.remote_relative_path = task.remote_path;
    entry.size_bytes = task.file_size;

    resilience::RetryConfig retry_config;
    retry_config.max_attempts = config_.retry_attempts;
    retry_config.base_delay_ms = config_.retry_base_delay_ms;
    retry_config.max_delay_ms = config_.retry_max_delay_ms;
    resilience::RetryPolicy retry_policy(retry_config);

    uint64_t bytes_sent = 0;
    uint32_t attempts = 0;
    task.result_status = retry_policy.execute_with_retry([&]() {
        uint64_t restart_offset = 0;
        if (config_.resume_enabled) {
            uint64_t remote_size = 0;
            int32_t size_ret = active_session.get_remote_file_size(
                entry.remote_relative_path, &remote_size);
            if (size_ret == 0 && remote_size > 0 && remote_size < entry.size_bytes) {
                restart_offset = remote_size;
            }
        }

        bytes_sent = 0;
        return active_session.upload_file(
            entry,
            &bytes_sent,
            [&](uint64_t current, uint64_t total) {
                state.bytes_current = current;
                state.bytes_total = total;
                std::lock_guard<std::mutex> lock(progress_mutex_);
                invoke_progress_callback(
                    task.local_path, task.remote_path, current, total,
                    reinterpret_cast<ftp_progress_cb_t>(task.progress_cb),
                    task.progress_user_data, state);
            },
            restart_offset);
    }, &attempts);

    task.bytes_sent = bytes_sent;
    if (task.progress_cb) {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        auto cb = reinterpret_cast<ftp_progress_cb_t>(task.progress_cb);
        cb(task.local_path.c_str(), task.remote_path.c_str(),
           state.bytes_current, state.bytes_total, state.bytes_per_second,
           task.progress_user_data);
    }

    result_aggregator_.record_result(task.local_path, task.remote_path,
                                      task.result_status, task.bytes_sent,
                                      attempts == 0 ? 1 : attempts,
                                      task.result_status);
    if (task.bytes_sent > 0) {
        result_aggregator_.add_bytes_transferred(task.bytes_sent);
    }
}

void TransferEngine::execute_worker_task(Task& task, uint32_t worker_id) {
    try {
        if (worker_id >= worker_sessions_.size()) {
            throw std::out_of_range("worker id outside session pool");
        }
        auto& session = worker_sessions_[worker_id];
        if (!session) {
            session = std::make_unique<protocol::ProtocolEngine>();
            session->get_config() = protocol_engine_.get_config();
        }

        if (!session->is_authenticated()) {
            int32_t connect_status = session->connect(protocol_engine_.get_credentials());
            if (connect_status != 0) {
                task.result_status = connect_status;
                result_aggregator_.record_result(task.local_path, task.remote_path,
                                                  connect_status, 0, 1, connect_status);
                session->disconnect();
                return;
            }
        }

        execute_upload_task(task, session.get());
        if (task.result_status != 0 || !session->is_authenticated()) {
            session->disconnect();
            session.reset();
        }
    } catch (...) {
        if (worker_id < worker_sessions_.size() && worker_sessions_[worker_id]) {
            worker_sessions_[worker_id]->disconnect();
            worker_sessions_[worker_id].reset();
        }
        task.result_status = -501;  // FTP_ERR_PROTOCOL
        result_aggregator_.record_result(task.local_path, task.remote_path,
                                          task.result_status, 0, 1,
                                          task.result_status);
    }
}

int32_t TransferEngine::execute_mkdir_task(const std::string& remote_dir) {
    /* Use ProtocolEngine to create remote directory */
    /* This calls MKD command via control channel */
    return protocol_engine_.create_remote_dir(remote_dir);
}

void TransferEngine::invoke_progress_callback(
    const std::string& local_path,
    const std::string& remote_path,
    uint64_t bytes_current,
    uint64_t bytes_total,
    ftp_progress_cb_t progress_cb,
    void* user_data,
    ProgressState& state
) {
    if (!progress_cb) {
        return;
    }
    
    /* Throttle to 10Hz max per spec Section 8.1 */
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.last_callback_time).count();
    
    if (elapsed < 100) {
        return;  /* Too soon since last callback */
    }
    
    /* Calculate bytes per second */
    auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.start_time).count();
    
    if (total_elapsed > 0) {
        state.bytes_per_second = static_cast<double>(bytes_current) * 1000.0 
                                 / static_cast<double>(total_elapsed);
    }
    
    state.last_callback_time = now;
    
    /* Invoke callback */
    progress_cb(local_path.c_str(), remote_path.c_str(),
                bytes_current, bytes_total, state.bytes_per_second, user_data);
}

void TransferEngine::sort_files_largest_first(
    std::vector<protocol::FileManifestEntry>& files) {
    std::sort(files.begin(), files.end(), 
              [](const protocol::FileManifestEntry& a, 
                 const protocol::FileManifestEntry& b) {
        return a.size_bytes > b.size_bytes;  /* Descending order */
    });
}

void TransferEngine::fill_result(void* out_result, void* out_file_results) {
    /* Fill C ABI result structure */
    /* This is called after upload_directory completes */
    (void)out_result;
    (void)out_file_results;
    /* Actual implementation fills the C struct from result_aggregator_ */
}

}} // namespace ftpclient::transfer
