# Ahadu Deploy Review Findings

## Repository evidence

- The public README presents a production-grade, high-performance FTP/FTPS library with C++ core, C ABI, Python bindings, concurrency, TLS, retries, resume, telemetry, and zero-copy claims.
- `src/ftpclient.cpp` currently implements `ftp_connect()` as validation plus credential-vault storage and a state transition; it does not instantiate or call the protocol engine. `ftp_ping()` only checks state and returns success.
- `src/ftpclient.cpp::ftp_upload_dir()` validates arguments and connection state, initializes only a few result fields, then returns `FTP_ERR_INVALID_STATE`.
- `FtpClientImpl.hpp` contains configuration, credential, callback, and pin storage but no protocol engine, live transport, transfer engine, or resilience controller member.
- `ProtocolEngine.hpp` has some control-channel groundwork, but `upload_file()`, `download_file()`, and `create_remote_dir()` are explicit stubs. Its TLS setter only toggles a boolean, and the default transport factory returns `PlainTransport`.
- `TransferEngine.cpp` traverses and sorts a manifest, but does not submit upload work to the thread pool. The per-file worker reads local bytes and marks the task successful without opening an FTP data channel or sending bytes to a server. Result filling is empty.
- `python/ftpclient/client.py` has a progress callback user-data bug: it computes `user_data` but passes `ffi.NULL` into `ftp_upload_dir()`. It also does not call `_check_error()` for the upload return code and assumes result fields are populated.
- Existing test artifacts and tests encode stub behavior: a valid `ftp_connect()` is expected to succeed without network I/O, while `ftp_upload_dir()` is expected to return `FTP_ERR_INVALID_STATE`.
- The repository cannot currently be rebuilt from source in this sandbox because `cmake` is not installed; pre-existing build artifacts are present. A placeholder scan confirms multiple unimplemented/simplified paths.

## InfinityFree evidence

- InfinityFree’s 2024 support thread states that InfinityFree free hosting does not support Node.js; Node.js support mentioned in the discussion refers to paid iFastNet hosting, not InfinityFree.
- InfinityFree’s official Terms of Service state that scripts must produce web-based content rather than act as an application server, and prohibit use for file distribution, file hosting/sharing, relay, streaming, and excessive resource consumption. The terms also state that users must back up their content and that InfinityFree does not warrant backups.
- InfinityFree’s Privacy Policy states that free website hosting is fulfilled by iFastNet Ltd., and that personal information such as email address and IP address is transmitted to iFastNet. It also states that data may be processed by service providers and across international borders.

## Consequence for Ahadu Deploy

The current codebase is a promising protocol/ABI foundation, not yet a working deployment client. Ahadu Deploy must first make the transfer path real and test it against a controlled FTP/FTPS server. Even after that, FTP can transfer a Node.js project’s files but cannot create a Node.js runtime, process manager, listening port, or server-side package installation on an InfinityFree free-hosting account. Therefore the platform should model InfinityFree as a static/PHP-compatible file target unless the user’s specific hosting plan and provider documentation explicitly confirm Node.js runtime support.

## Sources

1. https://forum.infinityfree.com/t/nodejs-and-subdomains/90423
2. https://www.infinityfree.com/terms/
3. https://www.infinityfree.com/privacy/

## Browser-verified excerpts

The official Terms of Service page states that scripts must be designed to produce web-based content and must not use the server as an application server. It also states that hosted files must be part of the active website and linked to it, and disallows backups, downloads, and other non-web-based content. These clauses are direct blockers for treating the free service as a general Node.js application host or artifact repository.

The official Privacy Policy page states that free hosting is fulfilled by iFastNet Ltd. and that InfinityFree transmits the account email address and IP address to iFastNet for service fulfillment. This means Ahadu Deploy should avoid uploading secrets or unrelated personal data and should treat provider-side processing as an explicit deployment constraint.

## PHP-plus-Node launcher investigation

The proposed launcher model has two distinct requirements: PHP must be able to invoke an operating-system process, and the provider must allow that process to remain running and expose it through a web URL or port. FTP can transfer a Node binary and application files, but it cannot grant either capability.

InfinityFree forum support states that free hosting disables PHP command-line functions including `shell_exec`, `exec`, `system`, and `proc_open`, and that command-line access is not allowed on free hosting. This directly blocks a PHP page from launching an uploaded Node executable on the free service.

The official iFastNet knowledge-base article documents a different supported model for premium CloudLinux/cPanel hosting: Node and npm are installed server-side, the user creates an application through cPanel's Setup Node.js App, selects an application root and startup file, runs npm installation, and uses the control panel's Start/Restart controls. This is not equivalent to uploading a portable Node runtime and launching it from PHP; it is a provider-managed Node application feature.

### Sources

4. https://forum.infinityfree.com/t/shell-exec/58004
5. https://kb.ifastnet.com/index.php?/article/AA-00416/0/Using-Node.js-on-a-premium-shared-hosting-account.html

## Provider-managed launch path

The cPanel documentation confirms a legitimate version of the desired workflow on a Node-capable hosting account. After files are uploaded, the provider-managed Passenger/Application Manager layer registers the application, maps it to a domain or URL, installs npm dependencies, injects environment variables, and exposes the application through the web server. cPanel documents `app.js` as the default startup file, reverse port binding behind the web server, and `tmp/restart.txt` as the restart trigger after a deployment.

The cPanel UAPI documentation exposes PassengerApps operations such as `enable_application`, `ensure_deps`, and application editing. This suggests Ahadu Deploy can have a cPanel/Passenger target adapter that performs FTP upload first, then uses a user-provided cPanel API token over HTTPS to register or update the application, install dependencies, enable it, touch the restart file, and run a health check. This adapter is applicable only when the hosting provider has enabled the corresponding cPanel/CloudLinux features; it is not available on InfinityFree free hosting merely because FTP works.

6. https://docs.cpanel.net/knowledge-base/web-services/how-to-install-a-node.js-application/
7. https://docs.cpanel.net/cpanel/software/application-manager/
8. https://api.docs.cpanel.net/specifications/cpanel.openapi/application-manager/passengerapps-register_application
9. https://api.docs.cpanel.net/specifications/cpanel.openapi/application-manager/passengerapps-enable_application
10. https://api.docs.cpanel.net/specifications/cpanel.openapi/application-manager/passengerapps-ensure_deps
