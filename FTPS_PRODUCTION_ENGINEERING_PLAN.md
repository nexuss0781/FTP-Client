# Production-Grade FTPS Client: End-to-End Engineering Plan

**Project:** `nexuss0781/FTP-Client`  
**Role in Ahadu Deploy:** Secure, high-performance artifact transport for static files and Node.js application releases  
**Planning principle:** Correctness and security first; performance optimizations are accepted only after benchmark evidence proves they preserve protocol behavior.

## 1. Product definition and finish line

The finished client will be a cross-platform C++17 library with a stable C ABI, a tested Python binding, and a high-level deployment adapter. It will support explicit FTPS as the default secure mode, optional implicit FTPS for compatible servers, bounded concurrent transfers, resumable uploads, integrity verification, structured results, cancellation, retries, and telemetry.

A deployment is successful only when the client has completed the data transfer, received the server’s final completion reply, verified the resulting artifact according to the selected policy, and returned an accurate per-file result. A successful TCP connection or a `STOR` preliminary reply is not success.

For Ahadu Deploy, the FTPS client is the **transport layer**, not the process launcher. On a Node-capable cPanel target, the next layer will call Passenger/cPanel operations after FTPS upload. On InfinityFree free hosting, the target policy will permit only compatible static/PHP deployments and will reject Node runtime deployment before transfer.

## 2. Current baseline and blocking gaps

The repository contains useful protocol parsers, directory walking, retry classes, TLS scaffolding, a C ABI, and Python types. The public execution path is not production-ready. `ftp_connect()` in `src/ftpclient.cpp` stores credentials and changes state without invoking a real protocol session; `ftp_ping()` does not send `NOOP`; and `ftp_upload_dir()` returns `FTP_ERR_INVALID_STATE` after validation. `FtpClientImpl` has configuration and callback storage but no live protocol or transfer-engine ownership.

`ProtocolEngine.hpp` contains control-channel groundwork, but `upload_file()`, `create_remote_dir()`, and `download_file()` are stubs. `TransferEngine.cpp` traverses files but does not submit work to the thread pool, reads local data without opening a data channel, marks the task successful as a placeholder, and leaves result filling empty. `ControlThread.hpp` explicitly ignores the requested command timeout, and its stop path can block if the transport read is not interrupted. `TlsTransport.cpp` contains manual hostname matching, incomplete socket ownership behavior, and no robust nonblocking deadline strategy.

The Python wrapper passes `ffi.NULL` instead of the registered progress callback user data and does not consistently raise on native upload errors. The capability flags currently advertise features that the public path does not actually execute. These issues must be resolved before any Ahadu Deploy release is called production-ready.

| Blocking defect | Required correction | Completion gate |
|---|---|---|
| Public connection is state-only | Wire `FtpClientImpl` to a real `ProtocolEngine` and transport | Real server greeting, TLS, authentication, and `NOOP` pass |
| Upload is a stub | Implement real data-channel `STOR` and final reply handling | Uploaded bytes and remote size/hash match |
| Directory transfer is simulated | Connect manifest, MKD, transfer workers, and result aggregation | Nested tree integration test passes |
| FTPS data protection is not enforced end to end | Implement `AUTH TLS`, `PBSZ 0`, `PROT P`, and TLS data sockets | Negative test proves unprotected data cannot be used in strict mode |
| Command timeout is ignored | Implement deadline-aware I/O and queue cancellation | Hung server tests terminate within configured bounds |
| TLS hostname logic is manual | Use OpenSSL verification APIs and explicit SNI/hostname policy | Mismatch, wildcard, IP, expired, and chain tests pass |
| Python progress user data is lost | Preserve callback handle through the native call and clean it in `finally` | Callback receives correct path and progress under concurrency |
| Capability reporting is aspirational | Advertise only tested compiled features | Capability contract matches integration results |

## 3. Protocol requirements

The protocol implementation will follow FTP core behavior from RFC 959, standardized extensions from RFC 3659, and FTPS negotiation from RFC 4217.[1] [2] [3]

### 3.1 Explicit FTPS state machine

The default secure path will be:

```text
TCP connect to control port
  -> read 220 greeting
  -> AUTH TLS
  -> TLS handshake with SNI and certificate verification
  -> reset FTP transfer parameters as required after AUTH
  -> USER / PASS
  -> PBSZ 0
  -> PROT P
  -> FEAT
  -> TYPE I
  -> PWD or target-root selection
  -> transfer operations
  -> QUIT and TLS shutdown
```

`AUTH TLS` must be treated as a protocol state transition, not merely a socket flag. After successful TLS negotiation, credentials must be sent only through the protected control channel. The client will not silently downgrade to plain FTP after an FTPS failure. An explicit insecure mode may exist for controlled local testing, but it will require a deliberate configuration value and will never be the default.

### 3.2 Implicit FTPS

Implicit FTPS will be supported as a separate mode, normally on a provider-selected port such as 990. The TLS handshake begins immediately after the TCP connection, before the FTP greeting is processed. The implementation must not confuse implicit mode with explicit `AUTH TLS`; both modes need independent integration tests.

### 3.3 Control-channel correctness

The control reader must support single-line and multi-line replies, preserve unread bytes between replies, reject malformed or unbounded responses, and map reply codes without losing the original code or redacted server text. Every command must have a deadline, and a timeout must interrupt the relevant socket operation rather than merely time out a future while the control thread remains blocked.

The command actor will serialize commands on one control connection. Concurrent transfer workers must not issue unsynchronized `PASV`, `SIZE`, `REST`, or `STOR` sequences through the same control channel. Each data transfer will use a coordinated control transaction or a dedicated session according to the selected concurrency mode.

### 3.4 Data channels and passive mode

The client will prefer `EPSV` for modern IPv4 and IPv6 compatibility, then fall back to `PASV` when the server advertises or requires it. In passive mode, the client will not blindly connect to an arbitrary address returned by the server; the default policy will use the control peer address when the passive response contains a private or mismatched address, with an explicit override for unusual deployments.

For every protected data transfer, the sequence will be `PBSZ 0`, `PROT P`, passive-channel negotiation, preliminary transfer reply, TLS handshake on the data socket, byte transfer, TLS shutdown or close, and final control reply. RFC 4217 specifies that `PBSZ` must precede `PROT` and that `PROT P` provides private data-channel protection.[1]

### 3.5 Machine-readable listings and resume

The client will prefer `MLSD`/`MLST` where available and fall back to `LIST` only through a clearly isolated parser. It will use `FEAT` to discover `MLSD`, `SIZE`, `MDTM`, `REST`, `HASH`, and related capabilities. RFC 3659 defines `SIZE`, `MDTM`, `REST`, and machine-readable listings for reliable metadata and restart behavior.[2]

Resume is safe only after comparing the local file identity with the remote partial object according to policy. The minimum policy is local size plus a local content hash recorded in the release manifest; stronger policy uses remote `SIZE` and `MDTM`, and strongest policy uses a server-side hash command. When uncertainty exists, the client restarts from zero rather than silently corrupting an artifact.

## 4. TLS and credential security

OpenSSL 3.x will remain the TLS implementation. The default minimum protocol version will be TLS 1.2, with TLS 1.3 preferred. TLS 1.0 and 1.1 will be rejected. The client will configure SNI from the logical hostname and will perform certificate chain validation against the operating-system or explicitly configured trust store. OpenSSL documents chain building, trust anchors, validity checks, intended-use checks, and hostname verification as separate verification concerns.[4]

Hostname verification will use OpenSSL’s hostname/IP verification APIs rather than hand-written wildcard and string comparison logic. A certificate pin may be added as an optional defense-in-depth check, but pinning will not replace normal chain and hostname verification. A pin mismatch will produce a distinct certificate error and will never trigger a plaintext fallback.

`CCC` and `PROT C` will be disabled by default. The client will not log credentials, full commands containing `PASS`, certificate private material, or server responses that may contain secrets. Logs will use redacted command names and bounded, sanitized reply text. The credential vault will track lengths explicitly, cleanse memory using a non-optimizable routine, use best-effort page locking and core-dump exclusion where supported, and document that operating-system guarantees vary.

The credential provider callback will be invoked at connection time, and returned buffers will be deep-copied into controlled storage only for the connection lifetime. Provider callbacks, certificate callbacks, and progress callbacks will have explicit lifetime and thread-safety rules. No callback may be invoked after client destruction begins.

## 5. Production architecture

```text
C ABI / Python binding / Ahadu Deploy adapter
                    |
             Session facade
                    |
      +-------------+-------------+
      |                           |
 Control session actor      Transfer coordinator
      |                           |
 TLS control transport     Bounded worker pool
      |                           |
      +-------------+-------------+
                    |
        Per-transfer data sessions
                    |
       Socket + TLS + buffer pool
                    |
        FTP/FTPS server compatibility
```

### 5.1 Session facade

`FtpClientImpl` will own a session object, configuration snapshot, credential provider, telemetry sink, and lifecycle mutex. It will enforce states such as `ALLOCATED`, `CONNECTING`, `AUTHENTICATING`, `CONNECTED`, `TRANSFERRING`, `DISCONNECTING`, `DISCONNECTED`, and `DESTROYED`. Public calls made in an invalid state will return deterministic errors.

The facade will expose both directory-level and single-file operations. Ahadu Deploy requires single-file upload for release artifacts such as `tmp/restart.txt`, so the ABI should add an additive `ftp_upload_file()` operation or a general transfer API with a `struct_size`-versioned options structure.

### 5.2 Control session actor

The control connection will be owned by one actor thread or one serialized event loop. It will own the reply parser, protocol state machine, command queue, deadlines, and server feature set. It will expose typed operations such as `login`, `feat`, `cwd`, `mkd`, `size`, `mdtm`, `rest`, `stor_prepare`, `stor_complete`, `rnfr_rnto`, `hash`, and `quit` rather than making higher layers assemble raw commands.

A command must carry an operation identifier, deadline, cancellation token, expected reply class, and redaction policy. Queue shutdown will complete all pending promises with a shutdown error and interrupt the underlying socket so destruction cannot hang indefinitely.

### 5.3 Transfer coordinator

The coordinator will build a manifest, apply exclusions and symlink policy, create parent directories, schedule file tasks, aggregate results, and enforce cancellation. It will start with one data worker for correctness, then support bounded concurrency with a default of four and a hard configurable maximum. A server-adaptive mode may lower concurrency after `421` or resource-limit responses.

Each file task will have a clear state sequence: `PLANNED`, `OPENED`, `REMOTE_PREPARED`, `TRANSFERRING`, `FINALIZING`, `VERIFIED`, `RENAMED`, `SUCCEEDED`, or `FAILED`. Temporary names will be used when atomic visibility matters. For example, `app.js` may upload as `.ahadu.partial.<release-id>.app.js`, then rename after verification.

### 5.4 Data session

Every worker gets a data-session object that owns the passive endpoint, socket, TLS state, I/O deadline, byte counters, and cleanup. TLS data sessions must be established only after the control transaction has accepted `PROT P`. A data-session failure must close the socket and return a typed error to the retry controller; it must not poison unrelated workers silently.

### 5.5 Buffering and performance

The client will use a bounded reusable buffer pool. For plaintext data channels, a platform zero-copy path may be evaluated. For FTPS, encryption generally requires data to pass through the TLS stack, so `sendfile()` must not be claimed as a universal FTPS optimization. The first production baseline should use 64 KiB to 1 MiB buffers selected by benchmark and memory budget, with no unbounded allocation per file.

Hashing will be performed while bytes are read, avoiding a second file pass when possible. Compression will remain opt-in and disabled for already-compressed deployment assets. `io_uring` will not be introduced until the blocking implementation has passed compatibility and performance gates; premature asynchronous complexity would obscure protocol bugs.

## 6. Implementation workstreams

### Workstream A: Public API and lifecycle

Update `include/ftpclient.h` with additive, `struct_size`-versioned options for single-file transfer, cancellation, integrity policy, transfer concurrency, and data-channel protection. Preserve existing 1.x symbols. Add explicit error codes for TLS negotiation, hostname mismatch, data-channel protection failure, timeout phase, cancellation, integrity mismatch, unsupported feature, and remote final-reply failure.

Wire `src/ftpclient.cpp` to the real session facade. Remove state-only success paths. Ensure every output structure is initialized, every owned allocation has a matching free operation, and `ftp_result_free()` is safe after partial failures.

### Workstream B: Transport and TLS

Refactor `TlsTransport.cpp` to use a clear socket ownership model. Separate TCP connect from TLS handshake for explicit FTPS, and implement immediate handshake for implicit FTPS. Apply connect, handshake, read, write, and shutdown deadlines. Use OpenSSL verification APIs, SNI, trust-store selection, and optional SPKI pinning. Convert OpenSSL errors into structured internal diagnostics without leaking secrets.

Add a platform socket abstraction for Linux, macOS, and Windows. The abstraction must support connect deadlines, poll/select-based readiness, interruptible shutdown, IPv4/IPv6 address resolution, and socket option configuration. Keep OpenSSL-specific types below the transport interface.

### Workstream C: Protocol engine

Complete reply parsing, multi-line handling, feature discovery, state transitions, path encoding, passive endpoint parsing, `EPSV`/`PASV` fallback, and protected-data negotiation. Add typed operations for `MLSD`, `SIZE`, `MDTM`, `REST`, `HASH`, `MKD`, `STOR`, `RNFR`, `RNTO`, and `DELE`.

The protocol engine must preserve server reply codes. A generic `FTP_ERR_PROTOCOL` is insufficient for retry and diagnostics if the original code was `421`, `425`, `426`, `450`, `451`, `452`, `530`, `550`, or `552`.

### Workstream D: Transfer engine

Replace the current simulated worker with real data-channel operations. Submit actual tasks to the thread pool, implement progress throttling, cancellation, remote directory creation, temporary-file rename, resume policy, integrity checking, and result aggregation. Ensure a failure in one file does not corrupt another file’s result or leave a worker-held buffer unreleased.

Add zero-byte upload support because Ahadu Deploy uses an empty `tmp/restart.txt` trigger. Add remote existence and metadata queries so the deployment layer can perform safe idempotent releases.

### Workstream E: Resilience

Integrate retry policy with typed operation classes. Retry connection resets, timeouts, selected passive-channel failures, and transient 4xx replies. Do not automatically retry authentication failures, certificate failures, local filesystem errors, malformed protocol replies, or uncertain destructive operations.

Implement exponential backoff with bounded full jitter, per-host circuit state, transfer stall detection based on progress timestamps, and cancellation that interrupts both control and data channels. Record attempt number, phase, server reply code, and elapsed time in redacted telemetry.

### Workstream F: Python binding

Fix callback user-data propagation in `python/ftpclient/client.py`. Register callbacks before the native call and unregister them in `finally` blocks. Apply `_check_error()` consistently to connection, upload, result-free, configuration, and callback-registration operations. Map native errors to the documented exception hierarchy while preserving operation metadata.

Release wheels only after ABI tests, platform tests, callback tests, and an FTPS integration test pass. The Python API should offer a synchronous core and an optional asynchronous wrapper that delegates blocking operations to an executor rather than pretending the native protocol is natively async.

### Workstream G: Build and packaging

Update `CMakeLists.txt` so all intended source files are compiled into the library, optional features are detected rather than assumed, and OpenSSL linkage is reproducible. Add CMake presets, compiler warnings, ASAN/UBSAN/TSAN configurations, symbol visibility checks, and package metadata. Keep generated build outputs out of source control.

## 7. Test and verification program

### 7.1 Unit and property tests

Test the reply parser with valid single-line replies, multi-line replies, split TCP reads, oversized replies, malformed codes, embedded punctuation, and unexpected EOF. Fuzz command parsing and passive endpoint parsing. Test path normalization against traversal, null bytes, control characters, Unicode, and platform separator differences.

Test the state machine for every valid and invalid transition. Test timeout and cancellation behavior with fake transports. Test retry classification and idempotency rules. Test credential cleanup with length-aware buffers and callback lifetime.

### 7.2 FTPS interoperability matrix

Use controlled servers representing at least two independent implementations, including one server with explicit FTPS, one with implicit FTPS, one with IPv6, one behind a passive-address NAT behavior, and one with restrictive feature support. The matrix should include:

| Scenario | Expected result |
|---|---|
| Explicit `AUTH TLS`, verified certificate | Connect and transfer successfully |
| Explicit TLS with hostname mismatch | Fail before credentials are sent |
| Implicit FTPS | Handshake before greeting and transfer successfully |
| `PROT P` rejected | Fail in strict secure mode; no plaintext data transfer |
| EPSV supported | Use EPSV |
| EPSV rejected, PASV supported | Fall back to PASV safely |
| Private PASV address | Use validated control peer policy or fail according to configuration |
| MLSD unsupported | Use isolated LIST fallback |
| REST unsupported | Restart from zero or fail according to resume policy |
| Final reply missing after data close | Fail; never report success from byte count alone |
| Remote quota or permission error | Preserve reply code and return typed remote error |
| Connection reset mid-file | Retry only when operation policy allows |

### 7.3 Security tests

Include invalid certificate chains, expired certificates, not-yet-valid certificates, hostname mismatch, IP-address certificate matching, wildcard boundary cases, untrusted self-signed certificates, TLS version downgrade attempts, disabled certificate verification, SPKI pin mismatch, and leaked `PASS` data in logs. Verify that strict mode sends `PBSZ 0` and `PROT P` and never uses `CCC` or `PROT C` implicitly.

Run static analysis, ASAN, UBSAN, and TSAN. Add a credential memory audit where feasible, while documenting that process memory protection is best effort and cannot guarantee against a privileged debugger or host-level compromise.

### 7.4 Chaos tests

Inject latency, packet loss, connection resets, half-closed sockets, delayed final replies, server-side `421` responses, and data-channel refusal. Verify bounded completion time, no deadlocks, no double-free, no leaked worker threads, accurate partial results, and safe retry behavior.

### 7.5 Performance tests

Benchmark single large files, many small files, mixed trees, FTPS control plus data protection, high-latency links, rate limits, and different concurrency settings. Record throughput, CPU, resident memory, TLS handshake count, average and p95 per-file latency, retries, and server connection count.

Do not retain the existing README benchmark claims until they are reproduced from a clean build with documented hardware, server version, TLS settings, file corpus, and measurement method. Set release targets from measured baselines. A reasonable first gate is no correctness regression while achieving stable throughput with bounded memory at four concurrent transfers; higher targets should be evidence-based.

## 8. Ahadu Deploy integration

The transport must expose the operations Ahadu Deploy needs:

```text
connect_ftps()
probe_capabilities()
create_remote_directories()
upload_file()
upload_directory()
verify_remote_size_or_hash()
rename_remote()
write_restart_trigger()
collect_transfer_result()
disconnect()
```

The cPanel/Passenger adapter will use this sequence:

```text
1. Preflight target capabilities.
2. Reject Node deployment on InfinityFree free-hosting profile.
3. Build and scan the release locally.
4. Exclude secrets, node_modules, logs, and .git.
5. Upload the release through FTPS.
6. Register or edit Passenger application through cPanel UAPI.
7. Ensure npm dependencies.
8. Enable the application.
9. Upload zero-byte tmp/restart.txt.
10. Verify the public /health endpoint.
11. Store a redacted release record and per-file results.
```

The target adapter must never infer runtime support from an FTP login. A target is Node-capable only when the provider-managed runtime, process manager, application registration, reverse proxy, dependency installation, and restart mechanism have been verified.

## 9. Release gates

### Alpha gate: real single-file transfer

The alpha release must connect to a controlled explicit FTPS server, verify the certificate and hostname, authenticate, negotiate `PROT P`, upload one file, receive the final reply, verify remote size, and cleanly disconnect. No public API may return simulated success.

### Beta gate: reliable directory release

The beta release must upload nested trees with bounded concurrency, partial results, cancellation, retry classification, resume policy, integrity verification, and accurate progress callbacks. It must pass interoperability and chaos tests.

### Release-candidate gate: secure production transport

The release candidate must pass the full TLS negative test suite, sanitizers, race tests, fuzz targets, ABI compatibility tests, Python binding tests, reproducible builds, and performance baselines. Capability flags and README documentation must match tested behavior.

### Ahadu Deploy gate: application release

The Ahadu Deploy integration must complete a staging deployment to a Node-capable cPanel/Passenger target, restart the application through the supported provider mechanism, verify `/health`, and preserve rollback information. It must also prove that an InfinityFree-free target is blocked for Node deployments before any transfer occurs.

## 10. Recommended implementation order

| Order | Work package | Exit condition |
|---:|---|---|
| 1 | Branch, CI, clean build, truthful capability flags | Clean checkout builds and current tests are classified as unit versus integration |
| 2 | Socket deadlines and transport ownership | No hangs or leaks under timeout and shutdown tests |
| 3 | Explicit FTPS control negotiation | AUTH TLS, verification, USER/PASS, PBSZ 0, PROT P pass |
| 4 | Passive protected data session | EPSV/PASV plus TLS data channel uploads one file |
| 5 | Single-file API and result ownership | Accurate final-reply result and zero-byte upload support |
| 6 | Directory transfer and remote directories | Nested tree succeeds with real bytes |
| 7 | Resume, integrity, retries, cancellation | Chaos suite passes with correct partial results |
| 8 | Bounded concurrency and buffer tuning | Benchmark improves without server overload or memory growth |
| 9 | Python binding hardening | Callback, exception, wheel, and ABI tests pass |
| 10 | cPanel/Passenger Ahadu adapter | FTPS upload → cPanel registration → restart → health check succeeds |
| 11 | Release hardening | Security, interoperability, performance, packaging, and documentation gates pass |

## 11. Engineering non-negotiables

The client will never report success from local byte reads alone. It will never silently downgrade from FTPS to plaintext. It will never log passwords or private key material. It will never perform unbounded concurrency or unbounded response buffering. It will never use a copied Node runtime as proof that the destination can execute Node.js. It will never expose an Ahadu Deploy release as active until transfer completion, activation, and health verification have all succeeded.

## References

[1]: https://datatracker.ietf.org/doc/html/rfc4217 "RFC 4217: Securing FTP with TLS"

[2]: https://datatracker.ietf.org/doc/html/rfc3659 "RFC 3659: Extensions to FTP"

[3]: https://www.rfc-editor.org/info/rfc959 "RFC 959: File Transfer Protocol"

[4]: https://docs.openssl.org/3.1/man1/openssl-verification-options/ "OpenSSL: X.509 certificate verification options"
