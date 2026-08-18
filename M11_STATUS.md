# M11 Status: FEAT, MDTM, HASH, and Explicit Integrity Verification

**Project:** FTP-Client  
**Milestone:** M11  
**Branch:** `feature/ftps-m11-capability-integrity`  
**Author:** Manus AI  
**Status:** Ready for final commit and merge

## Executive status

M11 is functionally complete and validated. The library now discovers server capabilities through multiline FEAT parsing, caches the result for the authenticated session, exposes MDTM and HASH queries through the C ABI and Python bindings, and provides an explicit SHA-256 comparison between a local file and a server-side HASH response.

The implementation remains separate from Ahadu Deploy. It provides the transport and integrity contract that a future deployment orchestration layer can call, without assuming a hosting provider, server runtime, or deployment policy.

## Delivered scope

| Area | Delivered result |
| --- | --- |
| Reply model | Added complete multiline reply preservation while retaining existing final-line compatibility. |
| State machine | Added authenticated `HASH` command support. |
| Capability negotiation | Added `ServerCapabilities`, FEAT token parsing, algorithm flags, session cache, and cache reset. |
| Remote metadata | Added MDTM retrieval with bounded C output. |
| Remote integrity | Added HASH retrieval with algorithm advertisement checks and hexadecimal digest validation. |
| Local-versus-remote decision | Added SHA-256 comparison returning `FTP_ERR_INTEGRITY` on mismatch. |
| C ABI | Added `ftp_server_capabilities_t`, HASH constants, four M11 functions, struct-size gating, and Windows export coverage. |
| Python | Added cffi wrappers, high-level methods, immutable capability dataclass properties, async methods, stubs, ABI checks, and type coverage. |
| Tests | Expanded M11 loopback integration and Python ABI suites. |

## Validation matrix

| Validation | Result |
| --- | --- |
| Clean Debug configure/build | Passed with `make clean` and the requested `make all CFLAGS="-ferror-limit=100 -Wno-unused-parameter"`. |
| Debug CTest | **13/13 passed**. |
| Python ctypes ABI suite | **54 passed, 0 failed**. |
| Python exception suite | **4/4 passed**. |
| Python type suite | **7/7 passed**. |
| Python bytecode compilation | Passed for all modified binding and test modules. |
| Clean ASan/UBSan configure/build | Passed. |
| ASan/UBSan CTest | **13/13 passed**, with no sanitizer diagnostics. |
| Git whitespace check | Passed. |

## Commit lineage

The feature branch currently contains the original M11 implementation commit `ed6faa8` (`Add M11 FEAT and HASH support`) and the binding commit `ea52601` (`Expose M11 server integrity bindings`). The final implementation patch adds explicit local-versus-remote verification, its tests, and the three milestone records. It is ready to be committed as the final M11 change before merging into `main`.

## Known intentional boundary

Local verification currently supports SHA-256 only because that is the established local streaming integrity primitive. Server-advertised MD5, SHA-1, and SHA-512 remain discoverable through capability flags and retrievable through HASH when advertised, but the local comparison API rejects algorithms without a matching local implementation using `FTP_ERR_NOT_IMPLEMENTED`.

## Handoff to M12

M12 should build on this stable capability and integrity contract by adding provenance-safe per-file result metadata, integrating optional server-side HASH verification into the RETR download path, and designing parallel directory scheduling around the stable per-file manifest. The M12 design should preserve the distinction between local digest, server HASH, and comparison outcome in both C and Python result objects.
