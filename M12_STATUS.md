# M12 Status: Provenance-Safe Verification and Parallel Downloads

**Project:** FTP-Client  
**Milestone:** M12  
**Branch:** `feature/ftps-m12-provenance-parallel`  
**Author:** Manus AI  
**Status:** Implementation and validation complete; ready for final commit and merge

## Executive status

M12 is functionally complete. The library now carries explicit per-file verification provenance through the native transfer engine, C ABI, and Python bindings. Remote SHA-256 verification is available as an opt-in RETR feature, and directory downloads can schedule a stable, path-validated manifest across independent authenticated sessions with bounded parallelism.

The default behavior remains compatible with M11 and earlier milestones: no server HASH command is issued unless requested, directory downloads remain serial unless `max_parallel` is greater than one, and upload results report no verification rather than inventing provenance.

## Delivered scope

| Area | Delivered result |
| --- | --- |
| Internal result model | Added `VerificationMetadata` to tasks and aggregated per-file results. |
| C ABI result contract | Added verification status, source bitmask, algorithm, local digest, and remote digest fields with owned-string cleanup. |
| Download options | Added tail-extended `verify_remote_hash` and `max_parallel` controls guarded by `struct_size`. |
| RETR verification | Added local SHA-256 calculation, optional server HASH retrieval, publication gating, and explicit unavailable/mismatch outcomes. |
| Directory scheduling | Added deterministic MLSD manifest collection and bounded parallel file tasks with independent worker sessions. |
| Result stability | Preserved remote-path sorting, path containment checks, cancellation, resume controls, and M9 symlink error reporting. |
| Python | Added `VerificationMetadata`, `FileResult.verification`, option fields, cffi decoding, type stubs, and tests. |
| Integration test | Added M12 loopback fixture proving HASH verification, provenance, and concurrent RETR execution. |

## Validation matrix

| Validation | Result |
| --- | --- |
| Clean Debug configure/build | Passed after `make clean` with the requested Make flags. |
| Debug CTest | **14/14 passed**. |
| Python ctypes ABI suite | **56 passed, 0 failed**. |
| Python exception suite | **4/4 passed**. |
| Python type suite | **8/8 passed**. |
| Python bytecode compilation | Passed for modified bindings and tests. |
| Clean ASan/UBSan configure/build | Passed. |
| ASan/UBSan CTest | **14/14 passed**, including M12 parallel downloads, with no sanitizer diagnostics. |
| Git whitespace check | Passed before the current documentation-only changes. |

## Commit lineage

The feature branch currently contains M11’s merged main history and the M12 core checkpoint `058504e` (`Add M12 verification provenance and download scheduler`). The final documentation, integration-test registration, ABI assertions, and milestone records are pending the final commit. The branch remains separate from `main` until all final records are committed and pushed.

## Known intentional boundaries

M12 performs local-versus-remote comparison with SHA-256 only because that is the established streaming local integrity primitive. The M11 capability layer can report other server algorithms, but M12 does not claim local comparison support for algorithms without a matching local implementation.

The scheduler parallelizes file transfers after manifest collection; it intentionally does not parallelize MLSD traversal. This preserves deterministic path validation and ensures that symlink and root-containment checks complete before any worker publishes files.

## Handoff to M13

M13 should focus on production deployment ergonomics: richer retry and verification policy controls, optional per-file server algorithm selection, structured event telemetry for verification outcomes, and packaging/release automation for the C ABI and Python bindings. Any new policy should preserve M12’s explicit provenance fields rather than collapsing remote, local, and unavailable outcomes into a single status.
