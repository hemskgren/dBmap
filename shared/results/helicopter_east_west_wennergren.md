
script:
./scripts/sim-ear-scenario run   --scenario-file simulator/ear/scenarios/helicopter_east_west.json   --ear ear_a=shared/simulator/sim-ear-a.json   --ear ear_b=shared/simulator/sim-ear-b.json   --ear ear_c=shared/simulator/sim-ear-c.json  
Scenario helicopter_ew_01 publishing 18 observations (mode=scripted):
  source_family=helicopter  speed=45 m/s  source=SIM-HELI-001
  movement=14002.1m / 311.2s logical  wall-clock≈300.0s at --time-scale 1
  keepalive (start):
    a  SIM-EAR-001
    b  SIM-EAR-002
    c  SIM-EAR-003
01  t=12s  b  22fe2136-4dfe-4b44-b43d-cfb9d102ef6b
02  t=40s  b  f98b1e8d-b995-41c8-abad-7bdde17dba00
03  t=60s  b  298d48b0-a2b3-4ffb-aeb8-83260206a869
04  t=72s  b  f9be3942-87eb-4c34-9c9f-0c71d74b4b59
05  t=90s  b  3fb0ab0b-309f-4573-a50f-e1e52a78239c
06  t=105s  b  7ac43e31-b8fe-4b0a-9a9a-1d83c287c71a
07  t=112s  c  f683e134-fd89-42a0-b5c2-59e785879fd3
08  t=120s  c  b6a770d2-b0e0-4ebe-ac34-85db4f58d18d
09  t=150s  c  75b9417a-accc-431e-955e-c1b2d6d3f68b
10  t=164s  c  42f1b687-6210-467e-ac3e-f2c4182df60a
11  t=185s  c  4855a2bd-0be6-451f-8b9e-313814a803c2
12  t=200s  c  68f1564d-07de-4d14-82eb-edc4bcb2ebca
13  t=205s  a  5bf6d78f-cadb-4e08-b59f-a4919a3678b5
14  t=210s  a  52e800b5-f216-4632-94e8-27e7459ff3c9
15  t=230s  a  46341d1f-2fb2-4a5d-86a3-4d2d9e5788f5
16  t=250s  a  0ba58494-88a2-4b5a-aaeb-79837594876c
17  t=280s  a  89d0970a-9f06-4998-a9f2-c15b27079723
18  t=300s  a  3638e6af-eda9-403d-9cc9-a0ded50edb31
  keepalive (end):
    a  SIM-EAR-001
    b  SIM-EAR-002
    c  SIM-EAR-003
Scenario helicopter_ew_01 finished: 18/18 MQTT publishes acknowledged.



4km:
./scripts/sim-ear-scenario run   --scenario-file simulator/ear/scenarios/helicopter_east_west.json   --ear ear_a=shared/simulator/sim-ear-a.json   --ear ear_b=shared/simulator/sim-ear-b.json   --ear ear_c=shared/simulator/sim-ear-c.json   --mode independent_detection   --max-range-m 4000
Scenario helicopter_ew_01 publishing 497 observations (mode=independent_detection):
  source_family=helicopter  speed=45 m/s  source=SIM-HELI-001
  movement=14002.1m / 311.2s logical  wall-clock≈256.0s at --time-scale 1
  keepalive (start):
    a  SIM-EAR-001
    b  SIM-EAR-002
    c  SIM-EAR-003



Independent detection diagnostics (Stage 1 range gate):
  mode=independent_detection  time_step=1s  default_max_range=4000m
  steps=313  ears=3  evaluations=939  observations=497
  ear_a: detections=171/313  range≤4000m  dist[1126m,7773m]  CPA t=171s@1126m  intervals=[86,256]s
  ear_b: detections=160/313  range≤4000m  dist[639m,10775m]  CPA t=72s@639m  intervals=[0,159]s
  ear_c: detections=166/313  range≤4000m  dist[1471m,7541m]  CPA t=164s@1471m  intervals=[82,247]s
  simultaneous ≥2 Ears: [82,247]s
  simultaneous 3 Ears: [86,159]s
  note: Detection model: deterministic range gate (Stage 1).
  note: Not an acoustic propagation model; distance-only threshold.
  note: Synthetic signal_level_dbfs=-40.0 (placeholder, not physics).
  note: Synthetic classification_confidence=0.5 (not calibrated).
  note: Bearing confidence omitted (unavailable / not calibrated).
  note: All configured Ears are evaluated at every shared simulation step.
Scenario helicopter_ew_01 finished: 497/497 MQTT publishes acknowledged.
Use scripts/hub_event_track.py to inspect the observations; no Hub logic was deployed.



5km:
$ ./scripts/sim-ear-scenario run   --scenario-file simulator/ear/scenarios/helicopter_east_west.json   --ear ear_a=shared/simulator/sim-ear-a.json   --ear ear_b=shared/simulator/sim-ear-b.json   --ear ear_c=shared/simulator/sim-ear-c.json   --mode independent_detection   --max-range-m 5000
Scenario helicopter_ew_01 publishing 612 observations (mode=independent_detection):
  source_family=helicopter  speed=45 m/s  source=SIM-HELI-001
  movement=14002.1m / 311.2s logical  wall-clock≈279.0s at --time-scale 1
  keepalive (start):
    a  SIM-EAR-001
    b  SIM-EAR-002
    c  SIM-EAR-003

612  t=279s  a  00916b24-1088-41ef-ba04-93d74a9324e9
  keepalive (end):
    a  SIM-EAR-001
    b  SIM-EAR-002
    c  SIM-EAR-003
Independent detection diagnostics (Stage 1 range gate):
  mode=independent_detection  time_step=1s  default_max_range=5000m
  steps=313  ears=3  evaluations=939  observations=612
  ear_a: detections=217/313  range≤5000m  dist[1126m,7773m]  CPA t=171s@1126m  intervals=[63,279]s
  ear_b: detections=183/313  range≤5000m  dist[639m,10775m]  CPA t=72s@639m  intervals=[0,182]s
  ear_c: detections=212/313  range≤5000m  dist[1471m,7541m]  CPA t=164s@1471m  intervals=[59,270]s
  simultaneous ≥2 Ears: [59,270]s
  simultaneous 3 Ears: [63,182]s
  note: Detection model: deterministic range gate (Stage 1).
  note: Not an acoustic propagation model; distance-only threshold.
  note: Synthetic signal_level_dbfs=-40.0 (placeholder, not physics).
  note: Synthetic classification_confidence=0.5 (not calibrated).
  note: Bearing confidence omitted (unavailable / not calibrated).
  note: All configured Ears are evaluated at every shared simulation step.
Scenario helicopter_ew_01 finished: 612/612 MQTT publishes acknowledged.


---

## Stage 2 — Distance-dependent synthetic signal levels

Model (independent_detection only):

`signal_level_dbfs = reference_level_dbfs - 20 * path_loss_exponent * log10(distance_m / reference_distance_m)`

Defaults: reference_level=−30 dBFS, reference_distance=1000 m, path_loss_exponent=1.0.
Min distance for log: 1 m. Levels explicitly clamped to ≤0 dBFS (protocol).
Detection eligibility unchanged (Stage 1 range gate only).

### 4 km — default signal parameters

```bash
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --mode independent_detection --max-range-m 4000 \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --ear ear_c=shared/simulator/sim-ear-c.json
```

Independent detection diagnostics (Stage 1 range gate + Stage 2 levels):
  mode=independent_detection  time_step=1s  default_max_range=4000m
  signal model (synthetic): reference_level=-30 dBFS  reference_distance=1000m  path_loss_exponent=1
  steps=313  ears=3  evaluations=939  observations=497
  ear_a: detections=171/313  range≤4000m  dist[1126m,7773m]  CPA t=171s@1126m (level -31.03 dBFS)  signal[-42.02,-31.03] dBFS  intervals=[86,256]s
  ear_b: detections=160/313  range≤4000m  dist[639m,10775m]  CPA t=72s@639m (level -26.11 dBFS)  signal[-41.96,-26.11] dBFS  intervals=[0,159]s
  ear_c: detections=166/313  range≤4000m  dist[1471m,7541m]  CPA t=164s@1471m (level -33.35 dBFS)  signal[-42.04,-33.35] dBFS  intervals=[82,247]s
  simultaneous ≥2 Ears: [82,247]s
  simultaneous 3 Ears: [86,159]s

Compared with Stage 1 baseline at 4 km: detections/intervals/observations **identical** (497; A 171 / B 160 / C 166). Signal levels now vary with distance (CPA loudest for each Ear).

### 5 km — default signal parameters

```bash
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --mode independent_detection --max-range-m 5000 \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --ear ear_c=shared/simulator/sim-ear-c.json
```

  steps=313  ears=3  evaluations=939  observations=612
  ear_a: detections=217/313  CPA t=171s@1126m (level -31.03 dBFS)  signal[-43.97,-31.03] dBFS  intervals=[63,279]s
  ear_b: detections=183/313  CPA t=72s@639m (level -26.11 dBFS)  signal[-43.95,-26.11] dBFS  intervals=[0,182]s
  ear_c: detections=212/313  CPA t=164s@1471m (level -33.35 dBFS)  signal[-43.94,-33.35] dBFS  intervals=[59,270]s
  simultaneous ≥2 Ears: [59,270]s
  simultaneous 3 Ears: [63,182]s

Compared with Stage 1 baseline at 5 km: detections/intervals/observations **identical** (612; A 217 / B 183 / C 212).

### 4 km — path_loss_exponent=2

```bash
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --mode independent_detection --max-range-m 4000 --path-loss-exponent 2 \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --ear ear_c=shared/simulator/sim-ear-c.json
```

  observations=497 (unchanged vs Stage 1 / default Stage 2)
  ear_a: detections=171  CPA level -32.07 dBFS  signal[-54.05,-32.07] dBFS  intervals=[86,256]s
  ear_b: detections=160  CPA level -22.21 dBFS  signal[-53.91,-22.21] dBFS  intervals=[0,159]s
  ear_c: detections=166  CPA level -36.70 dBFS  signal[-54.08,-36.70] dBFS  intervals=[82,247]s

Detection counts/intervals unchanged; levels drop faster with distance (≈12 dB per doubling).


---

## Stage 3 — Observation emission (temporal sampling)

Policy (independent_detection only): evaluate every Ear every 1 s (Stage 1 unchanged).
Emit observations with an episode-relative fixed interval:
- start of each Ear detection episode
- every `--observation-interval-seconds` while detected (relative to episode start)
- end of episode (single observation if end coincides with a periodic time)

Default interval: **5 s**. Detection counts/intervals and Stage 2 CPA/signal ranges are unchanged vs Stage 1/2 for the same range.

### 4 km — observation interval 5 s

```bash
./scripts/sim-ear-scenario dry-run \
  --scenario-file simulator/ear/scenarios/helicopter_east_west.json \
  --mode independent_detection --max-range-m 4000 \
  --observation-interval-seconds 5 \
  --ear ear_a=shared/simulator/sim-ear-a.json \
  --ear ear_b=shared/simulator/sim-ear-b.json \
  --ear ear_c=shared/simulator/sim-ear-c.json
```

  steps=313  evaluations=939  positive_detections=497  emitted_observations=102
  (start=3, periodic=96, end=3)
  ear_a: detections=171  episodes=1  emitted=35  intervals=[86,256]s
  ear_b: detections=160  episodes=1  emitted=33  intervals=[0,159]s
  ear_c: detections=166  episodes=1  emitted=34  intervals=[82,247]s
  simultaneous ≥2 Ears: [82,247]s
  simultaneous 3 Ears: [86,159]s

Sample emissions (ear_b): t=0 start, 5/10/… periodic, 159 end.
Compared with Stage 1/2 at 4 km: detections **497** unchanged; emitted **102** (was 497).

### 5 km — observation interval 5 s

  positive_detections=612  emitted_observations=127  (start=3, periodic=121, end=3)
  ear_a: detections=217  emitted=45  intervals=[63,279]s
  ear_b: detections=183  emitted=38  intervals=[0,182]s
  ear_c: detections=212  emitted=44  intervals=[59,270]s
  simultaneous ≥2 Ears: [59,270]s
  simultaneous 3 Ears: [63,182]s

Compared with Stage 1/2 at 5 km: detections **612** unchanged; emitted **127** (was 612).

### 4 km — observation interval 10 s

  positive_detections=497  emitted_observations=53  (start=3, periodic=47, end=3)
  ear_a: emitted=18  intervals=[86,256]s (unchanged)
  ear_b: emitted=17  intervals=[0,159]s
  ear_c: emitted=18  intervals=[82,247]s

Detection intervals unchanged; only emission density changes with the reporting interval.
