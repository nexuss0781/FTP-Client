# M5 Status — Independent Parallel FTPS Sessions

## Scope

M5 removes the single-session bottleneck for multi-file uploads by giving each worker task its own authenticated `ProtocolEngine`, control thread, and passive data-channel lifecycle. The existing base session remains responsible for traversal and remote-directory setup, while worker sessions perform file transfers independently.

## Delivered

The thread pool now invokes a real worker callback and preserves completion accounting when a task callback fails. TransferEngine schedules multiple files when `max_parallel` permits it, constructs worker sessions from the authenticated connection credentials and protocol timeout configuration, executes the complete M4 retry/resume upload path, and disconnects each worker session independently.

Public progress callbacks are serialized through a TransferEngine mutex because callbacks may not be thread-safe. Result aggregation remains thread-safe, and returned file results are sorted by remote path so completion timing cannot change the public order. A failure in one worker is recorded per file without corrupting sibling transfers or the base session.

The M4 regression fixture explicitly selects `max_parallel = 1` to preserve serialized compatibility coverage. The M5 fixture verifies overlapping transfers across independent sessions, file isolation, deterministic ordering, and one-worker failure isolation.

## Validation

The clean Debug build passes all nine registered CTest tests. The clean ASan/UBSan suite with leak detection also passes all nine tests. The Python ABI suite passes 37 assertions, the Python exception suite passes 4/4 checks, and Python bytecode compilation succeeds.

## Deliberate boundary

The current implementation creates an independent authenticated session for each worker task rather than maintaining long-lived per-thread sessions. This keeps ownership and teardown simple and safe. A future optimization may add reusable worker-session pooling, but it must preserve control-channel isolation and reconnect safely after a session-level protocol failure.
