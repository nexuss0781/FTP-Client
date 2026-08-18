# Ahadu Deploy: Full End-to-End Architecture and Execution Plan

**Project:** Ahadu Deploy  
**Purpose:** Personal, privacy-respecting deployment platform for Node.js and compatible web applications  
**Transport foundation:** `nexuss0781/FTP-Client`  
**Current FTPS foundation:** M0–M13 complete and merged into `main`  
**Document status:** Master architecture and execution plan  
**Author:** Manus AI

## 1. Executive decision

Ahadu Deploy should be built as a **target-aware release pipeline**, not as a generic folder copier and not as a PHP-based Node.js launcher. The platform will inspect a project, build a release, exclude secrets, compare project requirements against target capabilities, transfer the approved artifact, activate it through the target’s documented control plane, verify health, and record a local audit trail.

The FTPS library is the transport core for targets that expose file transfer. It does not itself install Node.js, execute `npm install`, start a process, bind a public port, or provide a process manager. A deployment may be called a **running Node.js deployment** only after the target adapter verifies a runtime, a launch mechanism, environment injection, and a reachable health endpoint.

> **Core product rule:** FTP/FTPS transfers files. Runtime activation belongs to a target adapter that has documented process-control capability.

For InfinityFree free hosting, Ahadu Deploy must stop a Node.js deployment during preflight. The existing repository research records that the free target does not provide a verified Node.js runtime and that PHP shell functions needed to launch an uploaded binary are disabled.[1] [2] The platform may support an approved static/PHP deployment there, subject to the provider’s current rules, but it must not claim that a Node.js backend is running.

## 2. What exists today

The repository already contains two complementary assets. The first is the hardened FTPS client library: a C++17 core, C ABI, Python bindings, capability negotiation, MDTM/HASH access, provenance-safe verification, bounded parallel directory transfers, and richer verification and retry policies. The second is an Ahadu Deploy prototype containing target-aware preflight checks, an InfinityFree policy boundary, a cPanel/Passenger adapter, a Node launch sequence, and tests for those boundaries.

The existing documents should be retained as supporting records:

| Document | Role |
| --- | --- |
| [`AHADU_DEPLOY_PLAN.md`](AHADU_DEPLOY_PLAN.md) | Earlier comprehensive architecture and staged roadmap. |
| [`AHADU_NODE_LAUNCH_SOLUTION.md`](AHADU_NODE_LAUNCH_SOLUTION.md) | Exact cPanel/Passenger launch sequence and InfinityFree stop condition. |
| [`ahadu_deploy/README.md`](ahadu_deploy/README.md) | Current prototype behavior and required cPanel target profile. |
| [`ahadu_deploy/preflight.py`](ahadu_deploy/preflight.py) | Runtime-aware target capability policy. |
| [`ahadu_deploy/cpanel_passenger.py`](ahadu_deploy/cpanel_passenger.py) | Provider-managed Passenger adapter prototype. |
| [`M13_STATUS.md`](M13_STATUS.md) | Current FTPS milestone status and validation evidence. |

The missing piece was a single master document connecting these assets into one executable plan. This document is that handoff.

## 3. Product boundary and supported target profiles

Ahadu Deploy is a personal local-first deployment tool. The first dependable form should be a command-line application that keeps credentials and release history on the user’s machine. A hosted dashboard is optional future work and should not be required for the MVP.

Each target profile must declare its capabilities instead of inferring them from successful FTP authentication.

| Target profile | Transfer method | Runtime/process capability | Supported Ahadu Deploy behavior | Hard boundary |
| --- | --- | --- | --- | --- |
| `infinityfree_free` | Provider FTP/FTPS if available | No verified Node runtime or process manager | Static/PHP-compatible web artifact after policy preflight | Never report a running Node.js service. |
| `static_ftp` | FTP/FTPS | Files only | Upload static/PHP release, verify files or HTTP URL | No server-side build, `npm install`, or process restart claim. |
| `cpanel_passenger_node` | FTP/FTPS plus cPanel UAPI | Provider-managed Node and Passenger | Upload, register/edit app, install dependencies, enable, restart, health-check | Requires provider features and API permissions. |
| `vps_node` | Prefer SSH/SFTP plus SSH commands | Full runtime and service control | Stage release, install dependencies, inject environment, switch symlink, restart systemd/PM2, health-check, rollback | FTP alone is insufficient for activation. |
| `managed_node_api` | Provider-native API/CLI | Provider-managed build/runtime | Submit provider-native release and poll build/deploy status | Do not pretend generic FTP is equivalent. |
| `local_ftp_test` | Controlled FTP/FTPS server | Test-only | Interoperability and regression testing | A mock server does not prove public-host compatibility. |

The preflight engine must return one of three outcomes: **allowed**, **allowed with limitations**, or **blocked**. It must explain the result in human-readable text and machine-readable JSON.

## 4. End-to-end architecture

```text
                 +--------------------------+
                 | Project source           |
                 +------------+-------------+
                              |
                              v
                 +--------------------------+
                 | Scanner and policy engine |
                 | secrets, paths, runtime   |
                 +------------+-------------+
                              |
                              v
                 +--------------------------+
                 | Local build adapter       |
                 | npm ci / build / tests    |
                 +------------+-------------+
                              |
                              v
                 +--------------------------+
                 | Release packager          |
                 | manifest, hashes, ID      |
                 +------------+-------------+
                              |
                              v
                 +--------------------------+
                 | Target adapter             |
                 | FTP | cPanel | SSH | API  |
                 +------------+-------------+
                              |
                              v
                 +--------------------------+
                 | FTPS transfer core         |
                 | retry, resume, verify      |
                 +------------+-------------+
                              |
                              v
                 +--------------------------+
                 | Activation adapter         |
                 | restart, switch, enable    |
                 +------------+-------------+
                              |
                              v
                 +--------------------------+
                 | Health and release state   |
                 | verify, record, rollback   |
                 +--------------------------+
```

The platform should have the following modules:

| Module | Responsibility | First implementation |
| --- | --- | --- |
| Project scanner | Detect Node projects, package manager, entrypoint, build output, secrets, and unsafe paths. | Deterministic Python filesystem scanner. |
| Policy engine | Compare project requirements with target capabilities and user policy. | Explicit target profile and preflight rules. |
| Build adapter | Produce a reproducible release from a clean workspace. | `npm ci`, optional user-approved build command, test hook. |
| Release packager | Create release ID, manifest, file metadata, and digests. | Expanded release directory plus JSON manifest. |
| Secret provider | Resolve FTP, cPanel, SSH, and application secrets at execution time. | Environment variables plus OS keyring integration. |
| FTPS adapter | Transfer files, create directories, verify results, retry failures, and preserve provenance. | Python wrapper over the C ABI. |
| cPanel/Passenger adapter | Register or edit Passenger applications and request dependency installation/restart. | Existing `CpanelPassengerClient` prototype. |
| SSH/VPS adapter | Transfer releases and execute service-management commands. | Separate adapter using SFTP/SSH; never overload FTP. |
| Managed-provider adapter | Call provider-native deployment APIs. | Future, provider-specific modules. |
| Activation controller | Execute target-specific activation and rollback steps. | Adapter-owned state machine. |
| Health checker | Validate HTTP health, expected status, body, TLS, and timeout. | HTTP client with redacted diagnostics. |
| Release state store | Persist plans, manifests, results, status transitions, and rollback references. | Local JSON/SQLite under an OS-protected directory. |
| CLI | Expose `target`, `scan`, `plan`, `deploy`, `verify`, `rollback`, and `history`. | Python CLI with JSON output. |

## 5. Release lifecycle

Every deployment must pass through the same logical lifecycle, even when a target adapter omits steps that it does not support.

### 5.1 Target configuration

The user creates a target profile containing the host, port, transport, TLS policy, remote root, runtime type, activation mechanism, health URL, and secret references. The profile stores references to secrets rather than secret values. The user must explicitly choose the target type; Ahadu Deploy must not silently upgrade a generic FTP target into a Node target.

Example profile:

```json
{
  "name": "personal-cpanel-api",
  "target_type": "cpanel_passenger_node",
  "ftp": {
    "host": "ftp.example.com",
    "port": 21,
    "tls": "explicit",
    "verify_certificate": true,
    "remote_root": "nodejs_app",
    "credential_ref": "keyring://ahadu/ftp/personal-cpanel"
  },
  "cpanel": {
    "server": "https://cpanel.example.com:2083",
    "username": "deploy",
    "api_token_ref": "keyring://ahadu/cpanel/personal"
  },
  "application": {
    "name": "ahadu-api",
    "domain": "api.example.com",
    "path": "nodejs_app",
    "startup_file": "app.js",
    "health_url": "https://api.example.com/health"
  }
}
```

### 5.2 Scan and preflight

The scanner identifies `package.json`, lockfiles, likely entrypoints, build scripts, framework output, and required runtime features. It generates a proposed manifest and flags secrets, private keys, `.git`, `node_modules`, caches, logs, test artifacts, database files, and large or suspicious files.

The policy engine then checks whether the project is compatible with the target. A Node service targeting `infinityfree_free` is blocked before any transfer. A static site targeting the same profile may proceed if its files satisfy the user’s selected policy. A cPanel Passenger target is allowed only when the profile declares Node runtime, Passenger, application registration, dependency installation, environment injection, restart, reverse proxy, and health-check capability.

### 5.3 Build

Builds occur locally in a controlled workspace. The default Node flow is:

```text
clean workspace
  -> install from lockfile with npm ci
  -> run approved build command if configured
  -> run approved tests or smoke checks
  -> select production artifact
  -> omit development-only files and node_modules unless target requires them
```

The build adapter must never execute untrusted project commands without an explicit user-approved command policy. It must record the command, working directory, package-manager version, Node version, exit status, and redacted output summary.

### 5.4 Release packaging

The packager creates an immutable release ID, for example `2026-08-19T120000Z-<short-hash>`. It writes the release into a local staging directory and creates a manifest containing:

```json
{
  "release_id": "2026-08-19T120000Z-a1b2c3d4",
  "project": "ahadu-api",
  "target": "personal-cpanel-api",
  "runtime": "nodejs",
  "startup_file": "app.js",
  "files": [
    {
      "path": "app.js",
      "size": 1842,
      "sha256": "..."
    }
  ],
  "excluded": [".env", ".git", "node_modules"],
  "verification": {
    "algorithm": "SHA-256",
    "policy": "local_and_remote"
  }
}
```

The user must be able to inspect this manifest with `ahadu plan` before deployment. The manifest must never contain passwords, API tokens, private keys, or secret values.

### 5.5 Transfer

The FTPS adapter converts the manifest into the native transfer options. It uses explicit TLS and certificate verification for production targets, bounded parallelism only after the target and policy allow it, and M13 verification modes appropriate to the target. The recommended default is `LOCAL_AND_REMOTE` when the server advertises a supported HASH algorithm; otherwise use `LOCAL_EXPECTED` or a clearly recorded `REMOTE_OPTIONAL` policy.

Each per-file result records status, bytes, attempt count, verification status, verification sources, algorithm, local digest, remote digest, and final error. The release result is successful only when the target-specific policy considers all required files successful.

### 5.6 Activation

Activation is owned by the target adapter. The FTPS adapter must not claim activation merely because files uploaded successfully.

For cPanel Passenger, the exact sequence is:

```text
1. Upload release files through FTP/FTPS.
2. Ensure the startup file exists at the application root.
3. Register or edit the Passenger application through cPanel UAPI.
4. Ask Passenger/cPanel to install npm dependencies.
5. Inject or reference environment variables without uploading .env.
6. Enable the application.
7. Upload <application-root>/tmp/restart.txt.
8. Check the public HTTPS health endpoint.
9. Record the release as active only after the health check passes.
```

For a VPS Node target, the sequence is:

```text
1. Upload the release into a new immutable release directory via SFTP.
2. Verify the manifest on the server.
3. Install production dependencies from the lockfile.
4. Write environment variables through a protected secret mechanism.
5. Atomically switch an `current` symlink or equivalent pointer.
6. Restart the systemd/PM2 service.
7. Wait for readiness and run the health check.
8. Roll back the pointer and restart if health fails.
```

### 5.7 Verification and state recording

The health checker must verify HTTPS certificate behavior, expected status code, response deadline, and an application-level response such as `{ "status": "ok" }`. It should record the URL, timestamp, status, latency, and a redacted response summary. It must not record authorization headers, cookies, environment variables, or response bodies by default.

The release state machine should use explicit transitions:

```text
PLANNED -> PREFLIGHTED -> BUILT -> PACKAGED -> TRANSFERRING
         -> TRANSFERRED -> ACTIVATING -> VERIFYING -> ACTIVE
```

Failure transitions must be explicit:

```text
PREFLIGHT_FAILED
BUILD_FAILED
TRANSFER_FAILED
ACTIVATION_FAILED
HEALTHCHECK_FAILED
ROLLBACK_PENDING
ROLLED_BACK
```

A successful file transfer without successful activation is `TRANSFERRED`, not `ACTIVE`.

## 6. Rollback model

Rollback depends on target capability. A cPanel Passenger target may support a previous release directory and restart trigger, but the adapter must verify the provider’s actual path and restart behavior before claiming rollback. A VPS target should use immutable release directories and an atomic `current` pointer, making rollback deterministic. A static FTP target may only support file-by-file restoration; Ahadu Deploy must label that as non-atomic rollback.

The local release state store should retain at least the last five successful manifests and the associated target result records. Rollback must require the target profile, release ID, and an explicit user confirmation unless an automated health-failure policy has been enabled.

## 7. CLI contract

The initial CLI should be local-first and machine-readable:

| Command | Purpose |
| --- | --- |
| `ahadu target add` | Create a target profile and secret references. |
| `ahadu target inspect` | Display target capabilities without printing secrets. |
| `ahadu scan` | Analyze the project and propose exclusions and runtime requirements. |
| `ahadu plan` | Build and display the exact release manifest and preflight decision. |
| `ahadu deploy` | Execute preflight, build, package, transfer, activation, and health verification. |
| `ahadu verify` | Re-run file and application health checks for a selected release. |
| `ahadu rollback` | Restore a previous release using the target adapter’s supported method. |
| `ahadu history` | Display local release state and redacted outcomes. |

Every command should support `--json` for automation and `--dry-run` where execution would change a target. Destructive or externally visible operations such as activation, restart, and rollback should require explicit confirmation unless the user passes a documented non-interactive flag.

## 8. Security and privacy requirements

Ahadu Deploy is personal software, but it must still maintain enterprise-quality boundaries:

| Requirement | Rule |
| --- | --- |
| Secret storage | Use an OS keyring or one-shot environment injection. Never store tokens in manifests, Git, PHP, JavaScript, logs, or exception text. |
| File exclusions | Exclude `.env*`, private keys, `.git`, `node_modules`, logs, caches, database files, and build artifacts by default. |
| TLS | Use explicit FTPS with certificate verification for production transfer targets. Plain FTP is opt-in for controlled local testing only. |
| Remote commands | Use cPanel UAPI or SSH with an explicit adapter. Do not use PHP shell functions as a general process manager. |
| Path safety | Reject absolute paths, traversal, symlink escapes, and unsafe remote names. |
| Audit logs | Record metadata and status transitions, not file contents or secret values. |
| Confirmation | Require manifest approval before first transfer and explicit confirmation before activation or rollback. |
| Failure honesty | Never label a release active without a successful activation and health check. |

For InfinityFree, the platform must be privacy-respecting by limiting uploads to the approved manifest and honoring the provider’s rules. It must not attempt to bypass disabled shell functions, upload a copied Node runtime as a workaround, or present a PHP launcher as a supported Node deployment.

## 9. Engineering roadmap

The FTPS transport milestones M0–M13 are complete. Ahadu Deploy should now proceed through platform milestones rather than adding unrelated transport features.

### A0: Reconcile repository and platform documentation

Update the root project documentation so the FTPS library status and Ahadu Deploy status are distinct. Keep the FTPS library as a separate project and make the Python prototype’s transport injection explicit. Add a top-level Ahadu Deploy README linking to this document.

**Done when:** a new contributor can identify the transport core, target adapters, current supported targets, and unsupported InfinityFree Node behavior in under five minutes.

### A1: Build the local-first CLI skeleton

Create a Python package or CLI project for Ahadu Deploy. Implement target profile loading, keyring references, JSON output, structured errors, and a local state directory. Do not add a dashboard yet.

**Done when:** `target add`, `target inspect`, `scan`, and `plan --dry-run` work without network side effects.

### A2: Implement project scanning and release manifests

Implement deterministic project scanning, exclusion rules, symlink/path safety, Node entrypoint detection, package-manager detection, build metadata, SHA-256 manifest generation, and human-readable plus JSON plan output.

**Done when:** the same source tree produces the same manifest ordering and release digest on repeated runs, excluding secrets by default.

### A3: Connect the Python CLI to the FTPS C ABI

Replace the prototype transport protocol with the real Python `FTPClient`. Map M13 verification and retry options from target policy into `DownloadOptions`/`UploadOptions`, preserve per-file results, and convert native errors into structured deployment errors.

**Done when:** a controlled local FTP/FTPS server receives a static release, the CLI reports per-file verification provenance, and a failed transfer never reports `ACTIVE`.

### A4: Finish static/PHP target deployment

Implement the `static_ftp` adapter. Run preflight, generate the manifest, upload the release, verify required files, optionally run an HTTP check, and record `TRANSFERRED` or `ACTIVE_STATIC` according to the selected health policy.

**Done when:** InfinityFree-compatible static/PHP deployments are supported without Node runtime claims and with provider-policy checks before transfer.

### A5: Complete the cPanel Passenger adapter

Harden the existing cPanel UAPI adapter. Add capability discovery, idempotent register/edit behavior, dependency-install polling, environment-variable validation, restart trigger upload, health checks, redacted API errors, and provider-specific rollback guidance.

**Done when:** a test or staging cPanel target can complete upload, registration, dependency installation, restart, and public `/health` verification using a token that never enters the manifest or logs.

### A6: Implement a VPS Node adapter

Add an SSH/SFTP adapter for a controlled Linux VPS. Use immutable release directories, lockfile-based production install, protected environment injection, systemd or PM2 service control, readiness polling, atomic release switching, and rollback.

**Done when:** a Node application can be deployed, restarted, health-checked, and rolled back on a controlled VPS without relying on FTP for process control.

### A7: Add release state, rollback, and audit UX

Persist release records locally, expose `history`, implement adapter-specific rollback, add confirmation gates, and make every state transition auditable. Add redacted JSON logs suitable for troubleshooting without secret disclosure.

**Done when:** a failed activation can be diagnosed from local state and a previous known-good release can be restored through the appropriate adapter.

### A8: Release and portability hardening

Run cross-platform builds and Python package validation for Linux x86_64 first, then Linux aarch64, Windows x64, and macOS architectures. Produce reproducible artifacts, checksums, release notes, and a security review. This corresponds to the release gates described in [`SPEC/RELEASE.md`](SPEC/RELEASE.md).

**Done when:** a clean checkout produces verified native and Python artifacts, ABI regression tests pass, and the supported platform matrix is honestly documented.

## 10. Immediate execution order

The next implementation sequence should be:

| Order | Work item | Why it comes next |
| --- | --- | --- |
| 1 | Create the Ahadu Deploy CLI/package skeleton. | Establish the platform boundary without touching transport contracts. |
| 2 | Implement scanner, exclusion policy, and deterministic manifest. | Prevent secret leakage and make deployments reviewable. |
| 3 | Implement target profile and preflight command. | Block unsupported Node-on-InfinityFree deployments before transfer. |
| 4 | Wire controlled static deployment through the FTPS Python binding. | Prove the platform-to-transport integration with the lowest activation risk. |
| 5 | Harden cPanel Passenger activation. | Deliver the supported shared-host Node path. |
| 6 | Add VPS Node deployment through SSH/SFTP. | Provide full process control where FTP cannot. |
| 7 | Add state, health, rollback, and packaging. | Turn the prototype into a dependable personal platform. |

## 11. Definition of done for Ahadu Deploy v1

Ahadu Deploy v1 is complete only when all of the following are true:

- A user can add a target without placing secrets in project files or Git.
- A project scan detects Node requirements, entrypoints, lockfiles, secrets, and unsafe paths.
- A dry-run plan shows the exact release manifest, exclusions, target capabilities, and policy decision.
- InfinityFree free hosting is rejected for Node runtime deployment before any transfer.
- A permitted static/PHP target can receive and verify a release through the FTPS core.
- A verified cPanel Passenger target can register, install, restart, and health-check a Node application.
- A verified VPS target can stage, activate, health-check, and roll back a Node application through SSH/SFTP and a process manager.
- Per-file transfer results preserve attempt counts and verification provenance.
- Activation failures never appear as successful deployments.
- Release state and rollback references are stored locally and redact secrets.
- Clean builds, ABI tests, Python tests, integration tests, sanitizer tests, and portability checks pass for the supported matrix.

## 12. Final handoff

The FTPS library is ready to serve as Ahadu Deploy’s file-transfer core. The next work should not attempt to make InfinityFree free hosting run Node.js. Instead, it should implement target adapters around the verified capability model:

```text
Ahadu Deploy CLI
  -> scan and policy
  -> build and manifest
  -> FTPS transfer for file-capable targets
  -> cPanel/Passenger or SSH activation for Node-capable targets
  -> health check and local release state
```

This architecture preserves the personal-project objective, respects provider boundaries, prevents misleading deployment status, and leaves room for a future dashboard without forcing credentials into a hosted control plane.

## References

[1]: https://forum.infinityfree.com/t/nodejs-and-subdomains/90423 "InfinityFree: Node.js availability discussion"

[2]: https://forum.infinityfree.com/t/shell-exec/58004 "InfinityFree: PHP shell functions discussion"

[3]: https://www.infinityfree.com/terms/ "InfinityFree Terms of Service"

[4]: https://www.infinityfree.com/privacy/ "InfinityFree Privacy Policy"

[5]: https://docs.cpanel.net/knowledge-base/web-services/how-to-install-a-node.js-application/ "cPanel: How to Install a Node.js Application"

[6]: https://docs.cpanel.net/cpanel/software/application-manager/ "cPanel Application Manager"

[7]: https://api.docs.cpanel.net/specifications/cpanel.openapi/application-manager/passengerapps-register_application "cPanel UAPI Passenger application registration"
