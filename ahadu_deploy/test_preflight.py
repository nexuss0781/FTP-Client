from ahadu_deploy.preflight import (
    ProjectRequirements,
    TargetProfile,
    TargetType,
    preflight,
)


def run() -> None:
    node_project = ProjectRequirements(requires_node_runtime=True)

    blocked = preflight(
        TargetProfile(
            name="InfinityFree free",
            target_type=TargetType.INFINITYFREE_FREE,
        ),
        node_project,
    )
    assert not blocked.allowed
    assert blocked.deployment_mode == "blocked"
    assert "Node.js runtime target" in " ".join(blocked.reasons)

    allowed = preflight(
        TargetProfile(
            name="Premium cPanel",
            target_type=TargetType.CPANEL_PASSENGER_NODE,
            node_runtime=True,
            process_control=True,
            reverse_proxy=True,
            provider_registration=True,
        ),
        node_project,
    )
    assert allowed.allowed
    assert allowed.deployment_mode == "cpanel_passenger_node"

    print("Ahadu Deploy preflight tests: PASS")


if __name__ == "__main__":
    run()
