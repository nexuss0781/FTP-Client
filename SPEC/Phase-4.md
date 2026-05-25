# Phase 4 Specification: Transfer Engine & Concurrency

**Document Version:** 4.0-DRAFT  
**Depends On:** Phase 1 (Frozen ABI), Phase 2 (Protocol Engine), Phase 3 (Security/TLS)  
**Scope:** Concurrent file transfers, thread pool scheduling, directory upload orchestration, progress callbacks, and result aggregation. No retry logic (Phase 5), no Python binding ergonomics (Phase 6), no platform zero-copy optimizations (Phase 7).

---

## 1. Purpose & Scope

Phase 4 transforms the protocol engine into a **high-throughput batch transfer system**. It implements the `ftp_upload_dir()` stub from Phase 1 using the manifest producer from Phase 2 and the secure transport from Phase 3. The core objective is **bandwidth saturation** through controlled parallelism while maintaining server-friendly behavior.

**In Scope:**
- Fixed-size thread pool with work-stealing task distribution
- Concurrent file uploads (configurable parallelism, default 4, max 16)
- Directory tree upload orchestration: local traversal → remote directory creation → concurrent file upload → result aggregation
- Connection-per-file data channels with optional connection reuse for control
- Progress callback throttling and thread-safe invocation
- Partial failure handling: per-file status enumeration without aborting the batch
- `ftp_result_t` population with atomic counters
- Graceful backpressure: server `421 Too many connections` handling

**Explicitly Out of Scope:**
- Retry, exponential backoff, circuit breaker (Phase 5)
- Download operations (mirror spec; deferred to Phase 7 or separate major version)
- Python-specific binding code (Phase 6)
- `sendfile()` zero-copy, io_uring, memory-mapped I/O (Phase 7)
- Bandwidth throttling / rate limiting (Phase 7)
- Delta/sync logic (file comparison, timestamp checks) (Phase 7)

---

## 2. Architectural Decisions & Trade-offs

| Decision | Selected Approach | Rejected Alternative | Rationale |
|----------|------------------|---------------------|-----------|
| **Concurrency Model** | Fixed thread pool + task queue | One thread per file (unbounded) | Unbounded threads exhaust file descriptors and trigger server connection limits. Fixed pool with task queue provides deterministic resource usage. |
| **Thread Pool Size** | `hardware_concurrency()` or user override, clamped 1–16 | Always `hardware_concurrency()` | User may know server limits better than CPU count. 16 is the hard ceiling to prevent server-side SYN flood or connection exhaustion. |
| **Control Channel** | One persistent control channel per handle | One control channel per worker thread | FTP servers track state (CWD, TYPE) per control connection. Multiple control channels would require session replication and complicate error attribution. Single control channel is the standard model. |
| **Data Channel** | Fresh passive data connection per file transfer | Persistent data channel reuse | FTP data channels are unidirectional and close after each transfer (`STOR`/`RETR`). Reuse is non-standard and unsupported by most servers. Fresh PASV per file is the only robust approach. |
| **Work Queue Ordering** | Largest-file-first within each directory level | Strict depth-first order | Uploading large files first maximizes parallel utilization. Small files at the end create a "tail latency" problem where workers finish early and sit idle. |
| **Directory Creation** | Batched on control channel before file uploads | Interleaved (mkdir before each file) | Batching reduces control channel round-trips. For a tree with 1,000 dirs, interleaved costs ~1,000 RTTs; batching costs ~1,000 commands but can be pipelined where safe (Section 6.3). |
| **Progress Callback** | Throttled to 10Hz max, per-file granularity | Every buffer write invokes callback | 10Hz prevents GIL storms in Python. Per-file granularity (bytes_current, bytes_total) is sufficient for UI progress bars. |
| **Result Aggregation** | Atomic counters + per-file failure vector | Simple success/fail boolean | Batch uploads require knowing *which* files failed for caller retry decisions. The vector is allocated in C++ and freed via future `ftp_result_free()` (Phase 4 amendment to ABI). |
| **Buffer Strategy** | Fixed-size heap buffer pool (256KB default) | Stack-allocated per transfer | Stack buffers > few KB are unsafe and cause cache pressure. Heap pool allows buffer reuse across transfers, reducing allocator contention. |

---

## 3. Transfer Engine Architecture

### 3.1 Component Hierarchy
```
ftp_client_t handle (Phase 1 opaque)
  └── FtpClientImpl (internal C++)
        ├── ProtocolEngine (Phase 2: control thread, state machine)
        ├── CredentialVault (Phase 3: secure memory)
        └── TransferEngine (Phase 4: NEW)
              ├── ThreadPool (fixed workers)
              ├── TaskQueue (priority queue)
              ├── BufferPool (reusable 256KB chunks)
              ├── ProgressDispatcher (throttle + callback marshaling)
              └── ResultAggregator (atomic counters + per-file log)
```

### 3.2 `TransferEngine` Internal Interface
```cpp
class TransferEngine {
public:
    // Called by ftp_upload_dir()
    int32_t upload_directory(
        const char* local_root,
        const char* remote_root,
        const ftp_upload_options_t* options,
        ftp_progress_cb_t progress_cb,
        void* progress_user_data,
        ftp_result_t* out_result
    );

    // Called by worker threads
    void worker_loop();

private:
    ThreadPool pool_;
    std::atomic<uint64_t> bytes_transferred_{0};
    std::atomic<uint64_t> files_success_{0};
    std::atomic<uint64_t> files_failed_{0};
    std::vector<<PerFileResult> file_results_; // guarded by mutex
};
```

---

## 4. Thread Pool & Concurrency Model

### 4.1 Thread Pool Specification
```cpp
class ThreadPool {
public:
    explicit ThreadPool(uint32_t num_workers);
    ~ThreadPool(); // Signals stop, joins all threads
    
    void enqueue(Task task); // Thread-safe, non-blocking
    void wait_for_all();     // Blocks until queue empty + all tasks done
    
private:
    std::vector<std::thread> workers_;
    std::deque<Task> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
};
```

**Worker Count Determination:**
```cpp
uint32_t worker_count = options->max_parallel;
if (worker_count == 0) worker_count = std::thread::hardware_concurrency();
worker_count = std::clamp(worker_count, 1u, 16u);
```

### 4.2 Task Definition
```cpp
struct Task {
    enum Type { MKDIR, UPLOAD_FILE } type;
    
    // For UPLOAD_FILE
    std::string local_path;
    std::string remote_path;
    uint64_t file_size;
    
    // For MKDIR
    std::string remote_dir;
    
    // Completion tracking
    std::atomic<int32_t>* completion_counter; // decremented on finish
};
```

### 4.3 Synchronization: Counting Latch
`upload_directory()` blocks the calling thread until all tasks complete. Synchronization uses a **counting semaphore** (or `std::condition_variable` + atomic counter):

```cpp
std::atomic<uint32_t> pending_tasks{0};
// enqueue N tasks, pending_tasks += N
// each worker decrements on completion
// caller waits on CV until pending_tasks == 0
```

**Critical Rule:** The calling thread (which may hold the Python GIL if called from Python) **must release any locks before entering `wait_for_all()`**. The Phase 6 binding spec will mandate `Py_BEGIN_ALLOW_THREADS` before calling `ftp_upload_dir()`.

---

## 5. Directory Upload Orchestration

### 5.1 Algorithm Overview
```
1. TRAVERSE (Phase 2 DirectoryWalker)
   Produce FileManifest: vector of (local_path, remote_path, size, is_dir)

2. SORT & PARTITION
   Separate manifest into:
   - dirs: all is_dir==true entries (pre-order preserved)
   - files: all is_dir==false entries, sorted descending by size

3. BATCH MKDIR (Control Channel)
   For each dir in dirs:
       ProtocolEngine::create_remote_dir(remote_path)
   Failure policy: if MKD fails with 550 (already exists), continue.
   If MKD fails with 4xx/5xx (no permission), abort with FTP_ERR_SERVER_DENIED.

4. CONCURRENT UPLOAD (Thread Pool)
   For each file in files (largest first):
       ThreadPool::enqueue(UPLOAD_FILE task)
   Wait for completion.

5. AGGREGATE RESULTS
   Populate ftp_result_t from atomic counters and per-file log.
```

### 5.2 Directory Pre-creation vs. On-Demand
**Decision:** Pre-create all directories in step 3 before any file upload begins.

**Rationale:**
- Many FTP servers throttle `CWD`/`MKD` commands. Batching them avoids interleaving with data transfer.
- Workers in step 4 can assume remote directory exists, eliminating `MKD` contention on the control channel.
- If a directory creation fails (permissions), we fail fast before transferring any data.

**Exception:** If `create_remote_dirs == 0` in options, skip step 3. If a file upload encounters a missing directory, return `FTP_ERR_SERVER_DENIED` for that file.

### 5.3 File Ordering: Largest-First
Files are sorted by `size` descending before enqueueing. This ensures that the longest-running transfers start first, maximizing the probability that all workers remain busy until the final files complete. This is a standard scheduling optimization in batch transfer tools (e.g., `rsync --delay-updates`, `lftp`).

---

## 6. Data Channel Management (Concurrent)

### 6.1 Per-Transfer Data Channel Lifecycle
Each `UPLOAD_FILE` task executes independently:

```
Worker Thread:
1. Lock control channel mutex (only one worker may use control at a time)
2. Send "TYPE I\r\n" (if not already binary)
3. Send "PASV\r\n" (or EPSV)
4. Parse host/port from response
5. Open PlainTransport or TlsTransport to data host:port
6. Send "STOR remote_path\r\n"
7. Expect 150/125 (opening data connection)
8. Unlock control channel mutex
9. Stream local file to data transport (see Section 7)
10. Close data transport (send EOF)
11. Lock control channel mutex
12. Read 226/250 final response
13. Unlock control channel mutex
14. Report result to ResultAggregator
```

### 6.2 Control Channel Mutex
The control channel is **single-threaded** by protocol design. A `std::mutex` (the `control_mutex_`) protects all control channel reads/writes. Workers acquire it briefly for command dispatch and final response reading.

**Performance Impact:** Minimal. Control commands are tiny text. The bottleneck is the data channel, not the control channel. With 4 workers, control channel mutex contention is <5% of total runtime.

### 6.3 Control Channel Pipelining (Limited)
While strict RFC 959 requires lock-step, a limited optimization is permitted:

```
Safe to pipeline:
- "MKD dir1\r\nMKD dir2\r\n" (if we don't need intermediate responses for ordering)
- "CWD /foo\r\nTYPE I\r\n" (stateless ordering)

NOT safe to pipeline:
- PASV + STOR (must know data port before STOR)
- STOR + anything (must wait for 226)
```

Phase 4 implements **no pipelining** for simplicity. Phase 7 may revisit. The performance gain from pipelining MKD commands is marginal compared to the complexity of tracking multi-reply state.

### 6.4 Server Connection Limits & Backpressure
If a worker receives `421 Too many connections` during PASV or STOR:

1. **Immediate Action:** Pause the task (do not fail).
2. **Backoff:** Worker sleeps for `100ms * (1 + random() % 5)`.
3. **Retry:** Re-acquire control channel, re-PASV, re-attempt STOR.
4. **Max Retries:** 3 attempts within the worker. If still failing, mark file as failed with `FTP_ERR_SERVER_DENIED`.
5. **Global Throttle:** If >25% of workers hit 421 simultaneously, reduce `max_parallel` by 1 for the remainder of the batch. This is a dynamic adaptation not exposed in the API.

---

## 7. Buffer & I/O Strategy

### 7.1 Buffer Pool
```cpp
class BufferPool {
public:
    static constexpr uint32_t BUFFER_SIZE = 256 * 1024; // 256KB
    
    char* acquire();  // Returns pointer to BUFFER_SIZE bytes
    void release(char* buf); // Returns to pool
};
```

**Behavior:**
- Pool pre-allocates `max_parallel * 2` buffers at `upload_directory()` start.
- Each worker acquires one buffer at task start, releases at task end.
- If pool exhausted (should not happen with sizing), fallback to `new char[BUFFER_SIZE]`.

### 7.2 File Read → Socket Write Loop
```cpp
// Worker task pseudocode
char* buf = buffer_pool_.acquire();
std::ifstream file(local_path, std::ios::binary);
while (file.read(buf, BUFFER_SIZE) || file.gcount() > 0) {
    std::size_t bytes_read = file.gcount();
    std::size_t bytes_sent = 0;
    while (bytes_sent < bytes_read) {
        int32_t sent = data_transport->write(buf + bytes_sent, bytes_read - bytes_sent);
        if (sent < 0) { /* handle error */ break; }
        bytes_sent += sent;
    }
}
buffer_pool_.release(buf);
```

**Platform Notes:**
- `std::ifstream` is acceptable for Phase 4. Phase 7 will evaluate `mmap()` or platform `sendfile()`.
- File opened in binary mode unconditionally (`std::ios::binary`).
- No text mode translation. FTP `TYPE I` (binary) is the only mode supported for upload.

### 7.3 Resume Support (REST)
If `resume_enabled == 1` in options:

```
1. Before STOR, send "SIZE remote_path\r\n"
2. If 213 <size> returned, compare to local file size.
3. If remote < local: send "REST <remote_size>\r\n", then "STOR remote_path\r\n"
4. Seek local file to remote_size.
5. Upload remainder.
6. If remote == local: skip file (return FTP_WARN_SKIPPED).
7. If SIZE fails (550 not found): treat as new file, upload full.
```

**REST Command Availability:** Not all servers support `SIZE` or `REST` with `STOR`. If server returns `500/502` to `SIZE`, disable resume for this session and upload full file. Do not fail the batch.

---

## 8. Progress Callback Subsystem

### 8.1 Invocation Semantics
The `ftp_progress_cb_t` registered in `ftp_upload_dir()` is invoked from **worker threads**. The C++ implementation must guarantee:

- **Frequency Cap:** Maximum 10 invocations per second **per file**. A simple time-gate per task:
  ```cpp
  auto now = std::chrono::steady_clock::now();
  if (now - last_progress_time_ < 100ms) return; // throttle
  ```
- **Final Invocation:** Always invoked on task completion (success or failure) with `bytes_current == bytes_total` or `bytes_current` at failure point.
- **Thread Safety:** Multiple workers may invoke the callback concurrently. The callback itself must be reentrant (documented in Phase 1 header).
- **No Blocking:** If callback takes >1ms, log a warning but do not abort the transfer.

### 8.2 GIL Interaction (Python Binding Precursor)
The C++ library is GIL-agnostic. However, the Phase 6 Python binding specification will require:

```python
# Python side (Phase 6 binding)
def _progress_trampoline(local, remote, current, total, bps, user_data):
    # This C function is registered as the ftp_progress_cb_t
    # It acquires GIL before calling Python callable
    with gil_held():
        py_callable(local.decode(), remote.decode(), current, total, bps)
```

The C++ side simply calls the function pointer. GIL management is entirely the binding's responsibility.

### 8.3 Progress Data Accuracy
- `bytes_total` is the file size from the manifest (pre-traversal).
- `bytes_current` is the cumulative bytes successfully written to the data transport.
- `bytes_per_second` is a smoothed moving average over the last 1 second of the specific file's transfer.

---

## 9. Result Aggregation & Partial Failure

### 9.1 `ftp_result_t` Population (Phase 1 Amendment)
Phase 4 requires extending the result structure. Since Phase 1 ABI is frozen, we use the **extension mechanism** defined in Phase 1 Section 10.2:

```c
/* New for Phase 4 - per-file detail vector */
typedef struct {
    const char* local_path;
    const char* remote_path;
    int32_t     status;        /* FTP_OK or error code */
    uint64_t    bytes_sent;
} ftp_file_result_t;

/* Extended result with detail pointer */
typedef struct {
    uint32_t    struct_size;        /* sizeof(ftp_result_t) */
    int32_t     status;
    uint64_t    files_total;
    uint64_t    files_success;
    uint64_t    files_failed;
    uint64_t    bytes_transferred;
    
    /* Phase 4 extension fields (only valid if struct_size >= sizeof(full struct)) */
    uint64_t            file_result_count;
    ftp_file_result_t*  file_results; /* Array allocated by C++, freed by ftp_result_free() */
} ftp_result_t;
```

### 9.2 Memory Ownership Amendment
A new ABI function is required for Phase 4:

```c
/* Free result resources allocated by ftp_upload_dir(). */
int32_t ftp_result_free(ftp_result_t* result);
```

**Rules:**
- Caller must call `ftp_result_free()` on any `ftp_result_t` passed to `ftp_upload_dir()`.
- `ftp_result_free()` zeroes all strings and internal arrays before deallocation.
- Safe to call with `file_results == NULL` (no-op for that field).

### 9.3 Partial Failure Policy
The default policy is **continue-on-failure**:

- If a single file upload fails (network error, permission denied, disk full), remaining files continue.
- `ftp_result_t.status` reflects the **worst** error encountered:
  - If any file failed with a permanent error → `FTP_ERR_SERVER_DENIED` or appropriate code.
  - If all succeeded → `FTP_OK`.
  - If some failed but others succeeded with retry → `FTP_WARN_PARTIAL_RETRY` (Phase 5).
- The caller inspects `file_results` to determine individual file fates.

**Abort-on-first-failure option:** Future Phase 7 may add `ftp_upload_options_t.abort_on_error`. Phase 4 always continues.

---

## 10. Integration with Phase 1-3

### 10.1 `ftp_upload_dir()` Implementation
The Phase 1 stub is replaced with the full orchestration:

```cpp
int32_t ftp_upload_dir(...) {
    // 1. Validate handle state (ALLOCATED or AUTHENTICATED)
    if (handle->state != AUTHENTICATED) return FTP_ERR_INVALID_STATE;
    
    // 2. Validate paths non-NULL
    if (!local_path || !remote_path) return FTP_ERR_INVALID_ARGUMENT;
    
    // 3. Resolve options (NULL = defaults)
    UploadOptions resolved = options ? *options : UploadOptions{};
    
    // 4. Traverse local directory (Phase 2 DirectoryWalker)
    FileManifest manifest = handle->engine->traverse(local_path, resolved);
    
    // 5. Pre-create remote directories (Phase 2 ProtocolEngine)
    for (auto& dir : manifest.dirs) {
        handle->engine->mkdir(dir.remote_path);
    }
    
    // 6. Initialize TransferEngine with thread pool
    TransferEngine xfer(handle->engine, resolved.max_parallel);
    
    // 7. Enqueue file tasks (largest first)
    for (auto& file : manifest.files_sorted_descending()) {
        xfer.enqueue_upload(file, progress_cb, user_data);
    }
    
    // 8. Block until completion
    xfer.wait_for_all();
    
    // 9. Populate out_result
    xfer.fill_result(out_result);
    
    return FTP_OK; // or worst-error code
}
```

### 10.2 State Machine Interaction
`ftp_upload_dir()` requires handle state `AUTHENTICATED`. During upload:

- State remains `AUTHENTICATED` (individual data channel states are internal to workers).
- If control channel encounters fatal error (TCP RST, 421 global shutdown), all workers are signaled to cancel pending tasks.
- Cancelation: Workers check an `std::atomic<bool> cancel_flag_` between buffer writes. If set, abort current file with `FTP_ERR_NETWORK_RESET` and stop accepting new tasks.

### 10.3 TLS Interaction
Data channels opened by workers use the same TLS configuration as the control channel:

- If control is `TlsTransport`, workers request `PROT P` (or `PROT C` if server rejected P) and perform TLS handshake on each data connection.
- Session resumption (Phase 3) is attempted for each data channel to reduce handshake overhead.

---

## 11. Quality Gates & Acceptance Criteria

### 11.1 Concurrency Correctness
**Setup:** Upload directory with 1,000 files (mix of 1KB, 1MB, 100MB) to localhost vsftpd.  
**Configurations:** `max_parallel = 1, 2, 4, 8, 16`.  
**Pass Criteria:**
- All files arrive intact (MD5 checksum match, verified by post-upload `XMD5` or local re-download).
- No race conditions detected by ThreadSanitizer (`-fsanitize=thread`).
- No deadlocks: test completes within 2× the single-threaded duration.

### 11.2 Bandwidth Saturation
**Setup:** Upload 10GB directory over 1Gbps LAN to ProFTPD.  
**Metric:** Compare `max_parallel=1` vs `max_parallel=4` throughput.  
**Pass Criteria:** `max_parallel=4` achieves ≥ 3.2× the throughput of single-threaded. Demonstrates that parallelism effectively saturates the link.

### 11.3 Server-Friendliness
**Setup:** Upload to server with `MaxClientsPerHost 2` (vsftpd config).  
**Configuration:** `max_parallel=8`.  
**Pass Criteria:**
- Library detects 421 errors, backs off, and completes without crashing the server.
- At least 75% of files succeed despite the restrictive server limit.
- No SYN flood or connection storm detected via `tcpdump`.

### 11.4 Progress Callback Accuracy
**Setup:** Upload 100MB file with progress callback logging every invocation.  
**Pass Criteria:**
- Callback invoked at start, at least once during transfer, and exactly once at end.
- Interval between invocations ≥ 100ms (10Hz cap).
- `bytes_per_second` value is within ±20% of actual throughput measured externally.

### 11.5 Partial Failure Handling
**Setup:** Upload tree where 50% of files target a read-only remote directory, 50% to writable directory.  
**Pass Criteria:**
- Writable files succeed; read-only files fail with `FTP_ERR_SERVER_DENIED`.
- `ftp_result_t.files_success == 50%`, `files_failed == 50%`.
- `file_results` array contains correct per-file status.
- `ftp_result_free()` successfully deallocates without leaks (Valgrind).

### 11.6 Resume Integrity
**Setup:** Upload 100MB file, interrupt at 50MB (kill -9 worker), restart with `resume_enabled=1`.  
**Pass Criteria:**
- Final remote file size == 100MB.
- MD5 checksum matches local file.
- Second upload completes in ~55% of original time (accounting for REST overhead).

---

## 12. Deliverables

| File | Purpose |
|------|---------|
| `src/transfer/ThreadPool.hpp/cpp` | Fixed worker pool with task queue |
| `src/transfer/TransferEngine.hpp/cpp` | Upload orchestration and result aggregation |
| `src/transfer/BufferPool.hpp/cpp` | Reusable 256KB buffer management |
| `src/transfer/ProgressDispatcher.hpp/cpp` | Callback throttling and invocation |
| `src/transfer/ResultAggregator.hpp/cpp` | Atomic counters + per-file result vector |
| `src/transfer/Task.hpp` | Task struct definitions (MKDIR, UPLOAD_FILE) |
| `src/transfer/UploadWorker.hpp/cpp` | Per-worker file upload logic |
| `include/ftpclient.h` *(amendment)* | Add `ftp_result_free()`, extend `ftp_result_t` |
| `tests/concurrency_correctness.cpp` | ThreadSanitizer + 1,000 file upload test |
| `tests/bandwidth_saturation.cpp` | 10GB LAN throughput benchmark |
| `tests/server_friendliness.cpp` | 421 backoff + restrictive server test |
| `tests/progress_accuracy.cpp` | Callback frequency + bps accuracy |
| `tests/partial_failure.cpp` | Mixed permission upload + result verification |
| `tests/resume_integrity.cpp` | REST + partial re-upload checksum test |

---

## 13. Transition Criteria to Phase 5

Phase 4 is **ratified** when:

1. All 6 Quality Gates (Section 11) pass on CI for Linux x86_64 and Windows x64.
2. ThreadSanitizer reports zero data races during concurrent upload tests.
3. Valgrind/ASan reports clean with zero leaks after `ftp_result_free()`.
4. Bandwidth saturation gate demonstrates ≥3× speedup from `max_parallel=4` on 1Gbps link.
5. The `ftp_upload_dir()` function is no longer a stub: it successfully uploads real directory trees against vsftpd, ProFTPD, and IIS FTP.
6. Progress callback invokes correctly from C and from Python `cffi` test script (Phase 6 precursor).
7. **No new exported symbols** beyond `ftp_result_free()` and Phase 1's existing surface. Internal C++ classes remain hidden.

**Upon ratification:** The transfer engine is ready for Phase 5 (Resilience & Fault Recovery), which will add retry policies, circuit breakers, and exponential backoff without altering the upload orchestration structure.

---

**End of Phase 4 Specification**
