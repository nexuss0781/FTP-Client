# Phase 7 Specification: Optimization & Observability

**Document Version:** 7.0-DRAFT  
**Depends On:** Phase 1 (Frozen ABI), Phases 2–6 (Complete Core + Python Binding)  
**Scope:** Platform zero-copy I/O, bandwidth governance, compression, telemetry hooks, final benchmarking, and production hardening. This is the **final engineering phase** before the library enters maintenance mode.

---

## 1. Purpose & Scope

Phase 7 extracts the remaining performance headroom from the architecture and exposes operational visibility for production deployments. It does not alter the public C ABI or Python API signatures; it only adds **optional** configuration paths and **transparent** runtime optimizations.

**In Scope:**
- Platform zero-copy: `sendfile()` (Linux), `TransmitFile` (Windows), `copyfile` (macOS)
- Memory-mapped I/O fallback for zero-copy on all platforms
- Linux io_uring evaluation for control channel (experimental, opt-in)
- Bandwidth throttling: token-bucket rate limiter per-transfer and global
- MODE Z on-the-fly deflate compression (if server advertises support)
- Telemetry/observability hooks: structured event callbacks compatible with OpenTelemetry
- Connection warm-up / keep-alive pooling across `upload_directory()` calls
- Final performance benchmark suite and comparative analysis against `lftp`, `ncftp`, and Python `ftplib`
- Production hardening: `mallopt`/`jemalloc` tuning hints, `RLIMIT_NOFILE` advisory, systemd service integration notes

**Explicitly Out of Scope:**
- New transfer protocols (SFTP, S3, WebDAV) — these are separate products
- GUI bindings (Qt, GTK) — out of scope for this library
- Database-backed checkpoint persistence — caller responsibility
- FIPS 140-2 validated mode (requires OpenSSL FIPS provider + certification process; document as known gap)

---

## 2. Architectural Decisions & Trade-offs

| Decision | Selected Approach | Rejected Alternative | Rationale |
|----------|------------------|---------------------|-----------|
| **Zero-Copy Path** | `sendfile()` on Linux; `TransmitFile` on Windows; `mmap`+`write` universal fallback | `splice()` (Linux pipe-to-socket) | `sendfile()` is the most mature and widely supported zero-copy path. `splice()` requires pipe management and offers marginal additional gain for FTP (file → socket is exactly what `sendfile()` does). |
| **io_uring** | Experimental opt-in via `ftp_set_option()`; default remains blocking I/O | Replace blocking I/O entirely | io_uring requires Linux 5.1+ and careful CQ/EQ management. The complexity is not justified for the control channel. As an experimental data-channel option, it provides a future path without destabilizing the proven architecture. |
| **Rate Limiting** | Token bucket per data channel + optional global cap | Leaky bucket | Token bucket allows burst tolerance (useful for directory uploads with mixed file sizes). Leaky bucket is smoother but penalizes legitimate bursts. |
| **Compression** | MODE Z (`MODE Z` + zlib deflate on data channel) | Pre-compress files client-side | MODE Z is wire-level and transparent to the user. Pre-compression would alter remote file contents, breaking checksums and requiring server-side decompression awareness. |
| **Telemetry** | C ABI callback emitting structured JSON-like events | Direct OpenTelemetry C++ SDK dependency | Direct OTel dependency would bloat the library and complicate linking. A C callback allows Python to bridge into `opentelemetry-python` or any other backend without C++ knowing the consumer. |
| **Connection Reuse** | Control channel kept alive across `upload_directory()` calls within one handle lifetime | Connection pool shared across handles | FTP is stateful (CWD, TYPE). Sharing control channels across handles would require session isolation logic that FTP does not support. Per-handle persistence is the correct model. |

---

## 3. Zero-Copy I/O Subsystem

### 3.1 `ZeroCopyTransport` Decorator
A new internal class wrapping `Transport` (from Phase 2/3) with zero-copy send capability:

```cpp
class ZeroCopyTransport : public Transport {
public:
    // Falls back to Transport::write() if zero-copy unavailable for this fd/socket
    int32_t sendfile(int fd, uint64_t offset, uint64_t count) override;
    
private:
    Transport* base_;
    bool sendfile_available_ = false;
};
```

**Rule:** `ZeroCopyTransport` is **not** a new public API. It is selected internally when:
- Platform is Linux and kernel ≥ 2.6.33 (for `sendfile()` with large file support)
- File descriptor is seekable regular file (not pipe, not FIFO)
- TLS is **not** active on the data channel (`sendfile()` is incompatible with TLS because data must pass through the encryption layer)

### 3.2 Linux `sendfile()` Path
```cpp
#include <sys/sendfile.h>

int32_t ZeroCopyTransport::sendfile(int fd, uint64_t offset, uint64_t count) {
    off_t off = static_cast<<off_t>(offset);
    ssize_t sent = ::sendfile(socket_fd_, fd, &off, static_cast<size_t>(count));
    if (sent < 0) {
        if (errno == EINVAL || errno == ENOSYS) {
            // Kernel or filesystem doesn't support sendfile for this combo
            return FALLBACK;
        }
        return -errno;
    }
    return static_cast<int32_t>(sent);
}
```

**Performance Target:** For files > 10MB on loopback, `sendfile()` path must achieve ≥ 95% of raw `cp` throughput, compared to ≤ 85% for the buffered copy path.

### 3.3 Windows `TransmitFile` Path
```cpp
#include <mswsock.h> // WSARecv, WSASend, TransmitFile

// Requires socket created with WSA_FLAG_OVERLAPPED, but we use blocking mode
// TransmitFile works with blocking sockets if hEvent is NULL
BOOL ok = TransmitFile(socket_fd_, file_handle, count, 0, NULL, NULL, TF_USE_KERNEL_APC);
```

**Constraint:** `TransmitFile` requires the socket to have been created with a specific provider. If the `PlainTransport` socket does not satisfy this, the call returns `FALSE` and we fall back to buffered copy.

### 3.4 macOS `copyfile` / `mmap` Path
macOS lacks `sendfile()` for socket-to-socket in the same way Linux does. The fallback is `mmap()` + `write()`:

```cpp
void* mapped = mmap(nullptr, count, PROT_READ, MAP_PRIVATE, fd, offset);
write(socket_fd_, mapped, count);
munmap(mapped, count);
```

This eliminates the user-space buffer copy (kernel copies from page cache to socket buffer directly), though it still incurs a syscall per chunk. It is measurably faster than `std::ifstream::read` + `write`.

### 3.5 TLS Incompatibility & Transparent Fallback
When the data channel is `TlsTransport` (Phase 3), zero-copy is **impossible** because OpenSSL `SSL_write()` requires plaintext in user-space buffers for encryption. The `ZeroCopyTransport` detects this and immediately returns `FALLBACK`, causing the caller to use the Phase 4 `BufferPool` path.

**No user configuration needed.** The library automatically uses zero-copy for plain FTP and falls back to buffered copy for FTPS.

---

## 4. io_uring Experimental Data Channel (Linux 5.1+)

### 4.1 Scope & Limitations
- **Only** for data channel reads/writes, **not** control channel.
- Only activated if `ftp_set_option(handle, FTP_OPT_USE_IOURING, 1)` is called.
- Requires kernel 5.1+ for `IORING_OP_SEND` / `IORING_OP_RECV`.
- Falls back to blocking I/O on kernel version mismatch or `ENOSYS`.

### 4.2 Implementation Sketch
```cpp
#ifdef __linux__
#include <liburing.h>

class IoUringDataChannel {
public:
    IoUringDataChannel(int socket_fd);
    int32_t submit_write(int fd, uint64_t offset, uint32_t length);
    int32_t wait_completion();
    
private:
    io_uring ring_;
    int socket_fd_;
};
#endif
```

**Rationale:** For high-concurrency scenarios (16 parallel uploads), io_uring reduces syscall overhead from 2 per buffer (`read` + `send`) to near-zero (batch submission). Expected 5–15% throughput improvement on NVMe-to-10Gbps links under heavy concurrency.

---

## 5. Bandwidth Throttling (Rate Limiting)

### 5.1 Token Bucket Algorithm
```cpp
class TokenBucket {
public:
    TokenBucket(uint64_t bytes_per_second, uint64_t burst_bytes);
    
    // Blocks (sleeps) until tokens available, then deducts
    void consume(uint64_t bytes);
    
private:
    uint64_t rate_;
    uint64_t burst_;
    uint64_t tokens_;
    std::chrono::steady_clock::time_point last_update_;
    std::mutex mutex_;
};
```

**Configuration (C ABI Extension):**
```c
int32_t ftp_set_rate_limit(
    ftp_client_t* handle,
    uint64_t bytes_per_second,      /* 0 = unlimited */
    uint64_t burst_bytes            /* 0 = default to 1 second of tokens */
);
```

**Behavior:**
- Global cap: If set, the sum of all active data channel throughputs is clamped to `bytes_per_second`.
- Per-file cap: If `bytes_per_second` is small (e.g., 100KB/s), each worker sleeps between buffer writes to maintain the rate.
- Burst tolerance: A 10MB file at 1MB/s limit completes in ~10s, but a 100KB file completes in ~0.1s (burst allowance).

**Integration:** The `TokenBucket` is checked in the `UploadWorker` loop after each `write()`:

```cpp
if (rate_limiter_) {
    rate_limiter_->consume(bytes_just_sent);
}
```

---

## 6. MODE Z Compression

### 6.1 Negotiation
After `AUTH TLS` + `PBSZ` + `PROT P` (or in plain FTP), the library may optionally negotiate:

```
Client: MODE Z\r\n
Server: 200 Mode Z ok\r\n
```

If server returns `500/502`, compression is disabled for the session.

### 6.2 Implementation
- Use `zlib` (linked statically or dynamically depending on platform policy).
- Compress data on-the-fly in the worker thread before `SSL_write()` or `write()`.
- Buffer size: 256KB input → deflate to variable output, sent in chunks.
- **CPU vs. Bandwidth Trade-off:** Compression is only beneficial if:
  - File is compressible (text, logs, JSON, CSV)
  - Network bandwidth is constrained (< 100Mbps) or metered
  - CPU is not the bottleneck

**Auto-Detection:** Phase 7 does **not** implement auto-detection. If `MODE Z` is negotiated, all subsequent transfers use it. Phase 7.1 (future) may add per-file compressibility heuristics.

---

## 7. Observability & Telemetry Hooks

### 7.1 Event Callback Interface (C ABI)
```c
typedef enum {
    FTP_EVENT_CONNECT_START,
    FTP_EVENT_CONNECT_END,
    FTP_EVENT_AUTH_START,
    FTP_EVENT_AUTH_END,
    FTP_EVENT_UPLOAD_START,
    FTP_EVENT_UPLOAD_END,
    FTP_EVENT_UPLOAD_PROGRESS,
    FTP_EVENT_RETRY_TRIGGERED,
    FTP_EVENT_CIRCUIT_BREAKER_TRIP,
    FTP_EVENT_ERROR
} ftp_event_type_t;

typedef struct {
    ftp_event_type_t type;
    int64_t          timestamp_ns;   /* CLOCK_MONOTONIC */
    const char*      file_path;      /* Nullable */
    const char*      remote_path;    /* Nullable */
    int32_t          status;         /* Error code if applicable */
    uint64_t         bytes_transferred;
    uint64_t         duration_ns;
    const char*      json_payload;   /* Nullable. Extra structured data. */
} ftp_event_t;

typedef void (*ftp_event_cb_t)(const ftp_event_t* event, void* user_data);
```

**Registration:**
```c
int32_t ftp_set_event_callback(ftp_client_t* handle, ftp_event_cb_t cb, void* user_data);
```

### 7.2 Event Payload Examples
| Event | `json_payload` |
|-------|---------------|
| `FTP_EVENT_UPLOAD_END` | `{"attempt_count":2,"retry_delays_ms":[1000,2000],"compression_ratio":0.85}` |
| `FTP_EVENT_ERROR` | `{"ftp_code":550,"retryable":false,"circuit_breaker_state":"OPEN"}` |
| `FTP_EVENT_CONNECT_END` | `{"tls_version":"TLSv1.3","cipher":"TLS_AES_256_GCM_SHA384","handshake_ms":45}` |

### 7.3 Python OpenTelemetry Bridge
The Python binding (Phase 6) can subscribe to this callback and emit spans:

```python
from opentelemetry import trace

def otel_bridge(event, user_data):
    tracer = trace.get_tracer("ftpclient")
    if event.type == FTP_EVENT_UPLOAD_START:
        span = tracer.start_span("ftp.upload", attributes={
            "ftp.local_path": event.file_path,
            "ftp.remote_path": event.remote_path,
        })
        user_data["spans"][event.file_path] = span
    elif event.type == FTP_EVENT_UPLOAD_END:
        span = user_data["spans"].pop(event.file_path)
        span.set_attribute("ftp.bytes", event.bytes_transferred)
        span.end()
```

**Design Principle:** The C++ library emits events. The Python library bridges them. The C++ library knows nothing about OpenTelemetry, keeping dependencies minimal.

---

## 8. Connection Warm-Up & Persistence

### 8.1 Control Channel Keep-Alive
Phase 2 introduced NOOP on idle. Phase 7 enhances this:

- After `ftp_upload_dir()` completes, the control channel remains in `AUTHENTICATED` state.
- A subsequent `ftp_upload_dir()` (or future `ftp_upload_file()`) reuses the existing control channel, skipping TCP + TLS handshake overhead (~50–200ms).
- Idle timeout is configurable (default 60s). If exceeded, the channel is closed and reopened transparently on the next operation.

### 8.2 Session Cache
- TLS session tickets (Phase 3) are cached in the handle.
- Data channel connections in subsequent uploads attempt TLS session resumption, reducing handshake from 2-RTT to 1-RTT (or 0-RTT with TLS 1.3).

---

## 9. Final Benchmark Suite

### 9.1 Comparative Benchmarks
| Competitor | Version | Test Conditions |
|------------|---------|---------------|
| `lftp` | 4.9.x | `lftp -e "mirror -R --parallel=4" ...` |
| `ncftpput` | 3.2.x | `ncftpput -C 4 ...` |
| Python `ftplib` | stdlib | `FTP.storbinary()` in loop |
| Our Library | Phase 7 | `ftp_upload_dir(max_parallel=4)` |

**Test Scenarios:**
1. **Single Large File:** 10GB binary file, loopback, plain FTP.
2. **Small File Flood:** 100,000 files × 4KB, loopback, plain FTP.
3. **WAN Simulation:** 1GB file over 100Mbps link with 50ms RTT, FTPS.
4. **Lossy Link:** 1GB file over 1Gbps with 1% packet loss, plain FTP.

### 9.2 Success Metrics
| Scenario | Target vs `lftp` | Target vs `ftplib` |
|----------|-----------------|-------------------|
| Single Large File | ≥ 105% of `lftp` throughput | ≥ 400% of `ftplib` |
| Small File Flood | ≥ 120% of `lftp` ops/sec | ≥ 800% of `ftplib` |
| WAN Simulation | ≥ 100% of `lftp` (protocol overhead dominates) | ≥ 300% of `ftplib` |
| Lossy Link | ≥ 150% of `lftp` (our retry logic vs `lftp` defaults) | ≥ 500% of `ftplib` |

---

## 10. Production Hardening

### 10.1 Resource Limits Advisory
The library emits a warning (via telemetry callback) if:
- `RLIMIT_NOFILE` < 1024 and `max_parallel` > 8 (risk of EMFILE).
- `RLIMIT_MEMLOCK` < 64KB and TLS is active (Phase 3 secure allocator may fail to lock).

### 10.2 Memory Allocator Hints
- Document recommendation to link against `jemalloc` or `mimalloc` for high-concurrency workloads to reduce allocator contention in the thread pool.
- Provide `CMake` option `FTPCLIENT_USE_JEMALLOC` for static linking.

### 10.3 systemd Integration Notes
For deployments running as a systemd service:
```ini
# /etc/systemd/system/ftp-uploader.service
[Service]
LimitNOFILE=65536
LimitMEMLOCK=infinity
Restart=on-failure
```

---

## 11. Quality Gates & Acceptance Criteria

### 11.1 Zero-Copy Performance Gate
**Test:** Upload 10GB file on Linux loopback, plain FTP, `max_parallel=1`.  
**Pass Criteria:** Throughput ≥ 95% of `cp /src /dev/null` baseline. `strace` confirms `sendfile()` syscalls present, no `read`/`write` pairs.

### 11.2 Rate Limiting Accuracy
**Test:** Upload 1GB file with `bytes_per_second=10MB`.  
**Pass Criteria:** Actual average throughput is 10.0MB/s ± 5%. No burst exceeding 12MB/s for >1 second.

### 11.3 io_uring Fallback Safety
**Test:** Enable `FTP_OPT_USE_IOURING` on kernel 4.19 (pre-5.1).  
**Pass Criteria:** Library silently falls back to blocking I/O. No crashes. `dmesg` shows no errors.

### 11.4 Telemetry Event Completeness
**Test:** Upload directory with 100 files, 10 failing, under rate limit.  
**Pass Criteria:** Every file produces exactly one `UPLOAD_START` and one `UPLOAD_END` event. `RETRY_TRIGGERED` count matches actual retries. `CIRCUIT_BREAKER_TRIP` fires if applicable. No duplicate or missing events.

### 11.5 Compression Ratio Verification
**Test:** Upload 100MB text log file with `MODE Z` negotiated.  
**Pass Criteria:** Wire bytes transferred < 50MB (≥ 2× compression). Remote file size is still 100MB (server decompresses transparently). CPU usage < 20% of one core.

### 11.6 Comparative Benchmark Gate
**Test:** Run full benchmark suite (Section 9) against `lftp` and `ftplib`.  
**Pass Criteria:** Meet or exceed all targets in Section 9.2.

---

## 12. Deliverables

| File | Purpose |
|------|---------|
| `src/platform/ZeroCopyTransport.hpp/cpp` | `sendfile`, `TransmitFile`, `mmap` wrappers |
| `src/platform/IoUringChannel.hpp/cpp` | Experimental io_uring data channel (Linux) |
| `src/platform/PlatformCapabilities.hpp/cpp` | Runtime feature detection (kernel version, syscall availability) |
| `src/transfer/TokenBucket.hpp/cpp` | Rate limiter |
| `src/transfer/ModeZCompressor.hpp/cpp` | zlib deflate for MODE Z |
| `src/observability/Telemetry.hpp/cpp` | Event emission and callback dispatch |
| `include/ftpclient.h` *(amendment)* | Add `ftp_set_rate_limit()`, `ftp_set_event_callback()`, `ftp_set_option()` |
| `python/ftpclient/telemetry.py` | OpenTelemetry bridge and event helpers |
| `python/ftpclient/options.py` | New option constants (`FTP_OPT_USE_IOURING`, etc.) |
| `tests/zero_copy_linux.cpp` | `sendfile()` strace verification |
| `tests/rate_limit_accuracy.cpp` | Token bucket throughput measurement |
| `tests/iouring_fallback.cpp` | Pre-5.1 kernel safety test |
| `tests/telemetry_completeness.cpp` | Event sequence validation |
| `tests/compression_ratio.cpp` | MODE Z wire-size verification |
| `benchmarks/comparative_suite.py` | `lftp`, `ncftp`, `ftplib` comparison harness |
| `docs/PRODUCTION.md` | Deployment guide, systemd notes, allocator recommendations |

---

## 13. Final Transition: Library v1.0.0

Phase 7 is **ratified** when:

1. All 6 Quality Gates (Section 11) pass on CI for Linux x86_64, Windows x64, and macOS x86_64+arm64.
2. The comparative benchmark gate demonstrates superiority over `ftplib` and parity-or-better vs `lftp`.
3. No regressions in Phase 4–6 tests: concurrency correctness, GIL release, Python callback integrity, and memory safety all remain passing.
4. The C ABI remains backward compatible: a binary compiled against Phase 1 headers links and runs correctly against the Phase 7 shared library.
5. The Python package passes `mypy --strict` and `pytest` with the new telemetry and options modules.
6. Documentation is complete: `README.md`, `API.md`, `PRODUCTION.md`, and inline docstrings for all public APIs.

**Upon ratification:** The project is tagged `v1.0.0`. The C ABI enters **permanent lockdown** (no breaking changes without major version bump). Subsequent work proceeds as patch/minor releases under semantic versioning.

---

**End of Phase 7 Specification**
