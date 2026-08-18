"""cPanel Passenger target adapter for Ahadu Deploy.

This adapter intentionally does not launch Node through PHP. It uses the
provider-supported contract:

1. Upload application files through FTP/FTPS.
2. Register or update the Passenger application through cPanel UAPI.
3. Ask cPanel to install npm dependencies and enable the application.
4. Upload tmp/restart.txt to trigger Passenger reload.
5. Verify the public health endpoint.

The FTP transport is injected so this module can use the hardened native
FTPClient once its real upload path is complete.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Mapping, Protocol
from urllib.parse import urljoin

import requests


class FTPTransport(Protocol):
    def upload_directory(self, local_path: str, remote_path: str, **kwargs: Any) -> Any:
        ...

    def upload_file(self, local_path: str, remote_path: str, **kwargs: Any) -> Any:
        ...


class CpanelUAPIError(RuntimeError):
    """Raised when cPanel UAPI rejects a deployment operation."""


@dataclass(frozen=True)
class PassengerApplication:
    name: str
    domain: str
    path: str
    startup_file: str = "app.js"
    environment: str = "production"
    application_url: str = "/"
    health_path: str = "/health"
    environment_variables: Mapping[str, str] = field(default_factory=dict)


class CpanelPassengerClient:
    """Small, redacting cPanel UAPI client for Passenger applications."""

    def __init__(
        self,
        server: str,
        username: str,
        api_token: str,
        *,
        verify_tls: bool = True,
        timeout_seconds: float = 30.0,
        session: requests.Session | None = None,
    ) -> None:
        if not server.startswith(("https://", "http://")):
            server = "https://" + server
        self.base_url = server.rstrip("/") + "/execute/"
        self.username = username
        self._session = session or requests.Session()
        self._session.headers.update({"Authorization": f"cpanel {username}:{api_token}"})
        self.verify_tls = verify_tls
        self.timeout_seconds = timeout_seconds

    def call(
        self,
        module: str,
        function: str,
        params: Mapping[str, Any] | list[tuple[str, Any]] | None = None,
    ) -> Any:
        """Call UAPI and return its data, never including the API token in errors."""
        endpoint = urljoin(self.base_url, f"{module}/{function}")
        response = self._session.get(
            endpoint,
            params=params or {},
            timeout=self.timeout_seconds,
            verify=self.verify_tls,
        )
        response.raise_for_status()
        payload = response.json()
        result = payload.get("result", {})
        if result.get("status") != 1 or result.get("errors"):
            errors = result.get("errors") or payload.get("errors") or ["unknown cPanel UAPI error"]
            raise CpanelUAPIError(f"{module}.{function} failed: {errors}")
        return result.get("data")

    def list_applications(self) -> Any:
        return self.call("PassengerApps", "list_applications")

    def register(self, app: PassengerApplication, *, enable: bool = True) -> Any:
        params: list[tuple[str, Any]] = [
            ("name", app.name),
            ("path", app.path.lstrip("/")),
            ("domain", app.domain),
            ("environment", app.environment),
            ("enabled", 1 if enable else 0),
        ]
        for key, value in app.environment_variables.items():
            params.extend([("envvar_name", key), ("envvar_value", value)])
        return self.call("PassengerApps", "register_application", params)

    def edit(self, app: PassengerApplication, *, enable: bool = True) -> Any:
        params: list[tuple[str, Any]] = [
            ("name", app.name),
            ("domain", app.domain),
            ("environment", app.environment),
            ("enabled", 1 if enable else 0),
        ]
        for key, value in app.environment_variables.items():
            params.extend([("envvar_name", key), ("envvar_value", value)])
        return self.call("PassengerApps", "edit_application", params)

    def ensure_dependencies(self, app_path: str) -> Any:
        return self.call(
            "PassengerApps",
            "ensure_deps",
            {"type": "npm", "app_path": app_path},
        )

    def enable(self, name: str) -> Any:
        return self.call("PassengerApps", "enable_application", {"name": name})


def build_launch_sequence(app: PassengerApplication) -> list[str]:
    """Return an auditable plan without touching a provider account."""
    return [
        f"Upload release files into {app.path}",
        f"Ensure startup file exists: {app.path.rstrip('/')}/{app.startup_file}",
        f"Register or edit Passenger application {app.name!r} on {app.domain}",
        "Install npm dependencies through cPanel PassengerApps.ensure_deps",
        f"Enable application {app.name!r}",
        f"Upload zero-byte restart trigger: {app.path.rstrip('/')}/tmp/restart.txt",
        f"HTTP health check: {app.application_url.rstrip('/')}{app.health_path}",
    ]
