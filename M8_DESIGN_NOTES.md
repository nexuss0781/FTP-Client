# M8 Design Notes

## Integrity contract

M8 uses SHA-256 in the existing OpenSSL dependency. Upload callers may provide an expected digest through the extended upload-options tail; the source is hashed before transfer and rechecked after a successful final server reply. Download callers use `ftp_download_file_ex()` with a structured options object, and the temporary local file is hashed before it is published. Digest values accept upper- or lower-case hexadecimal and colon-separated presentation, but are normalized internally to lower-case 64-character hexadecimal.

A mismatch returns `FTP_ERR_INTEGRITY` and never reports a successful result. Integrity and cancellation failures are terminal to the retry policy; an aborted user operation must not be retried behind the caller’s back.

## Resume safety

Legacy `resume_enabled` behavior remains available for compatibility. When `resume_metadata_enabled` is set for an upload, the first attempt always starts at offset zero instead of trusting an unrelated pre-existing remote object. Subsequent retry attempts may query SIZE and use REST only after the same invocation has created a partial object. The local source size and modification timestamp are rechecked before each retry, and a changed source returns `FTP_ERR_INTEGRITY`.

This is deliberately scoped to the active upload invocation. Cross-process durable resume manifests and remote-side hash negotiation are reserved for a later milestone because the current public protocol surface does not yet provide a portable server-side fingerprint command.

## Cancellation and stalls

`ftp_cancel()` sets a shared atomic token used by the synchronous upload/download path and all pooled worker sessions. Data loops check the token between I/O operations. Data-channel socket deadlines are bounded by the configured stall timeout; timeout-style socket errors are classified as `FTP_ERR_TIMEOUT`. If no progress occurs through the configured stall deadline, the transfer returns `FTP_ERR_STALLED`. Cancellation and stall termination shut down and reset the control session so a pending transfer-final reply cannot contaminate the next command sequence; callers must reconnect before another transfer.

`ftp_clear_cancel()` clears the token. A new upload or extended download also clears it at the beginning of the operation.

## ABI compatibility

Existing `ftp_download_file()` remains unchanged and delegates to the new `ftp_download_file_ex()` operation. `ftp_upload_options_t` adds fields only at the end of its struct-size-governed layout. `ftp_download_options_t` begins with `struct_size`. The new capability bit is advertised only because the SHA-256 path is exercised in the integration suite.
