# M7 Status — RETR Downloads and Remote Filesystem Wrappers

## Delivered

M7 adds a public `ftp_download_file()` C ABI operation backed by a real `ProtocolEngine::download_file()` implementation. The client sets binary mode, negotiates EPSV with PASV fallback, opens the passive data socket, upgrades it to TLS for explicit FTPS, requires a 125/150 RETR preliminary reply, streams bytes to a local temporary file, closes the data channel, validates the final 226/250 reply, and publishes the destination only after successful completion.

The implementation supports binary payloads, zero-byte downloads, nested local parent-directory creation, progress callbacks, exact byte accounting, result ownership, and negative-final-reply failure without falsely publishing a local file.

M7 also adds typed ProtocolEngine and C ABI wrappers for CWD, CDUP, DELE, RMD, and RNFR/RNTO. These operations remain serialized through the authenticated control thread and preserve the existing mapped error behavior.

## Validation

The clean Debug suite passes all ten registered CTest tests, including the new M7 download fixture. The ASan/UBSan suite with leak detection passes all ten tests on rerun. The Python ABI suite passes 37 assertions, the Python exception suite passes 4/4 checks, and Python bytecode compilation succeeds.

The integration fixture validates plain RETR, explicit-FTPS RETR with TLS data protection, EPSV-to-PASV fallback, zero-byte output, binary byte preservation, final-reply failure handling, result ownership, and local temporary-file cleanup.

## Follow-up boundary

Dedicated loopback integration coverage for the new remote filesystem C ABI wrappers remains a follow-up hardening task. Download-directory orchestration and machine-readable MLSD/LIST parsing are also not included in this milestone.
