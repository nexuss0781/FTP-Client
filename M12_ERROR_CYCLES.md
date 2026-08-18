# M12 Error-Cycle Recap

**Project:** FTP-Client  
**Milestone:** M12  
**Branch:** `feature/ftps-m12-provenance-parallel`  
**Author:** Manus AI

## Cycle 1: Provenance was not preserved through result aggregation

The M11 download path could verify a local or remote digest but returned only a status code and byte count. That left callers unable to distinguish a passed local expected digest from a passed local-plus-remote comparison, or to understand why a requested remote check was unavailable.

The resolution was a shared `VerificationMetadata` value carried by download tasks, `ResultAggregator::FileResult`, `ftp_file_result_t`, and Python `FileResult`. Owned C strings are copied at the ABI boundary and released by `ftp_result_free()`. Upload results remain valid because their zero-initialized metadata represents `FTP_VERIFY_STATUS_NONE`.

## Cycle 2: Server HASH had to remain opt-in

Automatically probing FEAT and issuing HASH after every RETR would add control traffic and could change behavior for existing callers. The resolution was a tail-extended `verify_remote_hash` option. Only callers that set the field and provide a struct size covering it request server-side SHA-256 verification. A server that does not advertise HASH produces `FTP_VERIFY_STATUS_UNAVAILABLE` with the underlying capability error rather than a false pass.

## Cycle 3: Directory downloads were serial and bypassed the stable manifest

The previous `ftp_download_dir` implementation recursively listed and downloaded each file immediately. That made parallel scheduling impossible and prevented result aggregation from sharing the same per-file metadata path as uploads.

The resolution was to keep traversal and path-safety checks serial, collect a deterministic `DownloadManifestEntry` vector, and schedule only the completed file manifest through `TransferEngine`. Each worker has an independent authenticated session, and the result aggregator sorts by remote path after parallel completion.

## Cycle 4: M9 symlink regression lost the failure status

The first scheduler refactor preserved the early return from path-containment validation but did not populate `ftp_result_t.status` when collection failed before the scheduler was constructed. The M9 integration test correctly caught this through its symlink-escape assertion.

The resolution was to populate the result status and zero counts for collection-stage failures before returning. The corrected full suite passed all 14 tests.

## Cycle 5: The first M12 FEAT-count assertion assumed one primary probe

The M12 integration fixture initially required at least three FEAT requests. The primary session performs MLSD traversal without a capability probe, while the two worker sessions each load their own session-scoped capability cache. The correct lower bound is therefore two FEAT requests for two workers.

The resolution was to assert at least two probes and retain the stronger behavioral assertion that each worker can independently negotiate HASH capability. This matches M11’s explicitly session-scoped cache design.

## Cycle 6: ABI and Python surfaces required synchronized tail extensions

Adding C fields without updating ctypes structures or Python option construction would allow native tests to pass while Python callers silently omitted M12 behavior. The resolution was to update the public header, cffi declaration extraction, Python option builders, result decoding, dataclasses, stubs, ctypes layouts, and type tests together.

The final Python ABI suite completed with **56 passed and 0 failed**, the exception suite with **4/4 passed**, and the type suite with **8/8 passed**.

## Cycle 7: Clean and sanitizer validation

A fresh Debug build was configured and cleaned in the canonical `build` directory, then all 14 CTest cases passed. A separate clean ASan/UBSan build also passed all 14 tests, including M12’s concurrent download fixture. No sanitizer diagnostics or ownership leaks were reported.

## Resolution summary

All implementation and test-cycle issues found during M12 were resolved. The remaining work is final documentation commit, feature-branch push, non-fast-forward merge into `main`, and post-merge repository verification.
