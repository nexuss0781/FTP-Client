# Ahadu Deploy: FTP Client Review and Node.js Deployment Plan

**Prepared by Manus AI**  
**Repository reviewed:** `nexuss0781/FTP-Client`  
**Reviewed commit:** `86954c907c30adc2af55be60db1346d8319f7e0e`  
**Purpose:** Establish a privacy-respecting, personal deployment platform that can transfer web and Node.js project artifacts reliably, while never assuming that an FTP destination can execute Node.js.

## Executive decision

The repository is a promising **FTP/FTPS library foundation**, but it is not yet a functioning deployment transport. The public documentation describes a production-grade client, whereas the current integration path still validates credentials, stores configuration, and returns stub results. The first engineering objective must therefore be to make one secure, observable, end-to-end file transfer work against a controlled FTP/FTPS server before adding deployment automation.

The most important hosting conclusion is that **FTP transfers files; it does not install Node.js, run `npm install`, start a process, expose a listening port, or create a process manager**. InfinityFree’s current public support information says its free hosting does not support Node.js, while its official Terms of Service prohibit using the server as an application server and impose restrictions on non-web files, file distribution, and resource-intensive use.[1] [2] Consequently, Ahadu Deploy should not promise “Node.js deployment to InfinityFree free hosting.” It should support InfinityFree only as a constrained **static/PHP-compatible web target**, unless the user verifies that a different paid provider and plan explicitly includes a Node.js runtime.

> **Strategic rule:** Ahadu Deploy may upload a Node.js project to a target, but it may call the project “deployed” as a running Node.js service only when the target adapter has verified a Node runtime, a start mechanism, environment configuration, and an externally reachable health endpoint.

## What the repository contains today

The codebase has meaningful groundwork. It defines an opaque C ABI, fixed-width types, explicit error categories, a Python `cffi` package, protocol parsing components, directory walking, retry-policy classes, circuit-breaker classes, and prebuilt phase tests. Those parts are valuable because they give Ahadu Deploy a possible native transport core that can later be reused by a CLI, a local desktop tool, or a service layer.

However, the working path is disconnected. In `src/ftpclient.cpp`, `ftp_connect()` validates the credential structure and stores it in `CredentialVault`, then changes the state to `CONNECTED`; it does not instantiate or invoke `ProtocolEngine`. `ftp_ping()` checks state and returns success without sending `NOOP`. Most critically, `ftp_upload_dir()` validates parameters and connected state, initializes a few result fields, and returns `FTP_ERR_INVALID_STATE`. These are not minor defects; they mean the public API cannot yet deploy files.

The protocol layer contains partial control-channel groundwork, but `ProtocolEngine::upload_file()`, `create_remote_dir()`, and `download_file()` are explicit stubs. The default transport factory returns `PlainTransport`, and `set_tls_mode()` only changes a boolean. The transfer layer traverses the local tree and sorts files, but it does not submit tasks to the thread pool. Its per-file worker reads local data and marks it as successfully transferred without opening an FTP data channel or sending it to a server. The result-filling method is empty.

The Python binding also needs correction before it can be the deployment interface. In `python/ftpclient/client.py`, the progress callback registration computes a callback `user_data` value but the C call passes `ffi.NULL`. The upload method does not apply `_check_error()` to the native return code and depends on result fields that the C implementation does not currently populate. These issues should be addressed after the native transfer path is real, not hidden by higher-level workarounds.

| Area | Current condition | Ahadu Deploy consequence |
|---|---|---|
| Public C ABI | Well-defined but partially aspirational | Preserve the ABI where possible, but revise capability reporting so it reflects real features |
| Connection | State transition and vault storage; no live network connection in the public path | Must wire a real control connection and authentication |
| TLS/FTPS | Security components exist, but public connection path does not use them | FTPS must be the default for deployment targets |
| Directory upload | Returns a stub error from the public API | This is the first blocking implementation milestone |
| Transfer engine | Reads files locally and simulates success | Must implement data channels, `STOR`, completion replies, and result aggregation |
| Concurrency | Thread-pool scaffolding exists but work is not submitted | Start with one reliable stream; add bounded concurrency after correctness |
| Resilience | Unit-level retry and circuit-breaker behavior exists | Integrate it around real connection and transfer errors |
| Python API | Useful shape, callback and error-handling defects | Make it a thin, tested wrapper over the corrected C ABI |
| Tests | Existing prebuilt tests pass, including phase tests | They prove parsing and stub contracts, not real deployment |
| Build reproducibility | Prebuilt artifacts exist; `cmake` is not available in this review environment | Add reproducible CI and remove reliance on checked-in build outputs |

## What was verified during review

The prebuilt `header_purity_test`, `phase2_test`, and `phase5_test` executables ran successfully. The phase tests reported 56 protocol and directory-walker checks passed, and the resilience test suite passed its retry, jitter, circuit-breaker, stall-detection, and idempotency checks. These results are useful, but they do not prove network interoperability. The ABI tests explicitly encode stub expectations: valid connection succeeds without network I/O, and upload returns `FTP_ERR_INVALID_STATE`.

A source rebuild could not be reproduced in this sandbox because the `cmake` executable is not installed. This is an environment limitation rather than evidence that the project cannot build. The repository should nevertheless add a clean CI build from an empty checkout, with compiler, OpenSSL, zlib, and test dependencies declared explicitly.

The credential vault currently copies secrets into `malloc`-allocated buffers, zeroes them using the string length, and frees them. That is better than leaving credentials in ordinary long-lived application objects, but it does not yet meet the stronger documentation claims around locked memory, core-dump exclusion, and guaranteed length-aware cleansing. The implementation should use a length-tracking secure buffer, best-effort page locking, explicit zeroization independent of string termination, and clear documentation of what is and is not guaranteed on each operating system.

## Hosting and privacy boundary

InfinityFree’s recent support discussion states that the free service does not support Node.js; the discussion distinguishes it from paid iFastNet hosting, which is a separate service.[1] The official Terms of Service state that scripts must produce web-based content rather than use the server as an application server. The same terms restrict backups, downloads, file sharing, file distribution, and excessive resource consumption.[2] Ahadu Deploy must treat these restrictions as a hard product boundary, not as an obstacle to bypass.

InfinityFree’s Privacy Policy states that free website hosting is fulfilled by iFastNet Ltd. and that InfinityFree transmits the account email address and IP address to iFastNet for service fulfillment.[3] This does not mean the user cannot use the service; it means Ahadu Deploy should be deliberately data-minimizing. The client should upload only files selected by the deployment manifest, never upload `.env` files or private keys by default, avoid logging file contents, redact passwords and tokens, and make the user explicitly approve every target profile.

| Target profile | What Ahadu Deploy may do | What it must not claim |
|---|---|---|
| InfinityFree free | Upload an approved static/PHP web tree over the provider-supported FTP/FTPS connection, after a dry run and policy check | A running Node.js service, background worker, custom port, or server-side `npm install` |
| Verified Node-capable shared host | Transfer only if the plan exposes a documented Node runtime and start/restart mechanism | Runtime support based only on the presence of FTP credentials |
| VPS or dedicated Linux host | Prefer SSH/SFTP or a provider API; upload artifact, install dependencies, configure environment, restart service, and run health checks | An FTP-only workflow for operations that require process control |
| Managed Node platform | Use the provider’s deployment API or CLI adapter when available | Treat generic FTP as an equivalent to the platform’s build and release system |
| Local or staging FTP server | Use as the first integration-test target for the native library | Infer public-host compatibility from a local mock alone |

## Proposed Ahadu Deploy architecture

Ahadu Deploy should be built as a **target-aware release pipeline**, not as a generic “copy this folder” command. Its central object is a deployment manifest describing the source project, build output, excluded paths, target capability profile, transfer policy, verification policy, and release identifier.

```text
Project source
    |
    v
Scanner and policy engine ----> rejects secrets, unsafe paths, unsupported target/runtime combinations
    |
    v
Build adapter ----> reproducible build output and dependency metadata
    |
    v
Release packager ----> manifest, hashes, release ID, local audit record
    |
    v
Target adapter
    |-------------------- FTP/FTPS adapter: files only
    |-------------------- SSH/SFTP adapter: files + commands + service restart
    |-------------------- Provider API adapter: provider-native build/release
    |
    v
Transfer engine ----> bounded concurrency, retry, resume, integrity verification
    |
    v
Activation and verification ----> web check or Node health check, status, rollback guidance
```

The initial product should be a local-first command-line application. A dashboard can be added later, but the first reliable version should not require a hosted control plane or store user FTP credentials on a remote server. This choice is appropriate for a personal platform and reduces privacy exposure while the transport core is being hardened.

The architecture should contain the following logical modules:

| Module | Responsibility | Initial implementation choice |
|---|---|---|
| Project scanner | Detect `package.json`, entry points, build scripts, framework output, and unsafe files | Deterministic filesystem scanner with explicit allow/deny rules |
| Policy engine | Compare project requirements with target capabilities | Reject Node runtime deployment to an InfinityFree-free profile |
| Build adapter | Run a user-approved build command in a controlled local workspace | Start with `npm ci` and a configurable build command; never upload `node_modules` by default |
| Release packager | Generate release ID, file list, sizes, and SHA-256 hashes | Expanded directory plus manifest; do not use InfinityFree as artifact storage |
| FTP/FTPS adapter | Connect, authenticate, list, create directories, upload, resume, verify, and disconnect | Existing C++ core behind the frozen ABI, once real transport is wired |
| SSH/SFTP adapter | Copy files and execute commands on a Node-capable server | Separate adapter; do not force process control through FTP |
| Secret provider | Resolve credentials at execution time | OS keyring or environment injection; never commit or log secrets |
| Release state store | Record local plan, result, and audit metadata | Encrypted local file or OS-protected directory; no remote control-plane requirement for MVP |
| CLI | `preflight`, `plan`, `deploy`, `verify`, `rollback`, and `target` commands | Python wrapper initially, with machine-readable JSON output |
| Future dashboard | Manage projects and view deployment history | Add only after the CLI path is dependable |

## Enterprise-grade upgrade roadmap

### Stage 0: Re-baseline the product honestly

Before adding features, align the README, capability flags, tests, and release version with reality. Mark connection and upload as incomplete until they perform real network operations. Remove or regenerate stale build artifacts from source control. Add a CI matrix for Linux first, then Windows and macOS when the transport implementation is stable. Add a test FTP server to CI or a reproducible integration environment; unit tests alone are insufficient.

The stage is complete when a clean checkout can configure, build, run unit tests, run ABI/header checks, and clearly report which optional capabilities are compiled and tested.

### Stage 1: Implement a correct single-stream FTP/FTPS path

The first working slice should be intentionally small: connect to a real server, negotiate explicit FTPS when configured, authenticate, issue `PWD` and `NOOP`, enter binary mode, open passive `EPSV` or `PASV`, create one remote directory, upload one file with `STOR`, wait for the final server reply, disconnect, and return an accurate result. Support plain FTP only as an explicit opt-in for local or controlled environments.

The public `FtpClientImpl` must own or reference a real protocol session. `ftp_connect()` must call the protocol engine rather than only changing an enum. The protocol engine must use the credential vault values, honor timeout settings, map server replies consistently, and close all control and data sockets on error. `ftp_ping()` must issue `NOOP` and wait for the reply.

### Stage 2: Make directory deployment correct before making it fast

Wire the transfer engine to actual `MKD` and `STOR` operations. Implement normalized remote paths, parent-before-child directory creation, symlink policy, maximum file and path limits, local permission errors, partial results, and cleanup of temporary resources. Start with one worker to make failures easy to diagnose. Add bounded parallel uploads only after the single-stream path passes interoperability tests against multiple FTP implementations.

For activation safety, upload each file to a temporary remote name and rename it only after the data-channel completion reply and optional integrity check. If the target does not guarantee atomic directory switching, Ahadu Deploy must report that rollback is file-based and not claim an atomic release.

### Stage 3: Integrate resilience and integrity

Connect the existing retry policy to real failure classes. Retry connection resets, timeouts, passive-channel failures, and selected transient server replies. Do not blindly retry authentication failures, certificate errors, local permission failures, or destructive commands. Implement a stall detector based on observed progress, not a fixed placeholder estimate.

Add a manifest-driven integrity check. The preferred order is server-side hash support where reliably available, followed by size comparison, and finally a client-side post-upload strategy when the server has no hash command. Every retry and final result should include the file path, remote path, attempt count, status, and a redacted reason.

### Stage 4: Harden secrets, callbacks, and ABI behavior

Make credential handling length-aware and best-effort secure across platforms. Never use `strlen()` to determine how many bytes to cleanse after a secret may have been transformed or stored with a different length. Add explicit ownership rules for callback user data, prevent callbacks after client shutdown, and serialize or reject concurrent calls according to a documented thread-safety contract.

Fix the Python progress callback user-data bug and apply native error checking consistently. Guarantee that callbacks are unregistered in `finally` blocks even if conversion or result parsing raises. Add tests for callback exceptions, cancellation, interpreter shutdown, and repeated connect/disconnect cycles.

The C ABI should also add a `struct_size` field to every extensible public structure, not only selected structures, and should version capability flags based on actual compiled behavior. A capability flag must never advertise resume, TLS, compression, or zero-copy unless the public path invokes and tests that feature.

### Stage 5: Build the Ahadu Deploy release workflow

Create the target profile and manifest system. A target profile should specify host, port, protocol, remote root, TLS policy, allowed deployment type, whether remote commands exist, and the verification method. The preflight command should explain exactly why a project is or is not deployable to a target.

For InfinityFree free hosting, the preflight result should say: “File transfer target only; Node.js process execution is not available under the verified free-hosting profile.” For a static site, the workflow may continue. For a Node.js service, it should stop before transfer and suggest selecting a Node-capable target adapter.

The default exclusion set should include `.env`, `.env.*`, private keys, certificates unless explicitly approved, `.git`, `node_modules`, local caches, test artifacts, logs, database files, and editor metadata. The user should be able to inspect and approve the final manifest before the first transfer.

### Stage 6: Add real Node.js deployment through a runtime-capable adapter

Node.js deployment requires a target adapter with process control. For a VPS, that means SSH/SFTP plus commands such as creating a release directory, installing production dependencies from a lockfile, injecting environment variables, switching the active release, restarting a systemd or PM2 service, and checking a health endpoint. For a managed platform, use its documented API or native deployment method.

The FTP adapter may still be used for static assets on the same project, but it must not pretend to start the backend. Ahadu Deploy should support split deployments: static frontend to a web target and Node.js API to a runtime target, with explicit URLs and health checks for each.

### Stage 7: Add operational UX after the core is proven

Only after the CLI is reliable should Ahadu Deploy add a dashboard, multi-project management, deployment history, approval gates, log streaming, scheduled deployments, and notifications. A later service version can run on a persistent host, but the personal MVP should keep credentials and release metadata local unless there is a clear need for a remote control plane.

## Recommended first implementation slice

The highest-value next increment is not a dashboard and not a Node.js launcher. It is a **real FTP/FTPS upload vertical slice** that transfers a small directory to a controlled server and proves the full path.

| Increment | Deliverable | Definition of done |
|---|---|---|
| A | Real connection | `ftp_connect()` performs DNS/TCP connection, greeting, authentication, and explicit FTPS when selected |
| B | Real file transfer | One file is uploaded through a passive data channel and confirmed by the final server response |
| C | Real directory transfer | Nested directories and multiple files upload with accurate result aggregation |
| D | Safe Python API | Python wrapper reports errors, progress, cancellation, and per-file results correctly |
| E | Deploy preflight | A target profile rejects unsupported Node.js-on-InfinityFree deployment before upload |
| F | Static target deployment | A permitted static/PHP tree can be planned, transferred, verified, and reported |
| G | Runtime target adapter | A separate Node-capable adapter can install/start/restart and health-check a Node service |

## Project checklist

- [ ] Replace public stub claims with an accurate implementation status.
- [ ] Add clean-build CI and stop relying on prebuilt artifacts.
- [ ] Wire `FtpClientImpl` to a real protocol session.
- [ ] Implement FTP/FTPS control and passive data channels.
- [ ] Implement `MKD`, `STOR`, completion replies, and result aggregation.
- [ ] Add real integration tests against at least two FTP server implementations.
- [ ] Add TLS verification tests, including hostname mismatch and expired certificate cases.
- [ ] Integrate retry, stall detection, resume, and integrity verification with real transfers.
- [ ] Fix Python callback user-data and native error propagation.
- [ ] Add target profiles, manifest generation, dry-run planning, and secret exclusion.
- [ ] Add an InfinityFree-free policy that permits only compatible web content.
- [ ] Implement a separate SSH/SFTP or provider-native Node.js runtime adapter.
- [ ] Add release activation and rollback semantics appropriate to each target.
- [ ] Add a dashboard only after the local-first CLI is reliable.

## Final recommendation

Proceed with Ahadu Deploy, but define its first milestone as **“secure, verified artifact delivery with target capability detection,”** not “run Node.js on any FTP host.” The current FTP client can become the transport core, but it needs a focused implementation pass to replace the public stubs with real network behavior and a second pass to make the Python binding and security guarantees accurate.

InfinityFree should remain an optional, policy-constrained static/PHP destination. The Node.js backend should be deployed to a runtime-capable environment through a separate adapter that can control processes and ports. This separation preserves the user’s privacy objective, respects InfinityFree’s published rules, and gives Ahadu Deploy an enterprise-grade foundation instead of building a fragile workaround around an unsupported hosting capability.

## References

[1]: https://forum.infinityfree.com/t/nodejs-and-subdomains/90423 "InfinityFree Forum: NodeJs and subdomains"

[2]: https://www.infinityfree.com/terms/ "InfinityFree Terms of Service"

[3]: https://www.infinityfree.com/privacy/ "InfinityFree Privacy Policy"
