# M3 Status — Single-File Protected Data Transfer

## Scope

M3 turns the previously unavailable upload path into a real serialized single-file transfer. The public `ftp_upload_dir()` entry point accepts a regular local file directly, and the existing directory traversal path now delegates each discovered file to the real protocol operation. Full concurrent transfer scheduling, retries, resume, and post-upload chmod remain outside this single-file milestone.

## Delivered

The protocol engine now issues `TYPE I`, prefers EPSV, falls back to PASV when EPSV is rejected, opens the passive data socket, sends `STOR`, requires a 125/150 preliminary reply, streams the local file including zero-byte files, closes the data transport, and requires a 226/250 final reply before reporting success.

When the control session is explicit FTPS with `PROT P`, the passive data socket is transferred into a TLS transport and handshaken with the same server name, CA bundle, verification mode, and timeout policy as the control channel. Plain FTP uses the same passive sequencing with an unencrypted data transport.

The control worker now returns detailed reply code/message data for protocol operations and supports a receive-only final-transfer reply operation without allowing another command to interleave on the control socket. FTP 1xx preliminary replies are classified separately from 3xx intermediate replies.

`ftp_result_t` is populated with overall status, file totals, success/failure counts, byte totals, and an owned per-file result array. Each file result includes paths, status, bytes sent, `attempt_count = 1`, and `final_error`; `ftp_result_free()` releases all owned memory and zeroes the structure. The Python ctypes ABI test models the extended result layout.

## Validation

The clean Debug build passes all seven registered CTest tests, including the new plain PASV-fallback upload and explicit-FTPS EPSV protected zero-byte upload scenarios. The Python ABI suite passes 34 assertions, the Python exception suite passes 4/4 checks, and Python bytecode compilation succeeds. The clean ASan/UBSan CTest suite also passes all seven tests with leak detection enabled.

## Boundary for M4

The next milestone should harden multi-file orchestration: remote directory creation against real MKD replies, concurrency limits, retry/idempotency policy integration, resume semantics, progress throttling during actual writes, and failure recovery that preserves a reusable authenticated control session where the server permits it.
