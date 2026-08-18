# M3 Design Notes — Single-File Protected Data Transfer

## Current seams

`ProtocolEngine::upload_file()` is still a placeholder, while `TransferEngine::execute_upload_task()` simulates a local read and reports success without opening a data channel. `ftp_upload_dir()` validates paths but always returns `FTP_ERR_NOT_IMPLEMENTED`.

`DataChannel` already parses PASV and EPSV replies, including private-address substitution for PASV, but it does not create transports. `ControlThread` serializes commands and returns only status codes; it must expose the raw 227/229 reply text and the 150/125 preliminary plus 226/250 final reply sequence to the protocol engine.

`StateMachine` currently treats passive negotiation and STOR as separate transitions, accepts a data command too early, and does not model the final reply reliably. M3 should either extend those transitions or keep the sequencing invariant inside a dedicated protocol-engine upload operation while preserving authenticated state after successful completion.

## M3 slice

The initial slice will support one file at a time through the existing public directory-upload entry point. Directory traversal and remote-directory creation remain in the existing transfer orchestration, but each file will use a real passive data connection and STOR. Concurrency and retry policies remain disabled until the single-file path has deterministic result accounting.

For plain FTP, the data socket is a `PlainTransport`. For explicit FTPS after `PROT P`, the data socket is wrapped in a new `TlsTransport` using the same CA bundle, verification mode, server name, and timeout policy as the control channel. The passive command is EPSV first with PASV fallback where appropriate; the EPSV result uses the control host, while PASV uses the parsed address subject to existing NAT policy.

The command flow is `TYPE I`, passive negotiation, `STOR <remote>`, wait for 125/150, stream the local file including zero-byte files, close the data transport, then require 226/250 before recording success. Any failed preliminary or final reply records the file error and leaves the control session reusable when the server permits it.

## ABI boundary

M3 will populate `ftp_result_t` and its owned `ftp_file_result_t` array, including one entry per attempted file, total/success/failure counts, byte totals, `attempt_count = 1`, and `final_error`. `ftp_result_free()` remains the release path. The public capability mask will not advertise directory transfer until the end-to-end entry point is implemented and tested.
