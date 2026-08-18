# Ahadu Deploy Node.js Target Prototype

This prototype implements the provider-managed launch model for Node.js applications on a cPanel/CloudLinux/Passenger host.

## The exact sequence

Ahadu Deploy first scans the project and creates a release manifest. It excludes secrets, private keys, `.git`, `node_modules`, logs, and local caches. It then uploads the application files through the hardened FTP/FTPS transport into the application root.

After the files exist on the server, the deployer uses a cPanel API token over HTTPS to call the Passenger application interface. The provider must already have Node.js, Passenger, Application Manager, and npm dependency support enabled. Ahadu Deploy registers or edits the application, configures the domain and application path, installs dependencies through cPanel, enables the application, uploads `tmp/restart.txt`, and checks `/health` through the public application URL.

The Node application must listen on the provider-selected `PORT` value and use the configured startup file, normally `app.js`. Passenger performs reverse port binding and exposes the app through the configured HTTP or HTTPS URL. Ahadu Deploy does not try to start the Node process through PHP.

## InfinityFree free-hosting behavior

The InfinityFree free-hosting target must fail preflight for a Node.js deployment. The provider’s published support information says free hosting does not provide Node.js, and PHP command-line functions such as `exec`, `shell_exec`, `system`, and `proc_open` are disabled. Therefore, uploading a Node binary and using a PHP file to launch it cannot establish a supported Node runtime on that target.

The same target may still be used for a permitted static/PHP deployment when the user’s files comply with the provider’s current rules.

## Required target profile

```json
{
  "target_type": "cpanel_passenger_node",
  "ftp": {
    "host": "ftp.example.com",
    "port": 21,
    "tls": "explicit",
    "remote_root": "nodejs_app"
  },
  "cpanel": {
    "server": "https://cpanel.example.com:2083",
    "username": "deploy",
    "api_token": "stored-in-os-keyring"
  },
  "application": {
    "name": "ahadu-api",
    "domain": "api.example.com",
    "path": "nodejs_app",
    "startup_file": "app.js",
    "health_path": "/health"
  }
}
```

The API token must never be written to the manifest, source repository, transfer logs, or PHP files. The production client should obtain it from an operating-system keyring or a one-shot secret provider.
