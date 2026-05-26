# Phase 5 Specification: Resilience & Fault Recovery

**Document Version:** 5.0-DRAFT  
**Depends On:** Phase 1 (Frozen ABI), Phase 2 (Protocol Engine), Phase 3 (Security/TLS), Phase 4 (Transfer Engine)  
**Scope:** Retry policies, circuit breakers, timeout hierarchies, stall detection, idempotency classification, and autonomous recovery for unattended batch operations.

---

## 1. Purpose & Scope

Phase 5 transforms the high-throughput engine from Phase 4 into a **self-healing system** capable of unattended operation over unreliable networks. It does not add new user-visible transfer features; instead, it injects resilience **below** the existing `TransferEngine` API so that callers receive fewer hard failures and more `FTP_WARN_PARTIAL_RETRY` outcomes.

**In Scope:**
- Per-file retry with configurable exponential backoff + full jitter
- Per-host circuit breaker to prevent hammering dead/unhealthy servers
- Hierarchical timeout architecture: connect, command, idle, data-transfer-with-stall-detection
- Dynamic data timeout based on observed bandwidth (adaptive stall detection)
- Idempotency classification for FTP commands to determine safe retry vs. fatal fail-fast
- Integration with Phase 4's `ResultAggregator` to distinguish "succeeded after retry" from "succeeded first attempt"
- Server-friendly backoff: temporary reduction of `max_parallel` after `421 Too many connections` bursts
- Graceful degradation: TLS → plain fallback only when explicitly permitted (security-critical)

**Explicitly Out of Scope:**
- Bandwidth throttling / rate limiting (Phase 7)
- Delta sync / file comparison logic (Phase 7)
- Checkpoint persistence to disk (library assumes caller restarts the batch)
- Load balancing across multiple FTP servers (out of scope for this library)
- Application-level transaction semantics (e.g., atomic multi-file commit) — FTP protocol limitation

---

## 2. Architectural Decisions & Trade-offs

| Decision | Selected Approach | Rejected Alternative | Rationale |
|----------|------------------|---------------------|-----------|
| **Retry Granularity** | Per-file, per-operation (STOR, PASV, connect) | Per-batch restart | Per-file retry minimizes re-transferred bytes. A single 100GB file failing should not restart a 10,000-file batch. |
| **Backoff Algorithm** | Exponential backoff with **full jitter** | Equal intervals, truncated exponential | Full jitter (`random(0, base*2^attempt)`) is proven optimal for thundering-herd avoidance in distributed systems (AWS paper). |
| **Circuit Breaker State** | Per-host in-memory (host:port key) | Global process-wide | A single bad server should not block connections to healthy servers in the same process. |
| **Timeout Model** | Deadline timers per operation, not wall-clock | Single global timeout for entire upload | A 10,000-file batch cannot have a single timeout. Each operation (connect, PASV, STOR block) carries its own deadline. |
| **Stall Detection** | Adaptive: no progress for N seconds, where N scales with file size and observed BPS | Fixed 60s for all files | A 10GB file at 1Mbps legitimately takes 80s per 10MB buffer. Fixed stall detection is either too aggressive (false positives) or too lax (wasted time). |
| **Idempotency** | Static command table (safe / unsafe / conditional) | Dynamic server-side detection | FTP servers do not advertise idempotency. We use a conservative static table based on RFC semantics. |
| **Parallel Throttle** | Temporary `max_parallel` reduction on 421, auto-restore next batch | Permanent reduction until process restart | Server load may be transient. Auto-restore allows recovery without user intervention. |

---

## 3. Resilience Component Architecture

### 3.1 Component Hierarchy
```
FtpClientImpl
  ├── ProtocolEngine (Phase 2)
  ├── CredentialVault (Phase 3)
  ├── TransferEngine (Phase 4)
  └── ResilienceController (Phase 5: NEW)
        ├── RetryPolicy (per-file configuration)
        ├── CircuitBreaker (per-host state)
        ├── DeadlineTimer (operation timeouts)
        ├── StallDetector (data flow watchdog)
        └── IdempotencyClassifier (command safety table)
```

**Integration Rule:** `ResilienceController` is a **wrapper layer**. It does not replace `TransferEngine` or `ProtocolEngine`. A worker thread in Phase 4 calls `ResilienceController::execute_with_retry(task)` instead of calling `ProtocolEngine` directly.

---

## 4. Retry Policy Engine

### 4.1 Configuration (from `ftp_upload_options_t` + Phase 5 extensions)
```cpp
struct RetryConfig {
    uint32_t max_attempts = 3;           /* 1 = initial only, no retry */
    uint64_t base_delay_ms = 1000;       /* Exponential backoff base */
    uint64_t max_delay_ms = 30000;       /* Cap backoff at 30s */
    double   jitter_factor = 1.0;      /* 1.0 = full jitter, 0.0 = no jitter */
    int32_t  retry_all_errors = 0;       /* 0 = retry only transient, 1 = retry all */
};
```

**ABI Amendment:** Phase 5 adds `ftp_set_retry_policy()` to override defaults per-handle, preserving backward compatibility with callers who rely on `ftp_upload_options_t` defaults.

```c
int32_t ftp_set_retry_policy(
    ftp_client_t* handle,
    uint32_t max_attempts,
    uint64_t base_delay_ms,
    uint64_t max_delay_ms
);
```

### 4.2 Exponential Backoff with Full Jitter
```
delay(attempt) = random_uniform(0, min(base * 2^attempt, max_delay))
```

Where `attempt` is 0-indexed (0 = first retry after initial failure).

**Example with base=1s, max=30s:**
| Attempt | Range (ms) | Typical Draw |
|---------|-----------|--------------|
| 0 (1st failure) | 0 – 1,000 | 734ms |
| 1 (2nd failure) | 0 – 2,000 | 1,421ms |
| 2 (3rd failure) | 0 – 4,000 | 3,892ms |
| 3 (4th failure) | 0 – 8,000 | — (if max_attempts=3, stop) |
| 5+ | 0 – 30,000 (capped) | — |

**Implementation:** Use `std::mt19937_64` seeded from `std::random_device` + `std::chrono` steady clock. Do **not** use `rand()`.

### 4.3 Retryable vs. Non-Retryable Errors
The Phase 1 error taxonomy is partitioned:

| Category | Retry? | Examples |
|----------|--------|----------|
| **Transient Network** | ✅ Yes | `FTP_ERR_NETWORK_RESET`, `FTP_ERR_TIMEOUT`, `FTP_ERR_CONNECT`, `FTP_ERR_PASSIVE_FAILED` |
| **Transient Server** | ✅ Yes | `FTP_ERR_SERVER_DENIED` (550) when server is temporarily full, `421 Too many connections` |
| **Permanent Auth** | ❌ No | `FTP_ERR_AUTH_FAILED`, `FTP_ERR_CERT_VERIFY` |
| **Permanent Protocol** | ❌ No | `FTP_ERR_PROTOCOL` (server violates RFC; retry will not fix) |
| **Permanent Local** | ❌ No | `FTP_ERR_LOCAL_IO` (disk error, permission denied) |
| **Ambiguous** | ⚠️ Conditional | `FTP_ERR_REMOTE_IO` (451) — retry once, then fail |

**Override:** If `retry_all_errors == 1`, even permanent errors are retried up to `max_attempts`. This is a "Hail Mary" mode for flaky scripting, not recommended for production.

---

## 5. Circuit Breaker

### 5.1 Design
Per-host (host + port as string key) state machine with three states:

| State | Behavior | Entry Trigger |
|-------|----------|-------------|
| `CLOSED` | Normal operation. All requests pass. | Initial state, or after `HALF_OPEN` success |
| `OPEN` | Fast-fail all requests for this host. | Failure rate ≥ threshold within time window |
| `HALF_OPEN` | Allow **one** probe request. If success → `CLOSED`; if fail → `OPEN`. | `OPEN` timeout expired (e.g., 60s) |

### 5.2 Parameters
```cpp
struct CircuitBreakerConfig {
    uint32_t failure_threshold = 5;      /* Failures to trip OPEN */
    uint64_t window_ms = 60000;          /* Rolling window for failure count */
    uint64_t open_duration_ms = 60000;   /* Time in OPEN before HALF_OPEN */
    uint32_t half_open_max = 1;          /* Parallel probes allowed in HALF_OPEN */
};
```

### 5.3 Failure Counting
- A "failure" is any `ftp_connect()` or `ftp_upload_dir()` call that returns a non-retryable error, or exhausts all retry attempts.
- `421 Too many connections` counts as **0.5 failures** (weighted) because it indicates server stress, not death.
- Successes reset the failure count for that host to zero.

### 5.4 Interaction with Thread Pool
If a worker encounters an `OPEN` circuit breaker for its host:
1. The task immediately fails with `FTP_ERR_NETWORK_RESET` (mapped to "circuit open").
2. The worker does **not** sleep or spin; it moves to the next task immediately.
3. The `ResultAggregator` records the failure reason as `circuit_breaker_open`.

### 5.5 Thread Safety
Circuit breaker state is protected by a `std::shared_mutex` (read-heavy: many workers check state, few updates). Host entries are stored in a static `std::unordered_map<std::string, HostState>` with lazy insertion.

---

## 6. Timeout Hierarchy & Stall Detection

### 6.1 Timeout Taxonomy
| Timeout | Default | Scope | Trigger |
|---------|---------|-------|---------|
| **Connect** | 5s | Per TCP connection | `connect()` syscall |
| **Command** | 30s | Per control channel request/response | Time between sending `VERB` and receiving final reply code |
| **Idle** | 30s | Control channel inactivity | No commands enqueued; triggers internal NOOP |
| **Data Stall** | Adaptive | Per data channel block | No bytes written to socket for N seconds |

### 6.2 Adaptive Data Stall Detection
The Phase 4 fixed buffer loop is enhanced with a stall watchdog:

```cpp
class StallDetector {
public:
    void start(uint64_t file_size_bytes);
    void report_progress(uint64_t bytes_sent_since_last_check);
    bool is_stalled();  // Called every buffer write completion
    
private:
    uint64_t file_size_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_progress_time_;
    double observed_bps_ = 0.0;  // EMA of throughput
    static constexpr double MINIMUM_BPS = 1024.0; // 1KB/s floor
};
```

**Algorithm:**
```
1. On first buffer write: record start_time, last_progress_time.
2. After each buffer: 
   elapsed = now - last_progress_time
   bytes_this_interval = current_total - previous_total
   instant_bps = bytes_this_interval / elapsed_seconds
   observed_bps = (0.7 * observed_bps) + (0.3 * instant_bps)  // EMA
3. Expected time for next buffer = buffer_size / max(observed_bps, MINIMUM_BPS)
4. Stall threshold = max(expected_time * 3, 5.0) seconds  // 3× expected, minimum 5s
5. If (now - last_progress_time) > stall_threshold: return STALLED
```

**Rationale:** A 1GB file at 10MB/s should never trigger stall. A 1KB file at 0 bytes/s for 10s should trigger. The 3× multiplier accommodates normal TCP variance.

### 6.3 Timeout Implementation
Use `SO_RCVTIMEO` / `SO_SNDTIMEO` on POSIX, or `setsockopt` with timeout structs. On Windows, use `WSARecv` with timeout or `setsockopt`. For control channel reads, use `select()`/`poll()` with deadline calculation rather than relying solely on socket timeouts (which may not cover the entire command-response cycle).

**TLS Consideration:** OpenSSL `SSL_read()` with `BIO_set_nbio` + `select()` is the robust pattern. Blocking `SSL_read()` with socket timeouts works for simple cases but can misreport SSL renegotiation delays as stalls. Phase 5 mandates the `select()` approach for the control channel.

---

## 7. Idempotency & Command Safety

### 7.1 Static Safety Table
Not all FTP commands are safe to retry. Phase 5 classifies every command used by the library:

| Command | Idempotency | Retry Rule | Rationale |
|---------|-------------|------------|-----------|
| `USER` | ✅ Safe | Retry | No server state change on re-send |
| `PASS` | ✅ Safe | Retry | Same credentials, same outcome |
| `CWD` | ✅ Safe | Retry | Idempotent directory change |
| `PWD` | ✅ Safe | Retry | Pure read |
| `TYPE I` | ✅ Safe | Retry | Idempotent mode set |
| `PASV` / `EPSV` | ✅ Safe | Retry | Allocates new ephemeral port each time |
| `PBSZ` / `PROT` | ✅ Safe | Retry | Idempotent security parameter |
| `NOOP` | ✅ Safe | Retry | No-op by definition |
| `SIZE` | ✅ Safe | Retry | Pure read |
| `REST` | ✅ Safe | Retry | Sets offset; re-setting same offset is safe |
| `MKD` | ⚠️ Conditional | Retry if 550 (already exists) | Creating existing dir may fail or succeed |
| `STOR` | ❌ Unsafe | **Never retry full file** | Overwrites remote file; retry must use `REST` resume |
| `DELE` | ❌ Unsafe | Never retry | Deleting twice is error; first may have succeeded |
| `RNFR` / `RNTO` | ❌ Unsafe | Never retry | Rename is stateful; retry may rename wrong file |
| `QUIT` | ✅ Safe | Retry (harmless) | Connection close |

### 7.2 STOR Retry Semantics
`STOR` is the critical non-idempotent command. Phase 5 mandates:

- **First attempt:** Send `STOR remote_path`. If fails before data flows (e.g., 425, 530), safe to retry from scratch.
- **Mid-transfer failure:** If data channel drops after bytes were sent, retry **must** use `REST <bytes_already_confirmed>` followed by `STOR`. The "confirmed" byte count is the amount acknowledged by the server's 226 (which we may not have received). Conservatively, use `REST 0` and re-upload full file unless Phase 4 resume tracking is active.
- **Post-complete failure:** If 226 is lost in transit but file actually succeeded, a naive retry overwrites it. Phase 5 does not solve this ambiguity (FTP protocol limitation). It retries with `REST` if resume is enabled, otherwise re-uploads.

---

## 8. Integration with Phase 4 Transfer Engine

### 8.1 Worker Thread Modification
The Phase 4 `UploadWorker` loop is wrapped:

```cpp
// Phase 4 (original):
// protocol_engine->upload_file(entry, data_transport);

// Phase 5 (wrapped):
ResilienceController::execute_with_retry(
    task,
    [&]() { return protocol_engine->upload_file(entry, data_transport); },
    retry_config,
    circuit_breaker_
);
```

### 8.2 Result Flag Enhancement
Phase 4's `ftp_file_result_t` gains a `attempt_count` field:

```c
typedef struct {
    /* ... existing fields ... */
    uint32_t    attempt_count;    /* 1 = first try succeeded, >1 = retried */
    int32_t     final_error;      /* Error code from last attempt (if failed) */
} ftp_file_result_t;
```

**ABI Rule:** Since Phase 1 structs use `struct_size`, adding fields at the end is safe for backward compatibility. Older callers with smaller `struct_size` will not see these fields; the C++ implementation checks `struct_size` before writing.

### 8.3 Progress Callback During Retry
When a retry occurs:
1. Progress callback receives a final invocation for the failed attempt (with `bytes_current` at failure point).
2. A new progress tracking session begins for the retry attempt.
3. The caller sees two separate progress sequences for the same file. This is correct behavior — the caller can sum `bytes_transferred` across attempts if desired.

---

## 9. Quality Gates & Acceptance Criteria

### 9.1 Retry Exhaustion Test
**Setup:** Upload 100 files to a server that drops every 3rd data connection with TCP RST. `max_attempts=3`.  
**Pass Criteria:** ≥95% of files succeed (some may fail if hit 3 times). Average `attempt_count` across successful files is between 1.3 and 1.8. No infinite loops.

### 9.2 Backoff Jitter Distribution
**Setup:** Simulate 10,000 retry delays with `base=1s, max=30s`.  
**Pass Criteria:** Kolmogorov-Smirnov test confirms uniform distribution within each attempt bucket. No clustering at bucket boundaries (proves jitter is working).

### 9.3 Circuit Breaker Trip & Recovery
**Setup:** Attempt connection to non-existent server IP. `failure_threshold=3`, `open_duration_ms=5000`.  
**Pass Criteria:**
- Attempts 1–3: Actual TCP connection attempts (observed via `tcpdump` or timing).
- Attempts 4–10: Immediate return (`FTP_ERR_NETWORK_RESET`) with no TCP packets emitted.
- After 5s: Attempt 11 performs actual TCP probe (HALF_OPEN).
- Attempt 11 success → subsequent requests normal.

### 9.4 Stall Detection Accuracy
**Setup:** Upload 1GB file over a link throttled to 1MB/s with 10s complete dropouts.  
**Pass Criteria:**
- No false positives during normal 1MB/s flow.
- Dropout detected within 15s of zero throughput.
- Upload resumes via `REST` if `resume_enabled=1`.

### 9.5 Server Stress Protection
**Setup:** vsftpd with `max_per_ip=2`. Upload 500 files with `max_parallel=8`.  
**Pass Criteria:**
- No vsftpd process crash or denial of service.
- Library receives `421` responses, backs off, and completes with ≥90% success.
- `max_parallel` temporarily reduced during the batch (observable in logs).

### 9.6 Memory Safety Under Retry Pressure
**Setup:** Upload 1,000 files with `max_attempts=5` to a server that alternates between accepting and rejecting connections every 2 seconds.  
**Pass Criteria:** Valgrind/ASan clean after 30 minutes of continuous operation. No handle leaks, no retry timer leaks.

---

## 10. Deliverables

| File | Purpose |
|------|---------|
| `src/resilience/RetryPolicy.hpp/cpp` | Exponential backoff, jitter, attempt counting |
| `src/resilience/CircuitBreaker.hpp/cpp` | Per-host state machine, failure tracking |
| `src/resilience/DeadlineTimer.hpp/cpp` | Cross-platform socket deadline management |
| `src/resilience/StallDetector.hpp/cpp` | Adaptive throughput-based stall detection |
| `src/resilience/IdempotencyClassifier.hpp/cpp` | Command safety table and STOR retry logic |
| `src/resilience/ResilienceController.hpp/cpp` | Facade wrapping ProtocolEngine calls |
| `include/ftpclient.h` *(amendment)* | Add `ftp_set_retry_policy()`, extend `ftp_file_result_t` |
| `tests/retry_exhaustion.cpp` | TCP RST injection + retry success rate |
| `tests/backoff_distribution.cpp` | Statistical jitter validation |
| `tests/circuit_breaker_trip.cpp` | Trip, fast-fail, and HALF_OPEN recovery |
| `tests/stall_detection.cpp` | Throttled link with injected dropouts |
| `tests/server_stress_protection.cpp` | vsftpd `max_per_ip` compliance |
| `tests/resilience_memory_safety.cpp` | Long-running alternating server stress |

---

## 11. Transition Criteria to Phase 6

Phase 5 is **ratified** when:

1. All 6 Quality Gates (Section 9) pass on CI for Linux x86_64 and Windows x64.
2. No regressions in Phase 4 bandwidth saturation gate: `max_parallel=4` still achieves ≥3× single-threaded throughput.
3. The `ftp_upload_dir()` function handles a 10,000-file upload against a server that drops 30% of connections, completing with ≥85% success rate and no manual intervention.
4. ThreadSanitizer reports zero races in `CircuitBreaker` shared state and `RetryPolicy` timer state.
5. Valgrind/ASan reports clean after the 30-minute resilience stress test.
6. **ABI backward compatibility verified:** A binary compiled against Phase 1 `ftpclient.h` loads the Phase 5 shared library and successfully calls `ftp_upload_dir()` without recompilation.

**Upon ratification:** The library core is functionally complete for batch upload operations. Phase 6 (Python Binding) will add ergonomic wrappers without modifying C++ internals.

---

**End of Phase 5 Specification**
