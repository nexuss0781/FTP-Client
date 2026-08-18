# M5 Design Notes — Independent Parallel FTPS Sessions

## Constraint

One `ProtocolEngine` owns one control worker and one FTP state machine. Sharing it across multiple upload workers would interleave commands and data-channel ownership, so M5 will not attempt to make the existing session concurrently reentrant.

## Architecture

`TransferEngine` will keep the existing authenticated session for manifest traversal and remote-directory creation. Once directory setup completes, each upload worker will construct its own `ProtocolEngine`, copy the base session’s connection credentials and timeout configuration, connect and authenticate independently, execute one file at a time through the existing M4 upload/retry/resume path, and disconnect before accepting the next task.

The thread pool will receive a real worker callback. Its worker count remains bounded by the existing 1–16 clamp and the configured `max_parallel` value. The pool will track active tasks and notify completion exactly once even when a task callback raises an internal exception. The base control session remains untouched while worker sessions run.

## Result and callback semantics

Per-file result records will be collected thread-safely and returned in deterministic remote-path order rather than completion order. Aggregate counters remain atomic. Progress callbacks are invoked from worker sessions; callback invocation is serialized through a TransferEngine mutex because the public callback may not be thread-safe.

A cancellation flag will prevent new work from starting after a fatal orchestration condition, while already-running worker sessions finish their current protocol operation and disconnect. Retry and resume remain per-file and use the M4 implementation unchanged.

## Test boundary

M5 tests will verify that two or more workers overlap on independent control sessions, that each file remains isolated, that results are deterministic despite varied completion times, that one worker failure does not corrupt another session, and that `max_parallel = 1` preserves M4 behavior.
