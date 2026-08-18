# M0 Error Cycles and Resolutions

## `tests/abi_test.c`

The first targeted edit did not apply because the expected connection-test comment differed from the current file. The file was reread, the exact blocks were located, and the edit was reapplied atomically. The test now expects `FTP_ERR_NOT_IMPLEMENTED` for valid connection and upload requests in M0.

## Capability search

An initial repository search used a file path as a content-search scope and was rejected because the search scope must be a directory glob. The search was rerun against `/home/ubuntu/FTP-Client/include/*.h` and completed successfully.

## Generated artifacts

The baseline contained tracked CMake outputs, shared libraries, Python bytecode, egg-info, and a copied binding library. These were removed from version control and added to ignore rules. They remain reproducible build outputs rather than source files.

## M0 implementation decisions

The public connection path no longer stores credentials or transitions to `CONNECTED` when no real session exists. Valid but unavailable operations return the new additive `FTP_ERR_NOT_IMPLEMENTED` status. Capability introspection returns zero until a capability is wired into and tested through the exported execution path.

## Clean build tool

The first required clean-build command failed because the tracked build directory referenced `/usr/bin/cmake`, which was absent in the environment. The stale build could not execute its generated clean target. CMake was installed, and the build will now be recreated from source rather than trusting the stale directory.

## C++ toolchain

The recreated CMake configuration detected GCC C but could not find a C++ compiler, so configuration stopped before compilation. The next recovery step is to install the standard GNU C++ toolchain, then rerun configuration from the clean build directory.

## OpenSSL development headers

The compiler reached the TLS source but failed because `openssl/ssl.h` was not installed. This is an environment dependency failure, not yet a source-code failure. The M0 build requires the OpenSSL development package before compilation can continue.

## CTest: phase5 stall detector

The clean build completed and ABI, header-purity, and phase2 tests passed. The pre-existing `phase5_test` aborted in `test_stall_detector_stall()` because the expected stall state was not reached within its timing assumption. This is unrelated to the M0 API changes but blocks a fully green baseline. The next step is to inspect the detector clock/test timing and fix the test or implementation without weakening the intended stall semantics.

## Stall detector test timing

The test configured `minimum_bps=1024` while the implementation estimates a 256 KiB buffer and applies a 3x multiplier, producing a threshold of several minutes rather than one second. The test was corrected to use 1 MiB/s, which yields a 0.75-second calculated threshold bounded by the configured one-second minimum. Production stall-detector logic was not changed.

## Python ABI test and sanitizers

The C ABI test passed under CTest, but loading the Debug shared library from Python failed because CMake unconditionally linked AddressSanitizer into the shared library; Python reported that the ASan runtime was not first in the library list. Sanitizers are now opt-in through `FTP_ENABLE_SANITIZERS=ON`, preserving a normal Debug build for ABI/binding tests and a separate sanitizer build for memory validation.

## Python exception range mapping

The new exception regression test exposed an existing boundary bug: `-301` was mapped to `FTPNetworkError` because the range condition was written in the wrong direction. The mapping will be corrected to explicit inclusive error bands for authentication, network, protocol, I/O, configuration, and system errors.
