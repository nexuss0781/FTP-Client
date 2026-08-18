# FTPS Client: Manageable Engineering Roadmap

**Scope:** FTPS client library only.  
**Out of scope:** Ahadu Deploy, Node.js launch, PHP launchers, cPanel, hosting adapters, deployment orchestration, and application health checks.

## Product finish line

The finished product will be a reliable C++17 FTPS client with a stable C ABI and Python bindings. It will connect to explicit and implicit FTPS servers, validate certificates and hostnames, negotiate protected control and data channels, upload and download files, create and inspect directories, resume interrupted transfers, verify integrity, retry safely, report progress, and operate under bounded memory and concurrency.

The implementation will be considered production-ready only when it performs real network transfers against independent FTPS servers. Unit tests and simulated byte reads are not sufficient evidence of readiness.

## Current baseline

The repository has useful protocol, TLS, resilience, transfer, ABI, and Python scaffolding, but its public path is incomplete. The public `ftp_connect()` stores credentials and changes state without opening a real protocol session. `ftp_ping()` does not send `NOOP`. `ftp_upload_dir()` is still a stub. The protocol upload and remote-directory methods are stubs, and the transfer engine reads local bytes without sending them through an FTP data channel. The control-thread timeout argument is ignored, and the TLS transport requires stronger socket ownership, hostname-verification, and deadline handling.

The roadmap below deliberately avoids trying to implement everything at once. Each phase produces a usable increment and a clear acceptance test.

## Phase 1 — Scope freeze and clean baseline

**Goal:** Establish a truthful, reproducible starting point.

**Work:** Remove or clearly label simulated behavior; document the supported API; define the error taxonomy; add a clean CMake build; keep generated artifacts out of source control; add compiler warning, ASAN, UBSAN, and TSAN configurations; separate unit tests from real-server integration tests; freeze the C ABI rules for the 1.x line.

**Deliverables:** A clean-build CI job, a capability matrix, a test inventory, an ABI compatibility test, and a short implementation status document.

**Acceptance:** A clean checkout builds from an empty build directory, existing tests pass, and no public function reports a feature as available when the public path is still simulated.

**Dependency:** None.  
**Recommended size:** 2–4 engineering days.

## Phase 2 — Minimal plain FTP control path

**Goal:** Make the control connection real before adding TLS complexity.

**Work:** Implement socket ownership, DNS resolution, IPv4/IPv6 connection, connect deadlines, greeting parsing, command serialization, multiline replies, reply-code preservation, `USER`, `PASS`, `SYST`, `FEAT`, `TYPE I`, `PWD`, `NOOP`, `QUIT`, and deterministic shutdown. Complete the protocol state machine and ensure all pending commands receive a result when the session closes.

**Deliverables:** A real `ProtocolSession`, a deadline-aware `ControlThread`, typed command operations, and a reliable `ftp_connect()`/`ftp_ping()`/`ftp_disconnect()` path.

**Acceptance:** Against a controlled FTP server, the client connects, authenticates, sends `NOOP`, returns the actual server reply code, and shuts down without hanging after network interruption.

**Dependency:** Phase 1.  
**Recommended size:** 1–2 weeks.

## Phase 3 — Explicit FTPS control security

**Goal:** Secure the control channel correctly and fail closed.

**Work:** Implement `AUTH TLS`, TLS handshake, SNI, TLS 1.2 minimum, TLS 1.3 preference, system/custom trust stores, certificate-chain verification, hostname/IP verification through OpenSSL verification APIs, and structured TLS errors. After `AUTH TLS`, reset protocol parameters as required, re-authenticate, issue `PBSZ 0`, and require `PROT P` in strict mode. Disable silent downgrade, `CCC`, and `PROT C` by default.

**Deliverables:** A production TLS transport, explicit FTPS configuration, certificate policy, redacted TLS diagnostics, and a secure-session state machine.

**Acceptance:** A valid certificate succeeds; expired, self-signed, untrusted, hostname-mismatch, wrong-IP, and TLS-version-downgrade cases fail before credentials are exposed. A server that refuses `PROT P` fails strict mode rather than sending unprotected data.

**Dependency:** Phase 2.  
**Recommended size:** 1–2 weeks.

## Phase 4 — Single-file protected data transfer

**Goal:** Transfer one real file through a protected data channel.

**Work:** Implement `EPSV` first and `PASV` fallback; validate passive addresses; create a data socket; negotiate TLS on the data channel; implement `STOR`, `RETR`, `ABOR`, `SIZE`, and final-reply handling. Add a single-file C ABI operation, accurate byte counters, zero-byte file support, and result ownership.

**Deliverables:** `ftp_upload_file()`, `ftp_download_file()`, data-session ownership, progress callbacks, and per-operation result structures.

**Acceptance:** Small, empty, binary, and multi-megabyte files upload and download correctly over explicit FTPS. The client reports failure if the data socket transfers bytes but the final control reply is missing or negative.

**Dependency:** Phase 3.  
**Recommended size:** 1–2 weeks.

## Phase 5 — Remote filesystem operations and directory transfer

**Goal:** Support reliable file trees without concurrency yet.

**Work:** Implement local traversal with depth and symlink policies; implement remote `MKD`, `CWD`, `CDUP`, `MLSD`/`MLST`, `LIST` fallback, `MDTM`, `RNFR`/`RNTO`, and `DELE`. Create parent directories in deterministic order. Add temporary remote filenames and rename-on-success where the server supports it. Complete result aggregation and partial-failure reporting.

**Deliverables:** A single-worker `upload_directory()` and `download_directory()` path, machine-readable listing parser, path-normalization policy, and remote-operation error mapping.

**Acceptance:** A nested tree containing empty directories, binary files, Unicode names, and zero-byte files transfers correctly. One failed file produces accurate partial results without hiding successful files or leaking resources.

**Dependency:** Phase 4.  
**Recommended size:** 1–2 weeks.

## Phase 6 — Resume, integrity, retry, and cancellation

**Goal:** Make transfers safe on unreliable networks.

**Work:** Implement `FEAT` capability discovery; resume with `SIZE`/`REST`; compare local and remote metadata before resuming; add optional server-side `HASH` support and client-side SHA-256; classify retryable and non-retryable errors; add exponential backoff with bounded jitter; implement cancellation and stall detection; ensure timeout interrupts socket I/O.

**Deliverables:** Transfer policy objects, integrity results, cancellation tokens, retry telemetry, and a deterministic recovery model.

**Acceptance:** A mid-transfer connection reset can resume or restart according to policy; changed local content is not incorrectly appended to an old remote partial; integrity mismatch is detected; cancellation completes within the configured bound.

**Dependency:** Phase 5.  
**Recommended size:** 1–2 weeks.

## Phase 7 — Bounded concurrency and performance

**Goal:** Improve throughput without sacrificing correctness or overloading servers.

**Work:** Add a bounded worker pool; choose a safe control/data-session model; add reusable buffer pools; enforce maximum simultaneous connections; schedule files by measured cost; add optional rate limiting; hash while streaming; and profile CPU, memory, TLS, and network behavior. Start with four workers and make concurrency configurable.

Do not claim universal zero-copy for FTPS. TLS encryption normally requires the data to pass through the TLS stack, so any platform-specific optimization must be benchmarked separately from the protected path.

**Deliverables:** Concurrent directory transfers, memory limits, connection limits, rate-limiter configuration, benchmark harness, and performance dashboard output.

**Acceptance:** Throughput improves over the single-worker baseline on a controlled server while memory remains bounded, no connection leaks occur, and server-side connection limits are respected. Performance claims are reproducible from a clean build and documented test corpus.

**Dependency:** Phase 6.  
**Recommended size:** 1–2 weeks.

## Phase 8 — Security hardening and API correctness

**Goal:** Make the client safe for unattended use.

**Work:** Replace manual certificate hostname matching with OpenSSL APIs; improve credential memory handling with explicit lengths and secure cleansing; add best-effort page locking/core-dump exclusion; redact passwords and secrets from logs; define callback lifetime and thread-safety rules; fix progress callback user-data handling; use `finally`-equivalent cleanup in Python; and ensure destruction interrupts all workers and sockets.

**Deliverables:** Security review checklist, redacted structured logging, callback-lifetime tests, secret-handling tests, and corrected Python exception mapping.

**Acceptance:** Static analysis, sanitizers, race tests, callback failure tests, interpreter-shutdown tests, and credential-leak checks pass. No insecure fallback is reachable through default configuration.

**Dependency:** Phases 3, 6, and 7.  
**Recommended size:** 1 week.

## Phase 9 — Compatibility and chaos validation

**Goal:** Prove the client works beyond one server implementation.

**Work:** Test against at least two independent FTPS servers and include explicit FTPS, implicit FTPS, IPv4, IPv6, EPSV/PASV fallback, NAT-mismatched passive addresses, MLSD absence, REST absence, quota failures, permission errors, server throttling, delayed replies, connection resets, half-closed sockets, and malformed responses.

**Deliverables:** Interoperability matrix, chaos test suite, compatibility notes, and a known-server-quirks document.

**Acceptance:** All supported scenarios pass with correct error codes and bounded completion times. Unsupported server behavior produces an explicit capability or protocol error rather than silent corruption.

**Dependency:** Phases 4–8.  
**Recommended size:** 1–2 weeks.

## Phase 10 — Packaging and release

**Goal:** Ship a maintainable library rather than a working developer checkout.

**Work:** Produce Linux, Windows, and macOS artifacts where supported; build Python wheels; publish headers, CMake package configuration, changelog, API documentation, security policy, and migration notes; add symbol-compatibility checks; sign release artifacts; and define vulnerability-response procedures.

**Deliverables:** Versioned shared libraries, Python wheels, C ABI headers, install documentation, release notes, and reproducible build metadata.

**Acceptance:** A clean machine can install the library and run a real FTPS smoke test using documented commands. ABI compatibility is verified against the previous 1.x release, and all release artifacts are traceable to a source commit.

**Dependency:** Phases 1–9.  
**Recommended size:** 1 week.

## Recommended milestone sequence

| Milestone | Included phases | Result |
|---|---:|---|
| M0 | 1 | Truthful, reproducible baseline |
| M1 | 2–3 | Real secure FTPS control session |
| M2 | 4 | One-file protected upload/download |
| M3 | 5 | Reliable single-worker directory transfer |
| M4 | 6 | Resume, integrity, retry, cancellation |
| M5 | 7 | Bounded high-performance transfer |
| M6 | 8–9 | Hardened and interoperable client |
| M7 | 10 | Packaged production release |

## Non-negotiable release rules

The client must not mark a transfer successful from local reads alone. It must receive and validate the server’s final completion reply. It must not silently downgrade from FTPS to plaintext. It must not advertise unsupported capabilities. It must not use unbounded response buffers, threads, connections, or memory. It must not log passwords, private keys, or complete sensitive commands. It must not make performance claims without reproducible benchmarks.

## References

[1]: https://datatracker.ietf.org/doc/html/rfc4217 "RFC 4217: Securing FTP with TLS"

[2]: https://datatracker.ietf.org/doc/html/rfc3659 "RFC 3659: Extensions to FTP"

[3]: https://www.rfc-editor.org/info/rfc959 "RFC 959: File Transfer Protocol"

[4]: https://docs.openssl.org/3.1/man1/openssl-verification-options/ "OpenSSL certificate verification options"
