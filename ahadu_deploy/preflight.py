"""Target capability checks for Ahadu Deploy.

The preflight layer separates file-transfer capability from runtime and
process-management capability. It prevents a deployment from being reported
as a running Node.js service merely because FTP credentials are valid.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum


class TargetType(StrEnum):
    INFINITYFREE_FREE = "infinityfree_free"
    STATIC_FTP = "static_ftp"
    CPANEL_PASSENGER_NODE = "cpanel_passenger_node"
    DIRECT_NODE_PROCESS = "direct_node_process"


@dataclass(frozen=True)
class TargetProfile:
    name: str
    target_type: TargetType
    ftp_enabled: bool = True
    node_runtime: bool = False
    process_control: bool = False
    reverse_proxy: bool = False
    provider_registration: bool = False


@dataclass(frozen=True)
class ProjectRequirements:
    requires_node_runtime: bool
    requires_background_process: bool = True


@dataclass(frozen=True)
class PreflightResult:
    allowed: bool
    deployment_mode: str
    reasons: tuple[str, ...]


def preflight(target: TargetProfile, project: ProjectRequirements) -> PreflightResult:
    reasons: list[str] = []

    if not target.ftp_enabled:
        return PreflightResult(False, "blocked", ("Target does not expose FTP/FTPS transfer.",))

    if not project.requires_node_runtime:
        return PreflightResult(True, "static_or_php", ("Project does not require a Node.js runtime.",))

    if target.target_type is TargetType.INFINITYFREE_FREE:
        return PreflightResult(
            False,
            "blocked",
            (
                "InfinityFree free hosting is an FTP/PHP web target, not a Node.js runtime target.",
                "A transferred Node binary cannot be launched through the disabled PHP shell functions.",
                "Select a provider-managed Node target instead of attempting a workaround.",
            ),
        )

    if not target.node_runtime:
        reasons.append("Target has no verified Node.js runtime.")
    if project.requires_background_process and not target.process_control:
        reasons.append("Target has no verified process manager or application launcher.")
    if not target.reverse_proxy:
        reasons.append("Target has no verified HTTP reverse proxy or public application URL.")

    if reasons:
        return PreflightResult(False, "blocked", tuple(reasons))

    if target.target_type is TargetType.CPANEL_PASSENGER_NODE:
        return PreflightResult(
            True,
            "cpanel_passenger_node",
            ("Files can be transferred, then Passenger can register and restart the Node application.",),
        )

    return PreflightResult(
        True,
        "direct_node_process",
        ("Files can be transferred and the target can launch and expose the Node process.",),
    )
