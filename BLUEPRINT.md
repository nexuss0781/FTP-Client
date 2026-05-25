# Professional Specification Proposal: High-Performance FTP Client Library

## Executive Summary

This proposal outlines a componentized specification for a C++17/20 native FTP client library exposing a C ABI for Python consumption. The design prioritizes **throughput saturation**, **fault resilience**, and **credential security** over GUI ergonomics (FileZilla's domain). The library targets programmatic batch operations where human interaction is impossible.

---

## Architectural Philosophy & FileZilla Trade-off Analysis

| Dimension | FileZilla Approach | Our Library Approach | Trade-off Rationale |
|-----------|-------------------|---------------------|---------------------|
| **Interaction Model** | GUI event-driven, human-paced | API-driven, headless batch | We eliminate wxWidgets overhead and UI latency; optimize for automation |
| **Concurrency** | Single queue with UI updates | Lock-free work-stealing thread pool | Saturate bandwidth via parallel streams rather than sequential queue |
| **Protocol Scope** | Multi-protocol (FTP/SFTP/S3) | FTP/FTPS core only (extensible) | Depth over breadth; FTP done correctly is better than FTP done partially |
| **Memory Model** | Standard heap (GUI tolerates pauses) | Custom allocator + secure zeroing | Prevents credential leakage in core dumps; eliminates fragmentation under load |
| **Error Handling** | Dialog boxes, manual retry | Automatic classification + circuit breaker | Unattended operation requires autonomous recovery |
| **Credential Storage** | Site Manager XML (encrypted at rest) | In-memory secure vault only (no persistence) | Library assumes caller manages persistence; we manage runtime exposure |

---

## Component Breakdown (7 Specification Phases)

### Phase 1: Foundation & C ABI Contract
**Goal:** Immutable interface boundary. Once set, never changes.

**Spec Components:**
- **ABI Stability Charter:** `extern "C"` function naming, calling convention (`cdecl`/`stdcall` per platform), opaque handle pattern (`ftp_client_t`), versioned API table (vtable dispatch for future extensibility without breaking ABI)
- **Build System Spec:** CMake with preset configurations for Linux (gcc/clang), Windows (MSVC), macOS (clang). Static vs shared library artifacts. Symbol visibility control (`__attribute__((visibility("hidden")))` / `__declspec(dllexport)`)
- **Error Code Taxonomy:** Hierarchical error classification — `FTP_OK`, `FTP_ERR_NETWORK_TRANSIENT` (retry candidate), `FTP_ERR_NETWORK_PERMANENT` (fail fast), `FTP_ERR_PROTOCOL` (server violation), `FTP_ERR_AUTH` (credential), `FTP_ERR_LOCAL_FS` (permission/disk). No exceptions cross the ABI.
- **Opaque Context Handle:** All state lives in C++ heap behind `void*`. Python sees only an integer-like handle. Reference counting or explicit `ftp_close()` — spec must choose deterministic cleanup model.

**Quality Gate:** ABI compatibility test using `ctypes` in Python loading a dummy shared object; must survive C++ struct layout changes without recompilation.

---

### Phase 2: Protocol Engine Core
**Goal:** RFC 959 + RFC 3659 (Extensions) compliant command pipeline with minimal latency.

**Spec Components:**
- **Command Pipeline Architecture:** State machine (NOT blocking sequential calls). Separate control channel thread from command dispatch. Support for command pipelining where server allows (send `PASV` + `STOR` without waiting for intermediate ACK when safe).
- **Connection Lifecycle Manager:** TCP keep-alive tuning, TCP_NODELAY for control channel, explicit connection pooling (persistent connections across directory uploads). Idle timeout detection with RST handling.
- **Passive Mode Intelligence:** Parse `PASV` and `EPSV` responses, handle IPv4/IPv6 dual-stack. NAT traversal helper (parse public IP vs local IP mismatch).
- **Directory Traversal Spec:** C++ `std::filesystem` recursive walker with configurable depth limits, symlink handling policy (follow/ignore/error), and early pruning (e.g., skip `.git` patterns).

**Performance Decision:** Use **synchronous blocking I/O with a thread pool** rather than async io_uring for Phase 2. Rationale: FTP is inherently request-response; the win from async I/O is marginal compared to connection parallelism. Simpler code, easier to make robust. *Revisit io_uring in Phase 7.*

---

### Phase 3: Security & Credential Vault
**Goal:** Credential handling more robust than FileZilla's runtime model.

**Spec Components:**
- **Secure Memory Allocator:** `mlock()` / `VirtualLock` pages holding credentials. `madvise(MADV_DONTDUMP)` / `MEMORY_INFO` exclusion from core dumps. Explicit `explicit_bzero` / `SecureZeroMemory` on free.
- **Credential Struct Design:** Plain C struct (`ftp_credentials_t`) with `const char*` fields. C++ side immediately copies into secure allocator; original stack copy zeroed. No credential strings remain in C++ standard containers.
- **Auth Method Negotiation:** Plain FTP, FTPS (Explicit TLS via `AUTH TLS`), FTPS Implicit. TLS version enforcement (1.2 minimum, 1.3 preferred). Certificate validation callbacks exposed through C ABI (allow Python to supply custom CA bundle or pinning logic).
- **Secret Injection Interface:** Support for credential providers rather than raw strings — e.g., `ftp_set_credential_provider(handle, callback)` where callback is invoked at connection time. This allows integration with Python keyring modules or HSMs without the C++ library ever persisting secrets.

**Quality Gate:** Memory forensic audit — attach debugger, dump core, grep for password string. Must not appear in plaintext.

---

### Phase 4: Transfer Engine & Concurrency
**Goal:** Saturate network bandwidth for directory tree uploads.

**Spec Components:**
- **Thread Pool Spec:** Fixed worker pool sized to `std::thread::hardware_concurrency()` or user override. Work-stealing queue for file tasks. Separate I/O threads vs CPU threads (compression/hashing if added later).
- **Per-File Transfer State:** Upload resume via `REST` + `STOR` (or `APPE` depending on server). Atomic rename on completion (`STOR` to temp name, `RNTO` final name) to prevent partial file exposure.
- **Concurrent Stream Policy:** Configurable max parallel uploads (default 4, capped at 16). Server-friendly backoff if receiving `421 Too many connections`. Connection-per-file vs connection reuse tradeoff: **Control connection persistent, data connection per file** — optimal for NAT/firewall scenarios.
- **Buffer Strategy:** Zero-copy where possible (`sendfile()` on Linux if available, falling back to `mmap()` + fixed buffer pool). Avoid `std::iostream` — use raw POSIX/Win32 file descriptors. Buffer size tuned to BDP (Bandwidth-Delay Product) — default 256KB, auto-detect via RTT sampling.

**Robustness Addition:** Transfer integrity. Optional `XMD5`, `XSHA1`, or `HASH` (RFC 3659) verification post-upload. If server lacks hash commands, client-side MD5 computed during upload and exposed to Python for manual verification.

---

### Phase 5: Resilience & Fault Recovery
**Goal:** Unattended operation over unreliable networks.

**Spec Components:**
- **Retry Policy Engine:** Exponential backoff with jitter (full jitter algorithm). Configurable max attempts (default 3). Separate policies for connection open vs transfer mid-stream. Idempotency analysis — `STOR` restarts are safe with `REST`; `DELE`/`RNTO` are not idempotent and require special handling.
- **Circuit Breaker:** Per-host failure tracking. After N consecutive connection failures, fast-fail new requests for M seconds to avoid hammering dead servers.
- **Partial Failure Handling:** Directory upload returns a structured result — list of (file_path, status, bytes_transferred, error_code). Successfully uploaded files are not retried; failed files enumerated for caller decision.
- **Timeout Hierarchy:** Connect timeout (5s), command timeout (30s), data transfer timeout (configurable, dynamic based on bandwidth — no progress for 60s = stall detection).

**Quality Gate:** Chaos testing specification — network latency injection, random TCP RST, bandwidth throttling. Library must complete 10GB tree upload over 10% packet loss link.

---

### Phase 6: Python Binding & Ergonomics
**Goal:** Zero-friction Python API without sacrificing performance.

**Spec Components:**
- **Binding Technology Selection:** `cffi` over `ctypes`. Rationale: `cffi` parses C declarations, handles struct alignment correctly, and has lower call overhead for bulk operations. Avoid Cython (requires C compiler at Python install time) and pybind11 (C++ ABI, not C ABI).
- **Python API Surface:**
  - Context manager support (`with FTPClient(...) as c:`)
  - `upload_directory(local, remote, progress_callback=None)`
  - Progress callback spec: callable receiving `(file_path, bytes_done, bytes_total, current_bps)` — invoked from C++ via Python C-API callback, frequency throttled to 10Hz to prevent GIL contention.
  - Exception mapping: C error codes map to Python exception hierarchy (`FTPAuthError`, `FTPNetworkError`, `FTPProtocolError`)
- **GIL Strategy:** C++ upload runs entirely in native threads. Progress callbacks acquire GIL briefly. Result marshaling happens once at completion, not per-file.

---

### Phase 7: Optimization & Observability
**Goal:** Production-grade tuning and debugging.

**Spec Components:**
- **Telemetry Interface:** Optional C ABI callback for structured events (connect start, handshake complete, transfer begin/end, retry triggered). Enables Python-side metrics export (Prometheus, etc.).
- **Platform Optimizations:** Linux `sendfile()` syscall, Windows TransmitFile, macOS copy-on-write where applicable. io_uring revisit for Linux 5.1+ if benchmarks justify.
- **Compression Negotiation:** MODE Z (deflate on-the-fly) if server supports — spec defines detection and fallback.
- **Benchmark Suite Spec:** Comparative benchmarks against `lftp`, `pftp`, and Python `ftplib`. Target: 2x throughput of `ftplib` on localhost, 1.5x on WAN. Memory profile: <50MB resident for 10,000 file directory tree.

---

## Recommended Implementation Sequence

| Phase | Duration Estimate | Dependency | Risk Level |
|-------|------------------|------------|------------|
| 1. Foundation/ABI | 1 week | None | Low |
| 2. Protocol Core | 2 weeks | Phase 1 | Medium |
| 3. Security Vault | 1 week | Phase 1 | Low |
| 4. Transfer Engine | 2 weeks | Phase 2, 3 | High |
| 5. Resilience | 2 weeks | Phase 4 | Medium |
| 6. Python Binding | 1 week | Phase 1 | Low |
| 7. Optimization | 2 weeks | Phase 5, 6 | Medium |

**Integration Rule:** Each phase produces a standalone spec document with acceptance criteria. Phases 1-3 can proceed in parallel after ABI freeze. Phase 4 is the critical path. Phase 6 (Python binding) can start against Phase 1 stubs using mock C++ implementation.

---

## Critical Success Metrics

1. **Throughput:** Saturate 1Gbps link with >8 parallel streams on typical Linux host
2. **Reliability:** 99.9% success rate over simulated 5% packet loss network for 1,000 file upload
3. **Security:** Pass static analysis (Coverity/SonarQube) with zero high-severity findings; pass memory dump credential leak test
4. **Compatibility:** Work against vsftpd, ProFTPD, Pure-FTPd, Windows IIS FTP, and AWS Transfer Family without code changes

---

## Next Step

Confirm this component breakdown aligns with your constraints, and I will draft the **detailed specification document for Phase 1 (Foundation & C ABI Contract)** as the first deliverable. This establishes the immutable boundary upon which all subsequent phases depend.
