from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from types import SimpleNamespace

import pytest

spec = spec_from_file_location(
    "dbmap_ear_simulator",
    Path(__file__).with_name("simulate_observation.py"),
)
assert spec is not None and spec.loader is not None
simulate_observation = module_from_spec(spec)
spec.loader.exec_module(simulate_observation)


def test_simulator_identity_requires_simulated_ear_prefix() -> None:
    simulate_observation.require_simulator_identity("SIM-EAR-001")

    with pytest.raises(
        simulate_observation.UnsupportedSimulatorIdentity,
        match="hub must be rebuilt and redeployed",
    ):
        simulate_observation.require_simulator_identity("EAR-001")


def test_old_hub_identity_is_rejected_before_consuming_bootstrap_token(monkeypatch) -> None:
    monkeypatch.setattr(
        simulate_observation,
        "api_request",
        lambda *args, **kwargs: pytest.fail("provision endpoint must not be called"),
    )
    args = SimpleNamespace(api_url="https://hub.example:8443", ca="ca.crt")

    with pytest.raises(simulate_observation.UnsupportedSimulatorIdentity):
        simulate_observation.finish_provisioning(
            args,
            {"node_id": "EAR-001", "bootstrap_token": "one-time-token"},
            "admin-token",
        )
