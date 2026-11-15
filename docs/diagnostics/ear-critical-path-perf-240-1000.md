# EAR performance evaluation — 240 MHz / 1000 Hz / Critical Path

**Branch context:** `firmware/r0-esp32-s3-measurement-baseline` (+ this preparation)  
**R0 reference:** 160 MHz / FreeRTOS 100 Hz — see [`r0-esp32-s3-measurement-baseline.md`](./r0-esp32-s3-measurement-baseline.md)  
**Status:** Investigation + experimental overlays + LAB impulse harness prepared.  
**Not done:** device flash/OTA, filled soak records, production Critical detector.

---

## 1. Critical Path — current implementation status

### Verified in source

| Item | Status | Location |
| --- | --- | --- |
| Lifecycle module | **Exists** | `ear_critical_path.c` / `.h`, `CRITICAL_PATH.md` |
| Compile default | `CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH` default **y** (depends on AE) | `ear_observation/Kconfig` |
| Device wiring | Listener on AE signatures + `ear_critical_poll` on `ear_obs` task | `ear_observation.c` |
| Capacity | 1 active; second START → `drop_busy` | verified |
| Evidence on START | By-value copy of `ae_signature_t` into `cp->frozen` | `start_active` |
| CE / registry | Independent; no CE promote; no Platform slot | verified |
| Hub origin | `context.origin=critical_path` when publishing | payload + Hub |
| Auto acoustic START (production) | **Not implemented** | `ear_critical_on_signature` always blocks START |
| `CRITICAL_AUTO_DETECT` | Kconfig exists; even if set, device forces `auto_detect_enabled=false` and logs warning | verified |
| Host tests | Lifecycle + integration (force_start) | `test_ear_critical_path.c`, `test_ear_critical_integration.c` |
| Device MQTT soak | **Outstanding** | CRITICAL_PATH.md |

### Execution context (verified)

| Aspect | Value |
| --- | --- |
| Task | Same `"ear_obs"` task (prio **4**, stack 6144) as activity / AE poll / CE / Manager |
| Audio path | `sp_mic` (prio 5) → window job → `ae_device_poll` analyze → `ae_publish_signature` → **sync** listener `critical_on_ae_signature` |
| Poll | `ear_critical_poll` after promotion/Manager each loop (`vTaskDelay(100)` ms between loops) |
| Competing work | Shares CPU with AE DSP, CE, promotion, normal Obs publish on the same task |

**Critical Path is not a separate high-priority task.** “ON” today means lifecycle module initialized + AE listener bound; it does **not** mean acoustic events auto-START.

### Short-lived impulse preservation (verified limits)

- Evidence is only as fine as the **completed analysis window** (default **1000 ms**).
- Mid-window / sub-window detection: **absent**.
- While active, further signatures only idle-reset via experimental fingerprint match; no second Critical Obs; no overflow evidence buffer (option A).
- PCM overwrite can drop a window before AE/Critical ever sees it (`sp_mic_pcm_pending_overwrite`).

### LAB impulse harness (this preparation)

| Item | Detail |
| --- | --- |
| Kconfig | `DBMAP_EAR_OBS_CRITICAL_LAB_IMPULSE` (default **n**) |
| Behavior | On idle + usable sig, if `crest_db >= lab_crest` **OR** `peak_dbfs >= lab_peak`, call `start_active` / force_start |
| Purpose | Enable matrix **C/D** timing of detect→preserve — **not** gunshot classification |
| Overlay | `sdkconfig.defaults.critical_lab_impulse` |

---

## 2. Configuration symbols

| Symbol | Role |
| --- | --- |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_{160,240}` / `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` | Build-time CPU |
| `CONFIG_FREERTOS_HZ` | FreeRTOS tick (`configTICK_RATE_HZ`) |
| `CONFIG_DBMAP_EAR_OBS_CRITICAL_PATH` | Lifecycle compile-in |
| `CONFIG_DBMAP_EAR_OBS_CRITICAL_LAB_IMPULSE` | LAB START harness |
| `CONFIG_DBMAP_EAR_OBS_CRITICAL_LAB_CREST_FLOOR_DB_X1000` | Lab crest ×1000 |
| `CONFIG_DBMAP_EAR_OBS_CRITICAL_LAB_PEAK_DBFS_X1000` | Lab peak ×1000 |
| `CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG` / `AE_DIAG` | UART timing dumps |

**Product `sdkconfig.defaults`:** **240 MHz / FreeRTOS 100 Hz** (fleet default).
This evaluation’s B/D cells still use `perf_240_1000` for **1000 Hz tick** only;
do not treat 1000 Hz as a product setting. Lab impulse stays overlay-only.

Runtime verify (UART `sp: res`):

- `cpu_mhz=` ← `esp_clk_cpu_freq()`
- `configured_mhz=` ← `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`
- `tick_hz=` ← `CONFIG_FREERTOS_HZ` (compile-time; confirm build)

I2S/DMA acquisition does **not** depend on FreeRTOS tick rate.

---

## 3. Missing prerequisites (before claiming full matrix)

| Prerequisite | Needed for | Status |
| --- | --- | --- |
| Production Critical auto-detector | Real “gunshot detection” claims | **Missing** (by design) |
| LAB impulse overlay flashed | C/D acoustic START path | Prepared; **not flashed** |
| Tuned lab crest/peak for stimulus | Reliable triggers without constant drop_busy | Operator tune |
| Sub-window detector | Latency << window remainder | **Out of scope** / absent |
| Device R0 baseline records | A as measured reference | Templates exist; soaks pending |
| Separate Critical task | Isolate CP from AE/CE on same task | Not present; measure as-is |

**Matrix support with LAB harness:** all four rows are buildable. Without LAB, C/D cannot exercise acoustic START (only force_start from host/tests).

---

## 4. Experimental builds (operator)

**Important:** `SDKCONFIG_DEFAULTS` only applies when **`sdkconfig` does not already
exist**. Later files in the chain override earlier ones. Put **`perf_240_1000`
after `r0_ref`** so 240/1000 wins over 160/100.

| Matrix | SDKCONFIG_DEFAULTS chain (order matters) |
| --- | --- |
| **A** R0 ref, CP lab off | `sdkconfig.defaults;sdkconfig.defaults.r0_ref` |
| **B** 240/1000, CP lab off | `sdkconfig.defaults;sdkconfig.defaults.r0_ref;sdkconfig.defaults.perf_240_1000` |
| **C** R0 ref, CP lab on | `sdkconfig.defaults;sdkconfig.defaults.r0_ref;sdkconfig.defaults.critical_lab_impulse` |
| **D** 240/1000, CP lab on | `sdkconfig.defaults;sdkconfig.defaults.r0_ref;sdkconfig.defaults.perf_240_1000;sdkconfig.defaults.critical_lab_impulse` |

Example (matrix A):

```bash
cd firmware/esp-ear
# Backup then remove sdkconfig so defaults apply:
cp sdkconfig sdkconfig.backup.r0 && rm -f sdkconfig sdkconfig.old
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.r0_ref" set-target esp32s3 build
# Verify in generated sdkconfig: CPU=160, FREERTOS_HZ=100
# After boot: sp: res … cpu_mhz=160 configured_mhz=160 tick_hz=100
```

Verify before flash (grep generated `sdkconfig`):

- A/C: `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`, `CONFIG_FREERTOS_HZ=100`
- B/D: `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240`, `CONFIG_FREERTOS_HZ=1000`
- C/D: `CONFIG_DBMAP_EAR_OBS_CRITICAL_LAB_IMPULSE=y`

Same hardware, same mic count, same stimulus across A–D.

---

## 5. Test matrix and measurements

### Matrix

| Test | CPU | Tick | Critical acoustic START |
| --- | ---: | ---: | --- |
| A | 160 | 100 | OFF (lab impulse off) |
| B | 240 | 1000 | OFF |
| C | 160 | 100 | ON via **LAB** crest/peak (not production detector) |
| D | 240 | 1000 | ON via **LAB** |

### Measurement definitions (wall µs via `esp_timer` / signature times)

| Metric | Definition | Source |
| --- | --- | --- |
| Window available → listener | `now - sig.window_end_mono_us` at lab decision | `lab_detect_us_*` |
| Window → evidence frozen | detect + `force_start` wall | `lab_preserve_us_*` |
| Critical force_start cost | inside `start_active` incl. START publish attempt | `lab_force_us_*` |
| AE analyze / post_dsp | R0 AE timing | `ae: timing` |
| promo_mgr / publish | R0 stage lines | `ear_obs: R0 stage` |
| Overwrite / drops | PCM overwrite, `drop_busy`, `evq_drop`, CE create drop | existing counters |
| Heap / stack / cpu / tick | `sp: res` | R0 diag |
| Scheduling note | `ear_obs` loop uses `vTaskDelay(100)` — tick 1000 Hz improves delay resolution but **does not** speed I2S; largest CP latency often **remainder of 1 s window** | document, do not confuse with tick |

No FreeRTOS tick counts as timing substitutes.

### Stimulus (system response, not classifier accuracy)

1. **Idle:** quiet room 10 min — counters, heap, overwrite=0.
2. **Impulse:** short high-crest click / clap / starter-pistol sim at known wall times (phone metronome / scope if available); ≥20 impulses, ≥2 s apart so capacity-1 can END (idle 15 s or force shorter max for lab).
3. **Concurrent Obs:** raise ambient so normal RMS Obs may be open during impulses.
4. **Load:** continuous broadband + impulses; watch overwrite and `drop_busy`.

Tune LAB floors so idle does not trigger; impulses do. Record floors on the Map record.

**Do not** claim gunshot detection performance from this stimulus.

### Soak duration (suggested)

- Idle 10 min + impulse series ~10 min per matrix cell.
- Enough for timing averages; not `verified_operating` fleet soak.

---

## 6. Recommended next implementation steps (if extending)

1. Operator: flash A→D, fill Map templates with identity + Critical `detect/preserve` lines.  
2. Optionally shorten `CRITICAL_MAX_ACTIVE_MS` in a **lab-only** overlay for denser impulses (not done here).  
3. Later ADR: completed-window vs mid-window detector — out of this experiment.  
4. Only after measured benefit: consider pinning 240/1000 in product defaults (separate decision).

---

## 7. Files touched for this preparation

| Path | Role |
| --- | --- |
| `ear_critical_path.c/.h` | LAB impulse + timing diag |
| `ear_observation/Kconfig` | LAB Kconfig |
| `ear_observation.c` | Wire LAB config; UART `critical diag` |
| `sp_diag` | Log `tick_hz` |
| `sdkconfig.defaults.r0_ref` | R0 pin 160/100 + diag |
| `sdkconfig.defaults.perf_240_1000` | Experimental 240/1000 |
| `sdkconfig.defaults.critical_lab_impulse` | LAB START on |
| This doc | Matrix + status |

Product defaults and Candidate/Observation policies are unchanged.
