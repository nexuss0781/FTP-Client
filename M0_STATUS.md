# Milestone 0 Status

**Branch:** `feature/ftps-m0-baseline`  
**Status:** Complete for the M0 scope  
**Latest published commit:** `a32d35a`

## Implemented

The repository now reports version `0.1.0` / `0x00010000`, returns zero public capability flags until features are end-to-end wired, and exposes the additive `FTP_ERR_NOT_IMPLEMENTED` status for valid operations that are not available in M0. The public connection path validates inputs but does not store credentials, claim a connection, or transition state. Valid upload requests return the explicit unavailable status and initialize their result structure.

The README and public header now identify the project as a development baseline rather than a production release. Generated CMake outputs, Python bytecode, egg-info, and copied native binding libraries were removed from version control. CTest registration was added, sanitizers became opt-in through `FTP_ENABLE_SANITIZERS=ON`, OpenSSL deprecation warnings were removed, and the Python binding now exposes `FTPNotImplementedError` with corrected error-band mapping.

## Validation

The normal Debug build completed from a clean build directory with CMake and Make. CTest passed all four registered tests: `abi_test`, `header_purity_test`, `phase2_test`, and `phase5_test`. The Python ABI test passed with 32 tests and zero failures. The Python exception test passed 4/4. The dedicated ASan/UBSan build and CTest suite also passed all four tests.

## Explicit M0 boundary

M0 does not implement a real FTP or FTPS connection, `NOOP`, `STOR`, `RETR`, or directory transfer. Those are the next milestone. M0 makes that limitation visible and machine-detectable instead of presenting simulated behavior as production capability.
