# ABDE resilience — one EAR without usable bearing

Scenario: `simulator/ear/scenarios/helicopter_abde_one_missing_bearing.json`  
ID: `helicopter_abde_one_missing_bearing_01`  
Profile: `shared/simulator/ear-profiles/abde.json`  

Baselines left unchanged:

- `helicopter_ade_overlap.json`
- `helicopter_abde_overlap.json`

## Selected EAR: ear_b (SIM-EAR-002)

Withhold **usable bearings from EAR B only**.

Why B:

- Remaining EARs A/D/E keep the ADE triangle already shown to localize as `useful` under common-epoch selection.
- B is the sparsest long-range EAR on this corridor (~3.3–4.3 km); missing bearing there is a realistic failure mode without changing installs, route, speed, or other EAR bearings.
- Removing A would drop the closest EAR; removing D or E would leave a less-characterized ADE subset.

Installation coordinates, orientations, source trajectory (1200 m @ 30 m/s), synchronized timestamps (0/10/20/30/40 s), and Observation windows match ABDE.

## How missing bearing is represented

Simulator observations for `ear_b` set `bearing_available: false` and omit `bearing_confidence`.

`load_scenario` then **does not** compute `bearing_deg`. Emission reuses the existing payload path:

```text
spatial.bearing = null
```

That matches Hub/protocol acceptance of null bearings and production assembly (`bearing_deg is None`, empty `bearing_samples`). No fabricated degree is introduced. No production association/localization changes.

## Offline analysis (production logic, no MQTT)

| Check | Result |
|---|---|
| Reports | 20 (5 per EAR) |
| Complete Observations | **4** |
| Events | **1** |
| **Event membership** | SIM-EAR-001, **002**, 004, 005 (**4**) |
| Usable bearings | A, D, E only — B assembled `bearing_deg=None` |
| `bearing_selection_mode` | `common_epoch` |
| Common-epoch excluded | SIM-EAR-002 — no bearing sample within 1 s of epoch |
| Forward / aligned / consensus pairs | **3 / 3 / 3** (ADE pairs only) |
| Outlier pairs | `[]` |
| **Localization contributors** | SIM-EAR-001, 004, 005 (**3**) — **not** 002 |
| Localization status | **`useful`** (existing quality rules) |

**Event membership ≠ localization contribution:** B stays an Event member; it is excluded only from common-epoch participation and contributors.

Baseline ABDE (unchanged file) still yields 4 contributors, 6 forward pairs, `useful`.
