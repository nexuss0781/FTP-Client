# M5 Error Cycles

## Existing M4 fixture accepted only one control session

Enabling default parallel scheduling caused the M4 mock server to accept only its original single connection, so the regression fixture timed out while worker sessions connected. The M4 fixture now explicitly sets `max_parallel = 1`, preserving its serialized compatibility coverage while the new M5 fixture exercises independent sessions.

## Worker sessions are per-task, not shared across the base session

With two workers and three files, the first two tasks opened two worker sessions and the third task needed a fresh session after a worker became available. The M5 server capacity was adjusted to model this lifecycle accurately. The production boundary is explicit: the base session is never shared with worker transfers.

## Rejected STOR stopped the control thread before QUIT

A 550 STOR response was classified as fatal by the control worker. `ProtocolEngine` attempted to reset the state afterward, but the worker loop had already stopped, so the session’s QUIT future could not complete. STOR errors are now recoverable at the control-thread level; the command still returns the mapped failure code, but the worker can disconnect cleanly.

## Failure test inherited default retries

An upload option value of zero means use the configured default retry count, not disable retries. The M5 failure-isolation test now calls `ftp_set_retry_policy(handle, 0, 0, 0)` explicitly before asserting one deterministic failed attempt.

## Parallel result ordering

Worker completion order is nondeterministic. `ResultAggregator::get_results()` now returns a sorted copy by remote path, keeping the public result sequence deterministic without serializing worker completion.
