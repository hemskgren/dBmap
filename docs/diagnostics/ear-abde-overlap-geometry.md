# ABDE four-EAR helicopter overlap — route geometry

Scenario: `simulator/ear/scenarios/helicopter_abde_overlap.json`  
Profile: `shared/simulator/ear-profiles/abde.json`  
Baseline preserved: `helicopter_ade_overlap.json` (unchanged).

## Installations used

Hub was unreachable from this agent session. Coordinates match:

| Alias | node_id | Lat | Lon | Ori | Source |
|---|---|---:|---:|---:|---|
| ear_a | SIM-EAR-001 | 59.351176 | 18.048664 | 0° | Verified Hub (ADE Phase 3) |
| ear_b | SIM-EAR-002 | 59.335118371024905 | 18.12680373585177 | 270° | Documented Hub / `sim_ear_slots` |
| ear_d | SIM-EAR-004 | 59.36694541493451 | 18.08826956823438 | 180° | Verified Hub (ADE Phase 3) |
| ear_e | SIM-EAR-005 | 59.32006416685073 | 18.09943980553271 | 0° | Verified Hub (ADE Phase 3) |

Confirm with `hub_admin.py list --detail` before a live run if installs may have drifted.

Baselines (m): A–B 4777, A–D 2848, A–E 4501, B–D 4159, B–E 2283, D–E 5251.

## Route

Same W→E corridor as ADE (speed/duration unchanged — not tuned for a nicer score):

| | |
|---|---|
| Start | 59.35224966, 18.05850000 |
| End | 59.35224794472361, 18.07967052913215 |
| Length / speed / duration | 1200 m / 30 m/s / **40.000 s** |
| Reports | 0, 10, 20, 30, 40 s on **all four** Ears |

## Per-epoch diagnostics (2D; height ignored)

| t (s) | Forward ABDE | min∠ (°) | min forward margin (°) | Limiting Ear (margin) |
|---:|:-:|---:|---:|---|
| 0 | YYYY | 30.8 | 12.1 | A |
| 10 | YYYY | 32.5 | 7.9 | A |
| 20 | YYYY | 34.1 | 5.9 | A |
| 30 | YYYY | 35.7 | 4.7 | A |
| 40 | YYYY | 34.0 | 3.9 | A |

| t | dA | dB | dD | dE | nA / nB / nD / nE |
|---:|---:|---:|---:|---:|---|
| 0 | 570 | 4316 | 2349 | 4266 | 77.9 / 26.2 / 45.9 / 327.0 |
| 10 | 866 | 4049 | 2143 | 4110 | 82.1 / 28.1 / 40.3 / 330.6 |
| 20 | 1164 | 3786 | 1963 | 3971 | 84.1 / 30.2 / 33.6 / 334.3 |
| 30 | 1462 | 3530 | 1814 | 3851 | 85.3 / 32.7 / 25.7 / 338.4 |
| 40 | 1762 | 3282 | 1705 | 3750 | 86.1 / 35.5 / 16.6 / 342.6 |

Pair intersection angles (°):

| t | AB | AD | AE | BD | BE | DE |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | 38.3 | 32.0 | 69.1 | 70.3 | 30.8 | 78.9 |
| 10 | 36.0 | 41.7 | 68.5 | 77.8 | 32.5 | 69.8 |
| 20 | 36.1 | 50.5 | 70.2 | 86.6 | 34.1 | 59.3 |
| 30 | 37.4 | 59.6 | 73.0 | 83.1 | 35.7 | 47.4 |
| 40 | 39.4 | 69.5 | 76.5 | 71.1 | 37.1 | 34.0 |

## Limitations

- **EAR A** remains the thinnest forward margin (~4–12°). Same ADE corridor tradeoff; not worsened by adding B.
- **EAR B** ranges ~3.3–4.3 km (sparse). Authored confidence is intentionally modest; do not treat as calibrated SNR.
- Docs table still lists older D/E *proposals*; this scenario uses **verified Hub** D/E from ADE. Reconcile `sim_ear_slots` / docs later if desired — not done here (no install changes).
- Hub unreachable during design; live `list --detail` recommended before MQTT.

## Expected live analysis (after user run)

- 20 reports → **4** Observations (5 reports each).
- **1** Event with ears A,B,D,E.
- `bearing_selection_mode=common_epoch`; one shared epoch for intersections.
- Up to **6** pair intersections; contributors ⊆ Event members.
- If one Ear is excluded at the selected epoch, Event membership stays 4.

Resilience variant (B bearings unavailable, Event membership still 4):  
`helicopter_abde_one_missing_bearing.json` — see `ear-abde-one-missing-bearing.md`.

Phase 4A controlled bias (B bearings present but +10° / +20°):  
`helicopter_abde_b_bearing_bias_p10.json`, `helicopter_abde_b_bearing_bias_p20.json` — see `ear-abde-b-bearing-bias.md`.
