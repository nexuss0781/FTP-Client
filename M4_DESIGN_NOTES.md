# M4 Design Notes — Multi-File Orchestration Hardening

## Findings

The local directory walker already emits deterministic directory and file manifests. The remaining gaps are execution gaps: `ProtocolEngine::create_remote_dir()` is a stub, `ThreadPool` workers only decrement counters and never invoke transfer work, `TransferEngine` currently executes files synchronously, and retry settings stored on `FtpClientImpl` are not passed into transfer configuration.

The protocol engine owns one control thread and one authenticated control session. FTP command serialization and the state machine therefore make true concurrent uploads unsafe on one handle. M4 will use bounded serialized file execution by default and make `max_parallel` an execution-bound setting rather than pretending that multiple data channels can safely share one control socket. A future multi-session design can provide true parallelism without violating the current handle ownership model.

## Planned implementation

`MKD` will be implemented through the control command queue, treating 550/server-denied responses as an existing-directory continuation only where the configured policy allows it. File transfers will use bounded per-file retry attempts with the existing `RetryPolicy`, preserving attempt count and final error in the result aggregator.

Resume support will be conservative: query `SIZE`, accept only a valid remote size smaller than the local file, issue `REST`, and upload from the confirmed offset. A remote size equal to or larger than the local file will not be blindly appended; the initial M4 behavior will restart from zero unless a later protocol-specific completion policy explicitly proves the file is complete.

Progress callbacks will be emitted from the actual write loop rather than only at completion. The callback state will remain per-file and throttled to 10 Hz, with a final callback guaranteed for both success and failure.

## Test boundary

M4 tests will cover nested directory creation, multiple files, deterministic result counts, retryable failure followed by success, resume offset handling, callback delivery, and non-retryable failures. The existing M0–M3 control and protected data tests remain mandatory regressions.
