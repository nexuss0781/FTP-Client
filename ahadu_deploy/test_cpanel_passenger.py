from __future__ import annotations

from typing import Any

from ahadu_deploy.cpanel_passenger import CpanelPassengerClient, PassengerApplication


class FakeResponse:
    def raise_for_status(self) -> None:
        return None

    def json(self) -> dict[str, Any]:
        return {"result": {"status": 1, "errors": None, "data": {"ok": True}}}


class FakeSession:
    def __init__(self) -> None:
        self.headers: dict[str, str] = {}
        self.calls: list[dict[str, Any]] = []

    def get(self, endpoint: str, **kwargs: Any) -> FakeResponse:
        self.calls.append({"endpoint": endpoint, **kwargs})
        return FakeResponse()


def test_register_preserves_multiple_environment_variables() -> None:
    session = FakeSession()
    client = CpanelPassengerClient(
        "https://cpanel.example.test",
        "deploy",
        "secret-token",
        session=session,  # type: ignore[arg-type]
    )
    app = PassengerApplication(
        name="ahadu-api",
        domain="example.com",
        path="nodejs_app",
        environment_variables={"NODE_ENV": "production", "PORT": "3000"},
    )

    client.register(app)

    params = session.calls[0]["params"]
    assert ("envvar_name", "NODE_ENV") in params
    assert ("envvar_value", "production") in params
    assert ("envvar_name", "PORT") in params
    assert ("envvar_value", "3000") in params
    assert "secret-token" not in repr(session.calls[0])
