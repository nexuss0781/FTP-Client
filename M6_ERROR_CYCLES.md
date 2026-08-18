# M6 Error Cycles

## Worker callback lacked stable identity

M5’s callback received only a task, so a worker could not safely own a reusable session. M6 propagates a stable worker ID from ThreadPool construction through callback invocation, allowing each worker to access exactly one session slot.

## Session destruction order

Reusable sessions must not be destroyed while ThreadPool workers are still active. The session vector is declared before ThreadPool so C++ destruction order stops and joins workers before destroying their ProtocolEngine objects.

## Regression fixture expected one new connection per task

M5’s connection-count assertion modeled fresh sessions for every task. After pooling, the test correctly expects one base session plus one reusable session per worker, while preserving the same overlap and isolation checks.

## Failed sessions must not be reused

A worker that receives a connect failure or a transfer error now disconnects and resets its pooled ProtocolEngine slot. The next task assigned to that worker creates a fresh authenticated session. The M6 fixture verifies a failed file followed by a successful queued file.
