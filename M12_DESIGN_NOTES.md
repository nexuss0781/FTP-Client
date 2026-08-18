# M12 Design Notes: Provenance-Safe Download Verification and Parallel Scheduling

**Project:** FTP-Client  
**Milestone:** M12  
**Branch:** `feature/ftps-m12-provenance-parallel`  
**Author:** Manus AI  
**Status:** Implementation and validation complete; final documentation commit pending

## Purpose and scope

M12 turns M11’s capability and HASH primitives into a usable download contract. The milestone adds per-file verification provenance to C++ results, the C ABI, and Python result objects; adds an opt-in server-side SHA-256 HASH check after a successful RETR; and replaces the previous serial directory-download execution path with a bounded scheduler backed by independent authenticated control sessions.

The default behavior remains conservative and backward compatible. Existing downloads do not issue HASH commands unless the caller explicitly sets `verify_remote_hash`. Existing directory downloads remain serial unless the caller requests a value greater than one through the tail-extended `max_parallel` option. Existing upload results carry empty verification metadata.

## Provenance model

Each per-file result carries a `VerificationMetadata` record with five fields. The status distinguishes no verification from a passed comparison, a failed comparison, and an unavailable verification attempt. The source bitmask records whether the local digest and/or remote digest participated. The algorithm and both digest values are retained as owned strings when available.

| Field | Meaning |
| --- | --- |
| `verification_status` | `NONE`, `PASSED`, `FAILED`, or `UNAVAILABLE`. |
| `verification_sources` | Bitmask containing `LOCAL` and `REMOTE` provenance bits. |
| `verification_algorithm` | Currently `SHA-256` when M12 verification is active. |
| `local_digest` | Digest calculated from the completed local `.part` file. |
| `remote_digest` | Digest returned and validated by the server HASH command. |

A caller can therefore distinguish four materially different outcomes: no check was requested; a local expected digest passed; local and remote digests both passed; or a requested remote check could not be completed because the server did not advertise HASH or returned an error. An integrity mismatch returns `FTP_ERR_INTEGRITY` and records `FAILED`; capability or protocol failures record `UNAVAILABLE` and preserve the underlying error code.

## RETR verification sequence

For an opted-in download, the protocol engine completes the data transfer and waits for the final positive transfer reply before hashing. It computes a streaming local SHA-256 digest from the temporary file. If the caller supplied an expected digest, that local value is checked first. When `verify_remote_hash` is enabled, the engine then queries the M11 HASH API for the remote path using SHA-256. Only after both comparisons pass is the temporary file published to its final path.

This ordering prevents a failed remote verification from publishing an untrusted artifact. The default path remains unchanged: without expected or remote verification, the file is published after the normal final RETR reply. Resume sidecar behavior continues to operate before final verification, and failed verification removes the temporary artifact rather than leaving a falsely published result.

## Parallel directory scheduling

The directory API still performs MLSD traversal serially to preserve deterministic path validation and parent-directory creation. It builds a stable `DownloadManifestEntry` vector containing local path, remote path, remote size when advertised, and any caller-provided expected digest. Once collection succeeds, `TransferEngine` schedules file tasks through the existing fixed-size thread pool.

Each worker owns an independent `ProtocolEngine` session. A worker creates and reuses its authenticated session for its assigned files, while the primary session remains responsible for manifest traversal. The scheduler defaults to one worker and accepts `max_parallel > 1` only when explicitly requested. Results are aggregated under the existing mutex-protected `ResultAggregator` and sorted by remote path, so parallel completion order cannot change the public result order.

Cancellation, local path containment, existing resume controls, and the prior symlink-escape behavior remain enforced. A collection-stage failure is reported through `ftp_result_t.status`, including the M9 symlink regression case.

## ABI and Python compatibility

The new download options are tail fields after the M10 fields and are read only when `struct_size` covers them. The per-file result extension adds owned strings after the existing attempt and error fields; `ftp_result_free()` releases all five owned string pointers. Zero-initialized result arrays therefore represent the legacy no-verification state safely.

Python mirrors the contract with `VerificationMetadata`, `FileResult.verification`, `DownloadOptions.verify_remote_hash`, and `DownloadOptions.max_parallel`. The async facade delegates the existing synchronous methods and requires no separate scheduling implementation. Ctypes ABI coverage now checks the new layout attributes, while the type suite covers passed/unavailable provenance and option validation.

## Validation design

The M12 loopback server advertises multiline FEAT with HASH SHA-256, serves two files through MLSD and RETR, returns deterministic SHA-256 HASH replies, and delays data delivery long enough to observe at least two concurrent RETR sessions. The integration test verifies payloads, two-file success accounting, local and remote provenance bits, matching digest values, SHA-256 algorithm metadata, and actual bounded parallelism.
