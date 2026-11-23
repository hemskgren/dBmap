"""Copy only allowlisted executable sources into the integration bind mount."""

import shutil
import sys
from pathlib import Path

HUB_MODULES = (
    "__init__", "config", "db", "device_diag", "device_monitor", "firmware_catalog",
    "ids", "models", "monitoring_math", "mqtt_listener", "mqtt_security",
    "ownership_service", "pagination", "regional_environment", "schemas",
    "service", "topics",
)

FILES = (
    *(f"hub/bootstrap/app/{name}.py" for name in HUB_MODULES),
    "scripts/lib/hub_cli.py",
    "simulator/ear/ear_profile.py",
    "simulator/ear/l0_measurements.py",
    "simulator/ear/publish_safety.py",
    "simulator/ear/sim_ear_slots.py",
    "simulator/ear/simulate_keepalive.py",
    "simulator/ear/simulate_observation.py",
    "tests/integration/ear_mqtt/lease_state.py",
    "tests/integration/ear_mqtt/round_trip.py",
)


def main(repo: Path, dest: Path):
    for filename in FILES:
        source = repo / filename
        if source != source.resolve() or not source.is_file():
            raise ValueError(f"bundle source must be a regular checkout file: {filename}")
        output = dest / filename
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, output)


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve(), Path(sys.argv[2]))
