# M4 Status — Multi-File Orchestration Hardening

## Scope

M4 hardens the M3 upload path for multi-file directory deployment while preserving the single-control-session serialization model. The local directory manifest is now executed rather than simulated, remote directories are created through MKD, and regular-file uploads use the same orchestration path as directory uploads.

## Delivered

`ProtocolEngine::create_remote_dir()` now queues MKD through the authenticated control worker. Existing-directory responses are treated as recoverable under the current policy, allowing nested directory creation to proceed deterministically.

Transfer configuration now carries retry attempts, exponential-backoff settings, resume selection, directory-creation policy, buffer size, and callback behavior. Each file executes through the existing retry policy. Retryable data failures drain the pending final transfer reply before the next attempt, preventing stale 426 responses from corrupting the next SIZE/REST command sequence.

Conservative resume support queries SIZE, accepts only a remote size smaller than the local file, issues REST at that confirmed offset, and streams the remaining bytes. Equal-or-larger remote files restart from zero rather than blindly appending. Progress callbacks are emitted from actual data writes and receive a guaranteed final callback.

The public capability mask now advertises tested plain passive data transfer, explicit-FTPS protected passive data transfer, and REST resume support. C and Python ABI tests validate the expanded mask and result behavior.

## Validation

The clean Debug build passes all eight registered CTest tests, including the M4 nested-directory/multi-file fixture and retry/resume/progress fixture. The Python ABI suite passes 37 assertions, the Python exception suite passes 4/4 checks, and Python bytecode compilation succeeds. The clean ASan/UBSan CTest suite passes all eight tests with leak detection enabled.

## Deliberate boundary

The current handle owns one control session, so M4 keeps file execution serialized even when `max_parallel` is configured. True parallel uploads require a future multi-session architecture that creates independent control/data-channel pairs rather than sharing one state machine.
