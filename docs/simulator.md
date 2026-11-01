# Ear simulator

Laptop tools that register `SIM-EAR-*` devices on your Hub and publish MQTT observations. Credentials stay in `shared/simulator/` so you can reuse the same Ears.

Hub host/token come from `.env`. No arguments → help (same as other Hub scripts).

## Do this first (Ear A)

Hub must be running. From the repo root:

```bash
set -a
. ./.env
set +a
```

**1. Add Ear A and publish one observation**

```bash
./scripts/sim-ear-observation add
```

Note the printed node id (for example `SIM-EAR-001`). `add` / `publish` send a keepalive before the observation(s).

**2. Place it on the map (once)**

```bash
uv run python scripts/hub_admin.py set-installation SIM-EAR-001 \
  --latitude 59.374868678394485 --longitude 18.001342117095227 --height-m 32
```

**3. Publish again later (same Ear)**

```bash
./scripts/sim-ear-observation publish
```

**4. Optional — stay online**

`once` = one keepalive and exit. `watch` = keep publishing every 30s until Ctrl+C (continuous). Default `--ear a`; use `--ear all` for every provisioned sim Ear that has a credentials file.

```bash
./scripts/sim-ear-keepalive watch
# or, after adding B/C:
./scripts/sim-ear-keepalive watch --ear all
```

**5. Check**

```bash
uv run python scripts/hub_admin.py show SIM-EAR-001
uv run python scripts/hub_event_track.py list --node-id SIM-EAR-001
```

---

## More Ears (B / C)

```bash
./scripts/sim-ear-observation add --ear b
./scripts/sim-ear-observation add --ear c
```

Then install once (use the printed node ids):

| `--ear` | Credentials file | Latitude | Longitude | Height |
| --- | --- | ---: | ---: | ---: |
| `a` (default) | `shared/simulator/sim-ear-a.json` | 59.374868678394485 | 18.001342117095227 | 32 m |
| `b` | `shared/simulator/sim-ear-b.json` | 59.335118371024905 | 18.12680373585177 | 155 m |
| `c` | `shared/simulator/sim-ear-c.json` | 59.32765051899676 | 18.05362024876985 | 72 m |

```bash
uv run python scripts/hub_admin.py set-installation SIM-EAR-002 \
  --latitude 59.335118371024905 --longitude 18.12680373585177 --height-m 155

uv run python scripts/hub_admin.py set-installation SIM-EAR-003 \
  --latitude 59.32765051899676 --longitude 18.05362024876985 --height-m 72
```

Reuse:

```bash
./scripts/sim-ear-observation publish --ear b
./scripts/sim-ear-keepalive watch --ear b
./scripts/sim-ear-keepalive watch --ear all
```

---

## Several observations

```bash
./scripts/sim-ear-observation publish --count 4 --interval-seconds 15
```

---

## Scenarios (optional)

Needs the Ears used by the scenario already added (and installed). Geometry matches the A/B/C sites above.

`dry-run` prints `source_family`, speed, and how long `run` will take at `--time-scale`. `run` / `retry-pending` print the scenario banner first, send keepalives, then each line as `t=…  ear  observation_id` when sent (keepalives again at the end of `run`).

**Vehicle A→B** (`vehicle_passes.json`) — Ears A and B:

```bash
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/vehicle_passes.json \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --time-scale 0.1

./scripts/sim-ear-scenario run \
  --scenario-file simulator/ear/scenarios/vehicle_passes.json \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --time-scale 0.1
```

**Airplane landing BMA** (`bma_landing.json`) — approach from the south to Bromma (59.35485, 17.94246). Closest to Ear C for most of the approach; CPA to C about 35–40s before touchdown. Mostly C, plus two quieter B hits (high free-air mount) and one late A near touchdown. Needs Ears A, B, and C:

```bash
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/bma_landing.json \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --ear ear_c=shared/simulator/sim-ear-c.json

./scripts/sim-ear-scenario run \
  --scenario-file simulator/ear/scenarios/bma_landing.json \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --ear ear_c=shared/simulator/sim-ear-c.json \
  --time-scale 0.1
```

**Helicopter east→west** (`helicopter_east_west.json`) — passes between all three Ears (closest B, then C, then A). Each Ear also has a quiet low-confidence observation before and after its main cluster (long-range notice):

```bash
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --ear ear_c=shared/simulator/sim-ear-c.json

./scripts/sim-ear-scenario run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --ear ear_c=shared/simulator/sim-ear-c.json \
  --time-scale 0.1
```

Interrupted publish (same `--ear` mapping as the run):

```bash
./scripts/sim-ear-scenario retry-pending \
  --scenario-file simulator/ear/scenarios/vehicle_passes.json \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json
```

---

## Commands

| Command | What it does |
| --- | --- |
| `./scripts/sim-ear-observation` | Help |
| `./scripts/sim-ear-observation add [--ear a\|b\|c]` | New SIM device + one observation |
| `./scripts/sim-ear-observation publish [--ear …]` | Reuse credentials; publish again |
| `./scripts/sim-ear-keepalive once\|watch [--ear a\|b\|c\|all]` | Keepalive (`once` = one round; `watch` = continuous) |
| `./scripts/sim-ear-scenario dry-run\|run\|retry-pending …` | Multi-Ear scripted pass |

`--help` works on the wrappers and on the Python entrypoints (Hub MQTT deps load only when running a command).

## Clear observations (keep SIM Ears)

Nodes, installations, and `shared/simulator/sim-ear-*.json` stay. Only Hub observation rows are removed.

```bash
# Delete every observation
uv run python scripts/hub_observations_db.py clear

# Delete observations older than 4 hours (default)
uv run python scripts/hub_observations_db.py prune

# Custom age
uv run python scripts/hub_observations_db.py prune --older-than-hours 1
```

To wipe **all** Hub device data (including real `EAR-*` / `OUT-*` nodes), see [Local deployment — wipe device data](./local-deployment.md#wipe-hub-device-data-destructive).

Related: [Local deployment](./local-deployment.md) · [Protocol](./protocol.md)
