# M4 Error Cycles

## MKD was still a stub

Nested directory uploads reached `TransferEngine::execute_mkdir_task()` but `ProtocolEngine::create_remote_dir()` returned invalid state without sending MKD. M4 now queues MKD through the authenticated control worker. A 550 MKD response is treated as a recoverable existing-directory condition so pre-existing paths do not poison the session.

## Retry read a stale final reply

When the data socket failed during a retryable write, the server had already queued the transfer’s 426 final reply. The next retry’s SIZE command consumed that stale reply and returned invalid state. M4 drains the receive-only final-transfer reply before resetting the state and starting the next SIZE/REST attempt.

## Python and C ABI option propagation

M4 now passes handle-level retry configuration and per-upload overrides into TransferConfig, while zero-valued upload options continue to select the configured defaults. Regular-file uploads now use TransferEngine as well, so they receive the same retry, resume, progress, and result semantics as directory uploads.

## Integration fixture callback crash

The initial M4 test passed a null callback user-data pointer while the callback unconditionally dereferenced it. The fixture now treats null user data as valid, matching the public callback contract.
