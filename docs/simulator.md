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
  --latitude 59.35100847144696 --longitude 18.04842390753845 \
  --height-m 74 --orientation-deg 0
```

`orientation_deg` is clockwise from true north to the Ear reference axis and is
required for cross-Ear geographic bearing diagnostics in `hub_event_track.py`.
Match the values in the scenario JSON (`ears.*.orientation_deg`).

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
| `a` (default) | `shared/simulator/sim-ear-a.json` | 59.35100847144696 | 18.04842390753845 | 74 m |
| `b` | `shared/simulator/sim-ear-b.json` | 59.335118371024905 | 18.12680373585177 | 155 m |
| `c` | `shared/simulator/sim-ear-c.json` | 59.32765051899676 | 18.05362024876985 | 72 m |

```bash
uv run python scripts/hub_admin.py set-installation SIM-EAR-002 \
  --latitude 59.335118371024905 --longitude 18.12680373585177 \
  --height-m 155 --orientation-deg 270

uv run python scripts/hub_admin.py set-installation SIM-EAR-003 \
  --latitude 59.32765051899676 --longitude 18.05362024876985 \
  --height-m 72 --orientation-deg 0
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

## Optional L0 extension (protocol v1)

Default publishes remain **legacy** Observations (no `emission_kind` /
`measurements`). To exercise Hub ingest of the Ear-compatible L0 extension:

```bash
./scripts/sim-ear-observation publish --include-l0 --l0-scenario quiet_ambient
./scripts/sim-ear-observation publish --include-l0 --l0-scenario stable_tonal
./scripts/sim-ear-observation publish --include-l0 --l0-scenario varying_level
./scripts/sim-ear-observation publish --include-l0 --l0-scenario high_crest_transient
./scripts/sim-ear-observation publish --include-l0 --l0-scenario clipped_windows
./scripts/sim-ear-observation publish --include-l0 --l0-scenario insufficient_data_nulls
```

Fixtures live in `simulator/ear/l0_measurements.py` and match
[L0 measurement contract](./ear-l0-measurement-contract.md).
`protocol_version` stays `1`. Combined-scene only — no per-source measurements.

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

**Helicopter east→west** (`helicopter_east_west.json`) — shared east→west route; closest approaches roughly B, then C/A (Ear A sits nearer the track than the old northern site).

Default `--mode scripted` keeps the authored observation list (reproducible baseline; exclusive per-Ear windows by design).

**Cross-EAR event pass (sparse stress test)** (`cross_ear_event_pass.json`) — deterministic three-SIM-EAR fixture for Candidate→Event experiments: one westbound vehicle, overlapping A→B→C detection windows, then a time-separated weak ear_b negative Observation. Site baselines are ~1.7–2.3 km — intentionally sparser than normal deployment. Use as a sparse-network stress test (initial localization evaluation target ±250 m). Bearings are true geometry relative to each Ear’s `orientation_deg`, not calibrated accuracy claims. Scripted lifecycles split when the same Ear has a silence greater than 30 s (`SCRIPTED_LIFECYCLE_GAP_SECONDS`).

**Cross-EAR dense scenarios (normal deployment geometry, ≤500 m neighbours)**

| File | Role |
| --- | --- |
| `cross_ear_dense_pass.json` | 3-EAR equilateral ≈480 m, moving source (primary dense baseline) |
| `cross_ear_dense_stationary.json` | Same 3-EAR layout, near-stationary source (~5 m crawl) |
| `cross_ear_dense_poor_geometry.json` | 3 collinear EARs ≈250 m spacing (poor bearing geometry) |
| `cross_ear_dense_four_ear.json` | 4-EAR ≈350 m square, moving source |

Aspirational dense evaluation target ±100 m (report ≤50/100/250 m bands). Not a guaranteed accuracy claim.

**Cross-EAR localization stress scenarios (B2.3 failure boundaries)**

| File | Role |
| --- | --- |
| `cross_ear_stress_nearly_parallel.json` | Tight ~160 m baseline; source far → nearly parallel LOBs |
| `cross_ear_stress_near_baseline.json` | Collinear EARs; source close to the EAR baseline |
| `cross_ear_stress_along_baseline.json` | Collinear EARs; source moves along the baseline |
| `cross_ear_stress_far_source.json` | Good dense triangle; source ≫ EAR spacing |
| `cross_ear_stress_poor_distribution.json` | Two EARs clustered; third poorly placed |
| `cross_ear_stress_inconsistent_bearing.json` | Good triangle control for injected bearing bias/drop/conflict |

Offline dense/sparse/stress evaluation (no MQTT; GT post-hoc only). Geometry classes are summarized separately — do not pool into one accuracy number. B2.4 adds geometric sensitivity (`baseline/sinθ`), ±1/3/5° bearing perturbation displacement, and leave-one-EAR-out layout diagnostics on selected baselines — indicators only, not calibrated uncertainty:

```bash
./scripts/eval_dense_sparse_localization.py
```

```bash
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/cross_ear_event_pass.json \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --ear ear_c=shared/simulator/sim-ear-c.json

./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/cross_ear_dense_pass.json \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --ear ear_c=shared/simulator/sim-ear-c.json
```

`--mode independent_detection` uses one shared clock (default 1 s) and evaluates **every** Ear at each step with a deterministic **range gate** (`--max-range-m`, optional `--ear-max-range ALIAS=m`). Overlap emerges from geometry + range, not authored windows. Detection eligibility is Stage 1 (range only). Stage 2 sets synthetic `signal_level_dbfs` from distance (`--reference-level-dbfs`, `--reference-distance-m`, `--path-loss-exponent`; defaults −30 dBFS @ 1000 m, exponent 1.0). Stage 3 emits observations on episode start / every `--observation-interval-seconds` (default 5, relative to each Ear’s detection-episode start) / episode end — not at every 1 s evaluation. Classification confidence stays a fixed synthetic 0.5; bearing confidence is omitted. Not acoustics.

```bash
# Scripted baseline (unchanged meaning)
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --ear ear_c=shared/simulator/sim-ear-c.json

# Independent multi-Ear detection (example 4 km range)
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --mode independent_detection \
  --max-range-m 4000 \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --ear ear_c=shared/simulator/sim-ear-c.json

./scripts/sim-ear-scenario run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --mode independent_detection \
  --max-range-m 4000 \
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
| `./scripts/sim-ear-scenario dry-run\|run\|retry-pending …` | Multi-Ear scenario (`--mode scripted` or `independent_detection`) |

`--help` works on the wrappers and on the Python entrypoints (Hub MQTT deps load only when running a command).

## Clear observations (keep SIM Ears)

Nodes, installations, and `shared/simulator/sim-ear-*.json` stay. Only Hub observation rows are removed.

```bash
# Delete every observation
uv run python scripts/hub_observations_db.py clear
# same as:
uv run python scripts/hub_observations_db.py prune --older-than-hours 0

# Delete observations older than 4 hours (default)
uv run python scripts/hub_observations_db.py prune

# Custom age
uv run python scripts/hub_observations_db.py prune --older-than-hours 1
```

To wipe **all** Hub device data (including real `EAR-*` / `OUT-*` nodes), see [Local deployment — wipe device data](./local-deployment.md#wipe-hub-device-data-destructive).

Related: [Local deployment](./local-deployment.md) · [Protocol](./protocol.md)
