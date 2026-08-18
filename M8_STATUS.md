# M8 Status — Integrity, Cancellation, and Stall Protection

## Delivered

M8 adds SHA-256 integrity verification to the active transfer paths. Upload options can carry an expected digest; the source digest is validated before transfer and rechecked after a positive final server reply. Extended downloads use `ftp_download_file_ex()` and verify the temporary RETR output before publication. Mismatches return `FTP_ERR_INTEGRITY` and do not publish a falsely trusted file.

The existing upload retry path now supports an opt-in metadata-safe mode. It refuses to resume an unrelated pre-existing remote object on the first attempt, checks local source size and modification time before every retry, and enables SIZE/REST only on retries created by the same operation. Legacy resume behavior remains available for compatibility.

M8 adds `ftp_cancel()` and `ftp_clear_cancel()` with a shared atomic token propagated to pooled worker sessions. Plain and TLS data sockets now distinguish deadline expirations as `FTP_ERR_TIMEOUT`. Configured stall deadlines produce `FTP_ERR_STALLED`, while cancellation produces `FTP_ERR_CANCELLED`. Both termination paths reset the control session to prevent a pending final transfer reply from contaminating later commands.

The Python bindings now expose M8 error names, `DownloadOptions`, `DownloadResult`, `FTPClient.download_file()`, `FTPClient.cancel()`, `FTPClient.clear_cancel()`, and the new upload option fields. Public C and Python ABI tests validate the new symbols and capability bit.

## Validation

The clean Debug build passes all ten CTest tests. The focused download fixture validates SHA-256 RETR verification, plain and explicit-FTPS RETR, EPSV/PASV behavior, negative final replies, distinct stall failure, and cooperative cancellation. The Python ABI suite passes 43 assertions, the Python exception suite passes 4/4 checks, the Python type suite passes 5/5 checks, and Python bytecode compilation succeeds.

The ASan/UBSan CTest log reports all ten tests passed in 36.29 seconds with leak detection enabled. The wrapper process did not print its final status line after CTest exited, but process inspection found no remaining CTest or test child and the captured log contains the complete 10/10 result.

## Scope boundary

M8 does not add cross-process durable resume manifests, remote-side HASH/MD5 negotiation, RETR REST resume, or directory download orchestration. Those require a portable remote metadata contract and remain candidates for a later milestone.
