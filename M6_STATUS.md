# M6 Status — Reusable Worker-Session Pooling

## Scope

M6 upgrades M5’s independent per-task sessions into reusable per-worker ProtocolEngine sessions. Each ThreadPool worker receives a stable ID and owns one session slot that remains authenticated across multiple queued files when transfers succeed.

## Delivered

ThreadPool now propagates worker identity through its callback. TransferEngine allocates one session slot per worker, copies the base connection configuration into each ProtocolEngine, connects lazily on the worker’s first task, and reuses the authenticated session for subsequent tasks assigned to that worker.

Any connection failure, transfer failure, or unexpected worker exception disconnects and resets that worker’s slot. The next task assigned to the worker creates a fresh session. Session storage is declared before ThreadPool so worker threads are joined before their ProtocolEngine objects are destroyed.

The M5 concurrency and isolation fixture now verifies reduced connection count under pooling and adds a post-failure queued file to validate reconnect behavior. Existing M0–M5 tests remain part of the regression suite.

## Deliberate boundary

The pool is scoped to one `TransferEngine` invocation. Sessions are intentionally not retained across separate public upload calls, because credentials, timeout configuration, and server state may change between calls. Cross-invocation pooling can be considered only after explicit invalidation and credential-rotation rules are defined.
