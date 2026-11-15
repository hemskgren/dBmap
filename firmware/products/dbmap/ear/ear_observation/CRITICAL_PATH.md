# Critical Observation Path (lifecycle slice)

EAR-owned Critical Observation lifecycle per ADR 0003. **Automatic acoustic
detection is hard-disabled** in this slice.

## What exists in source

- Module: `ear_critical_path.c` / `include/ear_critical_path.h`
- Device wiring in `ear_observation.c` when `CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH`
- Hub accepts optional `context.origin = "critical_path"`
- Payload builder emits `origin` only when set
- Capacity 1; second START while active → drop + `diag.drop_busy`
- No queue, overflow buffer, or event merging
- Independent of CE and normal Platform registry slots
- OTA/shutdown calls `ear_critical_close_for_ota` (registry walk alone is not enough)

## Timing (configurable Kconfig; provisional defaults)

| Parameter | Default | Notes |
| --- | --- | --- |
| PERIODIC | 5000 ms | Implementation default — not a final architectural decision |
| Idle timeout | 15000 ms | Resets only on experimental fingerprint match |
| Max active | 30000 ms | Hard END |

## Detection disabled (production)

- `CONFIG_DBMAP_EAR_OBS_CRITICAL_AUTO_DETECT` defaults **n**
- Production path: `ear_critical_on_signature` does **not** open a Critical
  Observation (no approved production trigger)
- Crest/peak offline harness results are **not** production criteria

## LAB impulse harness (measurement only)

- `CONFIG_DBMAP_EAR_OBS_CRITICAL_LAB_IMPULSE` defaults **n**
- When enabled (see `sdkconfig.defaults.critical_lab_impulse`), idle signatures
  meeting provisional crest/peak floors call `force_start` and record
  `lab_detect_us_*` / `lab_preserve_us_*` / `lab_force_us_*` for 240 MHz /
  1000 Hz experiments ([`docs/diagnostics/ear-critical-path-perf-240-1000.md`](../../../docs/diagnostics/ear-critical-path-perf-240-1000.md))
- **Not** gunshot classification; must not ship as a product default

## How to test the lifecycle without enabling detection

**Host (unit):**

```bash
cc -std=c11 -Wall -Wextra -Werror -O2 -g \
  -I firmware/products/dbmap/ear/audio_engine/include \
  -I firmware/products/dbmap/ear/ear_acoustic/include \
  -I firmware/products/dbmap/ear/ear_observation/include \
  -I firmware/platform/observation/include \
  firmware/products/dbmap/ear/audio_engine/ae_signature.c \
  firmware/products/dbmap/ear/ear_observation/ear_critical_path.c \
  firmware/platform/observation/episode_registry.c \
  firmware/products/dbmap/ear/ear_observation/test_host/test_ear_critical_path.c \
  -lm -o /tmp/test_ear_critical_path && /tmp/test_ear_critical_path
```

**Host (integration — Tests 4 & 5):** see header of
`test_host/test_ear_critical_integration.c` for the full `cc` line, then run
`/tmp/test_ear_critical_integration`.

- Test 4: `test4_critical_with_both_normal_slots_occupied`
- Test 5: `test5_ota_cleanup_while_critical_active`

Use `ear_critical_force_start` / `ear_critical_inject_evidence` / `ear_critical_poll`.
OTA product cleanup uses `ear_obs_ota_release_product_bindings` (same helper as
device `ear_observation_close_for_ota`).

**Continuation match (experimental only):** `match_distance_max` default 4.0 and
`match_crest_floor_db` default 12.0 — idle-reset only, not approved detectors.

## END vs L0 publication

Lifecycle END always clears the Critical binding **before** publish. If MQTT/L0
payload publication fails, the binding stays released (`diag.publish_fail`).
Missing L0 must not leave Critical active indefinitely.

## Not verified on hardware yet

Device soak, Hub MQTT round-trip of Critical reports, and physical EAR
verification remain outstanding. Do not claim full Critical Path verification
from host tests alone.
