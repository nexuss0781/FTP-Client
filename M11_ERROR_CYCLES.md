# M11 Error-Cycle Recap

**Project:** FTP-Client  
**Milestone:** M11  
**Branch:** `feature/ftps-m11-capability-integrity`  
**Author:** Manus AI

## Cycle 1: Python ABI suite lagged behind the C ABI

The feature implementation added `ftp_server_capabilities_t` and new exported functions, but `tests/abi_test.py` initially had no ctypes representation for the capability structure and no M11 symbol assertions. This created a coverage gap: the C ABI could compile while the Python compatibility test remained unaware of the new contract.

The resolution was to add a size-accurate `FtpServerCapabilities` ctypes structure, configure signatures for capability, MDTM, HASH, and verification functions, and test null-handle and disconnected-state preconditions. The corrected ABI suite completed with **54 passed and 0 failed**.

## Cycle 2: Python surfaces needed parity with the synchronous API

The low-level cffi and synchronous `FTPClient` bindings covered capability discovery and remote metadata, but the async facade and type stubs also needed to expose the same public behavior. The resolution was to add the three M11 query methods and the explicit verification method to `_async.py`, `client.py`, and `__init__.pyi`. The immutable `ServerCapabilities` dataclass received coverage for SHA-256 and SHA-512 convenience properties.

The Python type suite completed with **7/7 passed**, the exception suite with **4/4 passed**, and bytecode compilation completed without errors.

## Cycle 3: Retrieval alone did not provide an explicit integrity decision

Remote HASH retrieval exposed a digest but did not itself express whether a local deployment artifact matched the server’s value. That would force callers to reimplement comparison logic and could lose provenance when reporting a mismatch.

The resolution was an explicit `ftp_verify_local_file_with_remote_hash` operation. It computes a local streaming SHA-256 digest, retrieves and validates the remote HASH response, normalizes both values, and returns `FTP_ERR_INTEGRITY` on mismatch. Unsupported local algorithms return `FTP_ERR_NOT_IMPLEMENTED` rather than applying an incorrect algorithm.

The M11 loopback integration test now covers matching content, mismatching content, and unsupported MD5 verification.

## Cycle 4: Capability parsing had to preserve multiline FEAT information

A normal single-line reply field is insufficient for multiline FEAT replies because feature lines would be lost before protocol-level parsing. The resolution was to preserve an owned `full_message` in the reply object and propagate it through `CommandReply` and `Command::reply_full_message`. The parser continues to retain the legacy final-line `message` field for existing callers.

The integration server confirms that SIZE, MDTM, HASH, SHA-256, and SHA-512 are detected from one multiline FEAT response and that the cached session performs only one FEAT probe.

## Cycle 5: Output buffers and ABI evolution required bounded writes

The new string-returning C functions needed to reject short buffers without truncation or an out-of-bounds write. The capability struct also needed to tolerate older callers that provide a smaller `struct_size`.

The resolution was a shared bounded string-copy helper and offset-based field gating for `ftp_server_capabilities_t`. The integration test rejects a four-byte HASH output buffer, while the ABI structure remains append-friendly.

## Cycle 6: Clean and sanitizer validation

A fresh Debug build was required because earlier build directories could mask stale objects. The repository was configured from scratch, cleaned, and rebuilt with the project’s requested Make flags. All **13/13 Debug CTest tests** passed. A separate ASan/UBSan build was then cleaned and tested; all **13/13 sanitizer CTest tests** passed, including the 60-second parallel-session test. No sanitizer diagnostics were emitted.

## Resolution summary

All identified M11 implementation and validation gaps are resolved. The remaining work is administrative: commit the final M11 patch and records, push the feature branch, merge with a non-fast-forward merge into `main`, and verify the resulting remote branch state.
