# Ahadu Deploy: Exact Node.js Launch Solution

## Direct answer

The user’s idea is technically valid **when the destination is a Node-capable cPanel/CloudLinux host**. The correct sequence is not “PHP executes the uploaded Node binary.” The correct sequence is:

```text
Ahadu Deploy
  -> FTP/FTPS uploads app.js, package.json, and release files
  -> cPanel/Passenger registers the application
  -> cPanel installs npm dependencies
  -> Passenger launches Node and reverse-proxies the public URL
  -> Ahadu Deploy uploads tmp/restart.txt
  -> Ahadu Deploy checks /health
```

For InfinityFree free hosting, the same sequence stops at the first step. Its free hosting does not provide the Node runtime or the PHP command-line functions needed to invoke an uploaded binary. Therefore, the Ahadu Deploy preflight must identify that target as `infinityfree_free` and refuse to label a Node upload as a running backend.

## What to transfer

The release should contain the application source and lockfile, not a copied runtime binary:

```text
app.js
package.json
package-lock.json
src/
config/
tmp/restart.txt       # zero-byte file, uploaded after activation
```

Do not upload `.env`, private keys, `.git`, `node_modules`, logs, or local build caches. The provider-managed Node version is preferable because a copied Node binary may be incompatible with the server CPU architecture, operating-system ABI, libc, shared libraries, or security policy.

## Node entrypoint

The application must listen on the port supplied by the provider and normally use `app.js` as the startup file:

```js
const http = require("http");
const port = Number(process.env.PORT || 3000);

const server = http.createServer((req, res) => {
  if (req.url === "/health") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ status: "ok" }));
    return;
  }
  res.writeHead(404);
  res.end("Not found");
});

server.listen(port, "127.0.0.1");
```

Passenger controls the actual public binding. Ahadu Deploy should not hard-code a public TCP port into the Node application.

## cPanel/Passenger launch contract

The target provider must expose all of the following capabilities:

| Capability | Purpose |
|---|---|
| Node.js package/runtime | Runs `app.js` |
| Passenger or equivalent process manager | Keeps the application under provider control |
| Application registration | Maps a domain/path to the application root |
| npm dependency installation | Installs production dependencies |
| Environment variables | Supplies `NODE_ENV`, database URLs, and other secrets without uploading `.env` |
| Reverse proxy | Maps the public HTTPS URL to the application process |
| Restart operation | Applies a new release without manual SSH work |
| Health endpoint | Lets Ahadu Deploy verify the release |

The cPanel UAPI target adapter in this branch uses the following conceptual operations:

```text
PassengerApps.register_application
PassengerApps.ensure_deps
PassengerApps.enable_application
PassengerApps.edit_application
FTP upload: <application-root>/tmp/restart.txt
HTTP GET: <application-url>/health
```

The cPanel API token must be stored in an operating-system keyring or injected for one execution. It must not be placed in PHP, JavaScript, JSON manifests, Git, or transfer logs.

## Where PHP belongs

PHP can be part of the deployed web application, but it should not be responsible for launching Node on shared hosting. On a Node-capable cPanel account, Passenger is the launcher. PHP may call the Node API over HTTPS or display deployment status.

On a VPS where `proc_open` is available, PHP could technically start a child process, but that is not the enterprise design. Ahadu Deploy should use systemd, Supervisor, or another process manager and let PHP call a deployment service. A PHP web request is not a safe long-running process manager.

## Ahadu Deploy target rules

```text
if target == infinityfree_free and project_requires_node:
    stop before upload
    explain: FTP-only/PHP target; no verified Node launcher

if target == cpanel_passenger_node:
    upload files through FTP/FTPS
    register or edit Passenger application through cPanel UAPI
    ensure npm dependencies
    enable application
    upload tmp/restart.txt
    verify public /health endpoint

if target == vps_node:
    upload release through SFTP/SSH
    install dependencies from lockfile
    write environment through secret injection
    switch release
    restart systemd service
    verify /health endpoint
```

## Implementation status

The repository branch `feature/ahadu-deploy-passenger-prototype` now contains a working prototype entrypoint, a PHP capability probe, a cPanel/Passenger API adapter, runtime-aware preflight checks, a target manifest, and dependency-free tests. The remaining blocking work is to finish the native FTP client’s real upload implementation and then connect that transport to the cPanel adapter.

## Sources

[1]: https://forum.infinityfree.com/t/nodejs-and-subdomains/90423 "InfinityFree: Node.js availability"

[2]: https://forum.infinityfree.com/t/shell-exec/58004 "InfinityFree: PHP shell functions disabled"

[3]: https://kb.ifastnet.com/index.php?/article/AA-00416/0/Using-Node.js-on-a-premium-shared-hosting-account.html "iFastNet: Node.js on premium shared hosting"

[4]: https://docs.cpanel.net/knowledge-base/web-services/how-to-install-a-node.js-application/ "cPanel: How to Install a Node.js Application"

[5]: https://docs.cpanel.net/cpanel/software/application-manager/ "cPanel: Application Manager"

[6]: https://api.docs.cpanel.net/specifications/cpanel.openapi/application-manager/passengerapps-register_application "cPanel UAPI: Register Passenger application"
