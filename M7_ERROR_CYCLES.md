# M7 Error Cycles

## TLS verification enum conversion

The new RETR data-channel path initially assigned the public integer verification mode directly to `TlsConfig::verify_mode`. The compiler correctly rejected the narrowing enum conversion. The download path now uses the same explicit `CertVerifyMode` cast as the upload path.

## Platform-specific OpenSSL target names

The Linux build links OpenSSL as `ssl` and `crypto`, while imported `OpenSSL::SSL` targets are created only by the non-Linux discovery branch in this project. The M7 download test target now follows the existing platform-specific linkage pattern.

## Safe local publication after RETR

A download now writes to a `.ftpclient.part` file, closes the data channel, validates the final 226/250 control reply, and only then publishes the destination path. Negative final replies remove the temporary file and leave no falsely successful destination file.

## Explicit FTPS data-channel coverage

The M7 fixture initially exercised only plaintext RETR. It now performs an explicit AUTH TLS control handshake, PBSZ 0, PROT P, TLS data-channel handshake, zero-byte transfer, and PASV fallback after EPSV rejection.

## Sanitizer CTest startup flake

One initial ASan CTest invocation timed out while starting the control integration test, but the same executable completed independently and the complete sanitizer suite passed on rerun. The rerun result is the authoritative validation evidence.

## Remote-operation boundary

Typed C++ and C ABI wrappers for CWD, CDUP, DELE, RMD, and RNFR/RNTO are implemented and compile through the public header. Dedicated integration coverage for these wrappers remains a follow-up hardening item; the validated M7 transfer fixture focuses on RETR and protected data-channel correctness.
