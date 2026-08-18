# M13 Status: Richer Verification and Retry Policies

**Project:** FTP-Client  
**Milestone:** M13  
**Branch:** `feature/ftps-m13-verification-retry-policies`  
**Author:** Manus AI  
**Status:** Implementation and validation complete; ready for final documentation commit and merge

## Executive status

M13 is functionally complete. Verification is now selected through explicit policy modes, while retry behavior is configurable by error category, elapsed-time budget, jitter factor, and retry-all intent. The controls are available consistently for uploads, single-file downloads, and M12 parallel directory downloads.

The implementation preserves previous behavior by default. Older option structures remain valid through `struct_size` gating, stalled and cancelled transfers remain non-retryable by default, and M12 boolean verification fields continue to map to equivalent policy modes.

## Delivered scope

| Area | Delivered result |
| --- | --- |
| Verification policy | Added `NONE`, `LOCAL_EXPECTED`, `REMOTE_OPTIONAL`, `REMOTE_REQUIRED`, and `LOCAL_AND_REMOTE`. |
| Algorithm policy | Added explicit algorithm selection with SHA-256 validation and `FTP_ERR_NOT_IMPLEMENTED` for unsupported local comparison. |
| Retry categories | Added network, server, ambiguous, authentication, protocol, and local category masks. |
| Retry budget | Added monotonic maximum elapsed-time budget and delay clamping. |
| Retry randomness | Added configurable jitter factor from deterministic maximum delay to full jitter. |
| Retry-all | Added explicit opt-in retry of permanent categories. |
| Result metadata | Preserved actual attempt counts through native, C ABI, and Python results. |
| C ABI | Added tail-extended policy and retry fields to upload/download options with size guards. |
| Python | Added dataclass fields, validation, top-level constants, cffi wiring, and type-stub declarations. |
| Tests | Added M13 retry-policy unit coverage and upgraded M12 integration coverage for explicit policy fields. |

## Validation matrix

| Validation | Result |
| --- | --- |
| Clean Debug configure/build | Passed after `make clean` with the requested Make flags. |
| Debug CTest | **15/15 passed**. |
| Python ctypes ABI suite | **58 passed, 0 failed**. |
| Python exception suite | **4/4 passed**. |
| Python type suite | **8/8 passed**. |
| Python bytecode compilation | Passed for modified bindings and tests. |
| Clean ASan/UBSan configure/build | Passed. |
| ASan/UBSan CTest | **15/15 passed**, including M13 policy coverage, with no sanitizer diagnostics. |
| Git whitespace check | Passed before documentation-only changes. |

## Commit lineage

The feature branch contains M12 main history and the following M13 checkpoints:

| Commit | Description |
| --- | --- |
| `e4c8b42` | Add M13 verification and retry policy controls. |
| `cbf9a62` | Preserve non-retryable stalled transfer behavior. |
| `b993f5e` | Expose M13 policy controls to C ABI and Python. |

The final documentation commit is pending. After it is pushed, the branch will be merged into `main` with a non-fast-forward merge and the final status record will be corrected to reference the merge commit.

## Intentional boundaries

M13 supports SHA-256 for local computation and local-versus-remote comparison. The server may advertise other HASH algorithms through M11, but M13 does not claim a local implementation for algorithms that lack a matching integrity helper.

`REMOTE_OPTIONAL` deliberately permits publication when the server cannot provide HASH, but it records `UNAVAILABLE` provenance. Applications requiring a verified artifact must use `REMOTE_REQUIRED` or `LOCAL_AND_REMOTE` instead.

## Handoff

After M13, the remaining production work is release and operational hardening: structured event telemetry, packaging and distribution automation, portability validation across supported compilers and platforms, and final Ahadu Deploy integration planning.
