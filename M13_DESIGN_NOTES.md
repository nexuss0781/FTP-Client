# M13 Design Notes: Richer Verification and Retry Policies

**Project:** FTP-Client  
**Milestone:** M13  
**Branch:** `feature/ftps-m13-verification-retry-policies`  
**Author:** Manus AI  
**Status:** Implementation and validation complete; final commit and merge pending

## Scope

M13 builds on M12’s provenance metadata and optional server HASH support. It introduces explicit verification policy modes instead of forcing callers to combine legacy booleans, and it extends retry behavior with category selection, bounded elapsed time, configurable jitter, and explicit retry-all behavior. All public additions are tail extensions guarded by `struct_size`.

The default policy remains conservative. Existing M12 callers continue to work: an expected SHA-256 digest selects local verification, `verify_remote_hash` selects optional remote verification, and both together select local-and-remote comparison. Existing retry defaults retain the transient network, transient server, and ambiguous categories while leaving authentication, protocol, local, integrity, cancellation, and stalled-transfer failures non-retryable.

## Verification policy modes

| Mode | Local digest | Expected digest | Remote HASH | Failure behavior |
| --- | --- | --- | --- | --- |
| `NONE` | No | No | No | Publish after normal RETR completion. |
| `LOCAL_EXPECTED` | Yes | Required | No | Mismatch returns `FTP_ERR_INTEGRITY`; missing expected digest is invalid. |
| `REMOTE_OPTIONAL` | No | Optional | Best effort | HASH unavailability records `UNAVAILABLE` but permits publication. |
| `REMOTE_REQUIRED` | No | No | Required | HASH unavailability returns its underlying error and prevents publication. |
| `LOCAL_AND_REMOTE` | Yes | Optional | Required | The local digest must equal the remote HASH value; mismatch returns `FTP_ERR_INTEGRITY`. |

The requested algorithm is normalized case-insensitively. M13 currently supports SHA-256 for local computation and local-versus-remote comparison. An unsupported algorithm is rejected with `FTP_ERR_NOT_IMPLEMENTED` and recorded as unavailable rather than being silently downgraded.

M12’s boolean compatibility is translated when the explicit policy is `NONE`: expected digest plus `verify_remote_hash` maps to `LOCAL_AND_REMOTE`, expected digest alone maps to `LOCAL_EXPECTED`, and the boolean alone maps to `REMOTE_OPTIONAL`. This preserves existing applications while giving new callers a deterministic policy contract.

## Retry policy controls

The retry engine now accepts a category bitmask, an optional maximum elapsed-time budget, a jitter factor, and a retry-all switch. The category mask can independently select network, server, ambiguous, authentication, protocol, or local classes. The default mask remains network plus server plus ambiguous.

The elapsed budget is measured with a monotonic clock from the first attempt. Before sleeping, the policy clamps the randomized delay to the remaining budget. A zero budget preserves the unlimited-time behavior of earlier milestones. A zero jitter factor produces deterministic maximum delay, while one produces the existing full-jitter range; intermediate values scale the random range proportionally.

`retry_all_errors` is explicit opt-in behavior. It can retry permanent categories, but it does not retry a successful result. Stalled and cancelled transfers remain non-retryable by default so M8 behavior is preserved; callers can deliberately opt into broader behavior through retry-all.

The attempt count is carried through `Task`, `ResultAggregator`, C ABI `ftp_file_result_t`, and Python `FileResult`, allowing callers to distinguish first-attempt success from policy-driven recovery.

## Native integration

Upload tasks build `RetryConfig` from `TransferConfig` and use it around the existing upload operation. Download tasks now use the same policy wrapper, including single-file C ABI downloads and the M12 parallel directory scheduler. Each retry resets per-attempt byte and verification state while preserving the stable task identity and result path.

The protocol engine validates the selected verification mode after the temporary file has completed. Remote-required failures remove the temporary artifact. Optional remote failures preserve the local artifact but expose `UNAVAILABLE` provenance so callers do not mistake policy degradation for a verified pass.

## ABI and Python surface

The C ABI adds M13 fields to `ftp_upload_options_t` and `ftp_download_options_t`: retry maximum delay, retry elapsed budget, category mask, jitter factor, and retry-all behavior. Download options also add `verification_policy` and `verification_algorithm`. Existing callers that provide an older `struct_size` do not read or write these fields.

Python adds matching frozen-dataclass fields with validation, top-level constants for policy modes and retry categories, cffi construction, and updated type stubs. The async facade automatically benefits because it delegates to the synchronous client methods.

## Test strategy

The M13 policy test exercises network recovery, category filtering, retry-all behavior, category-mask mapping, and elapsed-budget handling without introducing long sleeps. The M12 loopback integration test now requests `LOCAL_AND_REMOTE` verification, supplies SHA-256, configures retry fields, and continues to validate parallel RETR execution and complete provenance metadata.
