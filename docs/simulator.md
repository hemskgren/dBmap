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
# or every provisioned slot that has a credentials file (a–g):
./scripts/sim-ear-keepalive watch --ear all
```

**5. Check**

```bash
uv run python scripts/hub_admin.py show SIM-EAR-001
uv run python scripts/hub_event_track.py list --node-id SIM-EAR-001
```

---

## Simulated Ear slots (A–G)

Slots are local aliases. Credentials land in `shared/simulator/sim-ear-{slot}.json`
(gitignored). Hub still allocates the next free `SIM-EAR-*` id at `add` time.

| Slot | Credentials file | Expected node id | Latitude | Longitude | Height | Orientation |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| `a` (default) | `shared/simulator/sim-ear-a.json` | `SIM-EAR-001` | 59.35100847144696 | 18.04842390753845 | 74 m | 0° |
| `b` | `shared/simulator/sim-ear-b.json` | `SIM-EAR-002` | 59.335118371024905 | 18.12680373585177 | 155 m | 270° |
| `c` | `shared/simulator/sim-ear-c.json` | `SIM-EAR-003` | 59.32765051899676 | 18.05362024876985 | 72 m | 0° |
| `d` | `shared/simulator/sim-ear-d.json` | `SIM-EAR-004` | 59.3655000 | 18.0900000 | 95 m | 180° |
| `e` | `shared/simulator/sim-ear-e.json` | `SIM-EAR-005` | 59.3180000 | 18.1000000 | 88 m | 0° |
| `f` | `shared/simulator/sim-ear-f.json` | `SIM-EAR-006` | 59.3550000 | 17.9950000 | 80 m | 135° |
| `g` | `shared/simulator/sim-ear-g.json` | `SIM-EAR-007` | 59.3120000 | 17.9800000 | 110 m | 45° |

This table is the **authoritative installation proposal** for the shared SIM estate
(also encoded in `simulator/ear/sim_ear_slots.py`). D–G are **simulated placement
proposals**, not claims about real sites — adjust after reviewing them on the map.

**Coordinate consistency**

- Hub `set-installation` coordinates and scenario `ears.*.` coordinates must
  **match** whenever a scenario uses those devices.
- Do **not** change `helicopter_east_west.json` in this estate expansion. Its
  `ear_a` is ~20 m from the documented Hub A install above
  (`59.3511652, 18.0487074` in the scenario vs `59.3510085, 18.0484239` here).
  B and C already match. Resolve A drift deliberately later — do not silently
  merge sources of truth.
- Overlapping multi-EAR helicopter scenarios: ADE (`helicopter_ade_overlap.json`)
  and ABDE (`helicopter_abde_overlap.json`). Slots F–G remain Hub inventory /
  keepalive / future use.

### Register B / C (existing)

```bash
./scripts/sim-ear-observation add --ear b
./scripts/sim-ear-observation add --ear c
```

Confirm printed ids are `SIM-EAR-002` / `SIM-EAR-003`, then:

```bash
uv run python scripts/hub_admin.py set-installation SIM-EAR-002 \
  --latitude 59.335118371024905 --longitude 18.12680373585177 \
  --height-m 155 --orientation-deg 270

uv run python scripts/hub_admin.py set-installation SIM-EAR-003 \
  --latitude 59.32765051899676 --longitude 18.05362024876985 \
  --height-m 72 --orientation-deg 0
```

### Register D–G (new)

**Before each `add`:** confirm the expected id is free and that you are not
relocating an existing device.

```bash
uv run python scripts/hub_admin.py list
uv run python scripts/hub_admin.py show SIM-EAR-004   # expect not found / unused
uv run python scripts/hub_admin.py show SIM-EAR-005
uv run python scripts/hub_admin.py show SIM-EAR-006
uv run python scripts/hub_admin.py show SIM-EAR-007
```

Register one slot at a time. `add` refuses to bind the slot if Hub assigns a
different id than the expected `SIM-EAR-00N` (pending credentials are left for
inspection — do not overwrite another slot’s file).

```bash
./scripts/sim-ear-observation add --ear d
# must print SIM-EAR-004
uv run python scripts/hub_admin.py set-installation SIM-EAR-004 \
  --latitude 59.3655000 --longitude 18.0900000 \
  --height-m 95 --orientation-deg 180

./scripts/sim-ear-observation add --ear e
uv run python scripts/hub_admin.py set-installation SIM-EAR-005 \
  --latitude 59.3180000 --longitude 18.1000000 \
  --height-m 88 --orientation-deg 0

./scripts/sim-ear-observation add --ear f
uv run python scripts/hub_admin.py set-installation SIM-EAR-006 \
  --latitude 59.3550000 --longitude 17.9950000 \
  --height-m 80 --orientation-deg 135

./scripts/sim-ear-observation add --ear g
uv run python scripts/hub_admin.py set-installation SIM-EAR-007 \
  --latitude 59.3120000 --longitude 17.9800000 \
  --height-m 110 --orientation-deg 45
```

After map review, retune a placement without re-registering:

```bash
uv run python scripts/hub_admin.py set-installation SIM-EAR-004 \
  --latitude <new> --longitude <new> --height-m 95 --orientation-deg 180
```

Keepalive / publish:

```bash
./scripts/sim-ear-observation publish --ear d
./scripts/sim-ear-keepalive once --ear d
./scripts/sim-ear-keepalive watch --ear e
./scripts/sim-ear-keepalive watch --ear all   # every a–g file that exists
./scripts/sim-ear-keepalive once --ear-profile shared/simulator/ear-profiles/ade.json
```

### EAR profiles (preferred for scenarios)

An **EAR profile** only selects which installed simulator devices participate.
A **scenario** defines source motion, detection windows, and observations.
Keep them independent so the same profile can be reused across scenarios.

Profiles live under `shared/simulator/ear-profiles/` (committed; no secrets).
Credential paths are resolved centrally through `sim_ear_slots`
(`shared/simulator/sim-ear-{slot}.json`).

Format:

```json
{
  "name": "abcdefg",
  "ears": {
    "ear_a": "a",
    "ear_b": "b",
    "ear_c": "c",
    "ear_d": "d",
    "ear_e": "e",
    "ear_f": "f",
    "ear_g": "g"
  }
}
```

Initial profiles: `abc.json`, `ade.json`, `abde.json`, `abcde.json`,
`abcdefg.json`.

```bash
# Helicopter baseline (A–C) — dry-run
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --ear-profile shared/simulator/ear-profiles/abc.json

# Same profile — MQTT run
./scripts/sim-ear-scenario run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --ear-profile shared/simulator/ear-profiles/abc.json \
  --time-scale 0.1

# Keepalive for the slots listed in a profile (aliases ignored; slot order kept)
./scripts/sim-ear-keepalive watch \
  --ear-profile shared/simulator/ear-profiles/abcdefg.json
```

Do not mix `--ear-profile` with `--ear` (or keepalive `--credentials-file`) in
one invocation. Missing credential files, duplicate aliases/slots, unsupported
slots, and invalid JSON are hard errors.

**Custom profile:** copy an initial file, edit `name` and the `ears` map
(scenario alias → slot letter), save under `shared/simulator/ear-profiles/`.
Do not put credentials or Hub install coordinates in the profile.

**Legacy** individual mappings remain supported:

```bash
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --ear ear_c=shared/simulator/sim-ear-c.json
```

When a future scenario defines `ear_d`…`ear_g`, prefer a matching profile
(`ade`, `abcdefg`, …) instead of hand-written `--ear` lines.---

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

**Helicopter east→west** (`helicopter_east_west.json`) — shared east→west route; closest approaches roughly B, then C/A (Ear A sits nearer the track than the old northern site). Leave this file unchanged when adding ADE or other estate scenarios.

Default `--mode scripted` keeps the authored observation list (reproducible baseline; exclusive per-Ear windows by design). Authored report spacing is irregular and averages roughly **18–19 s** — it is **not** a strict 20-second timer.

**Helicopter ADE overlap** (`helicopter_ade_overlap.json`) — simultaneous three-SIM-EAR helicopter pass through the A–D–E triangle (Hub installs for SIM-EAR-001/004/005). Authored synchronized reports at **0 / 10 / 20 / 30 / 40 s** on all three Ears (one Observation each; START/PERIODIC×3/END). Use `--ear-profile shared/simulator/ear-profiles/ade.json`. Site baselines are multi-km — Hub analysis needs `--allow-long-range-sim-ears`. Do not apply `helicopter_ew_10s` here; cadence is already authored. Ground truth stays post-hoc only. **Keep this file as the three-EAR common-epoch regression baseline.**

```bash
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_ade_overlap.json \
  --ear-profile shared/simulator/ear-profiles/ade.json

./scripts/sim-ear-scenario run \
  --scenario-file simulator/ear/scenarios/helicopter_ade_overlap.json \
  --ear-profile shared/simulator/ear-profiles/ade.json \
  --time-scale 0.1
```

**Helicopter ABDE overlap** (`helicopter_abde_overlap.json`) — same W→E corridor and 40 s / 10 s cadence as ADE, plus SIM-EAR-002 (ear_b, ori 270°). Four synchronized Observations for common-epoch redundancy. Profile: `shared/simulator/ear-profiles/abde.json`. Geometry notes: `docs/diagnostics/ear-abde-overlap-geometry.md`. Multi-km baselines → `--allow-long-range-sim-ears`. Do not apply `helicopter_ew_10s`. **Keep this file as the four-EAR common-epoch regression baseline.**

```bash
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_abde_overlap.json \
  --ear-profile shared/simulator/ear-profiles/abde.json

./scripts/sim-ear-scenario run \
  --scenario-file simulator/ear/scenarios/helicopter_abde_overlap.json \
  --ear-profile shared/simulator/ear-profiles/abde.json \
  --time-scale 0.1
```

**Helicopter ABDE one missing bearing** (`helicopter_abde_one_missing_bearing.json`) — controlled resilience variant of ABDE: identical route, speed, installs, orientations, and 0/10/20/30/40 s schedule, but `ear_b` observations set `bearing_available: false` so reports emit `spatial.bearing: null` (no fabricated degree). All four EARs still produce complete Observations / Event membership; localization may use only A/D/E. Profile remains `abde`. Notes: `docs/diagnostics/ear-abde-one-missing-bearing.md`. Multi-km baselines → `--allow-long-range-sim-ears`. Do not modify ADE/ABDE baseline files for this experiment.

```bash
# Dry-run (no MQTT)
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_abde_one_missing_bearing.json \
  --ear-profile shared/simulator/ear-profiles/abde.json

# MQTT publish (user clears Hub first; do not run from automation)
./scripts/sim-ear-scenario run \
  --scenario-file simulator/ear/scenarios/helicopter_abde_one_missing_bearing.json \
  --ear-profile shared/simulator/ear-profiles/abde.json \
  --time-scale 0.1

# Hub analysis after the MQTT run
uv run python scripts/hub_event_track.py list --allow-long-range-sim-ears

# Map overlay (ephemeral Analysis scratch)
uv run python scripts/hub_map_analysis.py analyze --allow-long-range-sim-ears
uv run python scripts/hub_map_analysis.py publish \
  --dataset helicopter-abde-one-missing-bearing \
  --dataset-title "ABDE one missing bearing" \
  --allow-long-range-sim-ears
```

**Helicopter ABDE EAR B bearing bias** (`helicopter_abde_b_bearing_bias_p10.json`, `helicopter_abde_b_bearing_bias_p20.json`) — controlled Phase 4A variants of the ABDE baseline. Identical route, installs, orientations, cadence, confidence, and lifecycle; only `ear_b` observations set `bearing_bias_deg` to **+10** or **+20** (applied to generated node-relative bearings, `% 360`). Bearings remain present (this is incorrect data, not missing data). Profile: `abde`. Notes: `docs/diagnostics/ear-abde-b-bearing-bias.md`. Multi-km baselines → `--allow-long-range-sim-ears`. Do not modify ADE/ABDE baseline files.

```bash
# Dry-run (+10°)
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_abde_b_bearing_bias_p10.json \
  --ear-profile shared/simulator/ear-profiles/abde.json

# Dry-run (+20°)
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_abde_b_bearing_bias_p20.json \
  --ear-profile shared/simulator/ear-profiles/abde.json

# MQTT publish — run one variant per cleared Hub window (user-driven; not automation)
./scripts/sim-ear-scenario run \
  --scenario-file simulator/ear/scenarios/helicopter_abde_b_bearing_bias_p10.json \
  --ear-profile shared/simulator/ear-profiles/abde.json \
  --time-scale 0.1

uv run python scripts/hub_event_track.py list --allow-long-range-sim-ears

# Optional map overlay after analysis (do not publish from automation)
uv run python scripts/hub_map_analysis.py analyze --allow-long-range-sim-ears
uv run python scripts/hub_map_analysis.py publish \
  --dataset helicopter-abde-b-bias-p10 \
  --dataset-title "ABDE B bearing bias +10" \
  --allow-long-range-sim-ears
```

**Helicopter ABDE EAR B time-varying bearing bias** (`helicopter_abde_b_bearing_bias_time_varying.json`) — Phase 4B. Same ABDE geometry/timing as the baseline; only `ear_b` uses per-observation `bearing_bias_deg` of **0 / +5 / +10 / +20 / +5** at t=0/10/20/30/40. Reuses the Phase 4A field (no new simulator framework). Offline, production common-epoch selection prefers **t=0** (unbiased) and matches the baseline `useful` / 4-contributor result; forced mid-pass epochs become `weak` with B pairs as outliers. Notes: `docs/diagnostics/ear-abde-b-bearing-bias-time-varying.md`.

```bash
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_abde_b_bearing_bias_time_varying.json \
  --ear-profile shared/simulator/ear-profiles/abde.json

# MQTT — user clears Hub first; not automation
./scripts/sim-ear-scenario run \
  --scenario-file simulator/ear/scenarios/helicopter_abde_b_bearing_bias_time_varying.json \
  --ear-profile shared/simulator/ear-profiles/abde.json \
  --time-scale 0.1

uv run python scripts/hub_event_track.py list --allow-long-range-sim-ears

# Optional map overlay (not automated)
uv run python scripts/hub_map_analysis.py analyze --allow-long-range-sim-ears
uv run python scripts/hub_map_analysis.py publish \
  --dataset helicopter-abde-b-bias-time-varying \
  --dataset-title "ABDE B bearing bias time-varying" \
  --allow-long-range-sim-ears
```

Optional named profile `--scripted-cadence-profile helicopter_ew_10s` derives a denser scripted schedule at a **nominal 10 s** interval **inside each existing detection window** (START/END pinned to the window bounds; PERIODICs on the 10 s grid). Route, speed, windows, and the 30 s Observation lifecycle split rule are unchanged. Provenance is recorded on the run (`derived from helicopter_east_west`, cadence 10 s). This is a controlled denser-sampling experiment for scripted comparison — **not** a firmware-equivalence claim. Firmware’s current default PERIODIC interval is **5 s**. More reports do not imply more independent Observations or stronger Event evidence (still one Observation per Ear per original window).

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
# Scripted baseline (authored irregular ~18–19 s spacing; immutable JSON)
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --ear-profile shared/simulator/ear-profiles/abc.json

# Scripted 10 s cadence profile (same route/windows; denser reports only)
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --scripted-cadence-profile helicopter_ew_10s \
  --ear-profile shared/simulator/ear-profiles/abc.json

./scripts/sim-ear-scenario run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --scripted-cadence-profile helicopter_ew_10s \
  --ear-profile shared/simulator/ear-profiles/abc.json \
  --time-scale 0.1

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
| `./scripts/sim-ear-observation add [--ear a\|b\|c\|d\|e\|f\|g]` | New SIM device + one observation |
| `./scripts/sim-ear-observation publish [--ear …]` | Reuse credentials; publish again |
| `./scripts/sim-ear-keepalive once\|watch [--ear a…g\|all]` | Keepalive (`once` = one round; `watch` = continuous) |
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
