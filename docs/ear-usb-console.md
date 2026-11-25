# EAR USB serial console (multi-device)

Read-only UART log access for EAR boards over USB. Baud is **115200**. The Hub
is not on this path.

Use this when you need live `ear_obs` / AE / Platform lines from a physical EAR.
Always name the **exact** serial device when more than one ESP32 is plugged in.

## Pick the right `/dev/tty*`

Several EARs (or other ESP32 boards) often appear as `/dev/ttyACM0`,
`/dev/ttyACM1`, … (CH340 adapters commonly use `ttyACM*`; some boards use
`ttyUSB*`).

```bash
# List ports + stable USB IDs (preferred for multi-device labs)
ls -la /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
ls -la /dev/serial/by-id/

# Example by-id names (lab-dependent):
#   usb-1a86_USB_Single_Serial_<SERIAL>-if00 -> ../../ttyACM0
```

**Pass the port explicitly** to any tool (never assume `ttyACM0` is “the EAR”):

| Situation | What to do |
| --- | --- |
| Operator / agent told a port | Use that path only (e.g. `/dev/ttyACM0`) |
| Unclear which board | Ask, or match `by-id` serial to the labeled cable/board |
| Wrong port open | You will see another product’s logs or silence — switch port |

Linux permission: user must be in group `dialout` (see
[device registration](./device-registration-and-deployment.md)).

```bash
groups | tr ' ' '\n' | grep -x dialout
test -r /dev/ttyACM0 && echo readable
```

## Open the console

Set `PORT` to the chosen device.

### Quick read (no ESP-IDF)

```bash
PORT=/dev/ttyACM0   # or /dev/ttyACM1, /dev/ttyUSB0, …
stty -F "$PORT" 115200 raw -echo
timeout 5 cat "$PORT"
```

Useful for a short smoke check that EAR lines appear (`ear_obs:`, `ae:`, etc.).

### ESP-IDF monitor (preferred for interactive sessions)

This repo’s IDF install is typically under `$HOME/esp/esp-idf` (same tree the
firmware builder / local Ear builds use). `idf.py` is **not** on PATH until the
environment is sourced:

```bash
PORT=/dev/ttyACM0
. "$HOME/esp/esp-idf/export.sh"
cd firmware/esp-ear
idf.py -p "$PORT" monitor
```

Leave the monitor with `Ctrl+]`.

Flash + monitor (only when flashing was requested):

```bash
idf.py -p "$PORT" flash monitor
```

Alternatives: `screen "$PORT" 115200` or `picocom -b 115200 "$PORT"`.

## What “EAR log” looks like

With serial diagnostics enabled on the image, expect recurring lines such as:

```text
I (…) ear_obs: diag snap … rms_dbfs=… state=ACTIVE …
I (…) ear_obs: L0 win rms=… B0=… B1=… …
```

Exact tags depend on the flashed build (`serial_diag` / AE diag overlays). See
[firmware/esp-ear/README.md](../firmware/esp-ear/README.md) for field meanings.

### Correlating audio windows and reports

- Match capture and Audio Engine records by node, boot/session and `window_seq`.
  The internal window job's `collection_start_mono_us` is the start of the
  software collection loop. `pcm.window_end_mono_us` is its end stamp, taken
  after reading and before finalizing levels. Both use the device monotonic
  clock. Queued DMA and blocking reads prevent either timestamp from specifying
  the exact acquisition time of a physical sample.
- `window_ms` is the configured nominal analysis duration. Divide each channel's
  captured or retained sample count by the nominal sample rate to get that
  span's nominal sample duration. The internal job pairs
  `channels[i].sample_count` (captured) with `pcm.sample_count_ch[i]`
  (retained). Their difference for a projected PCM channel is the cap discard.
  The job also carries the window's incomplete stereo count and the existing
  channel transport counters. A timeout or partial read does not quantify
  missing frames. Zero faults and zero cap discards leave transport continuity
  unverified. Sequence holes and pending overwrites track missing jobs separately.
- Audio Engine `analyze_us` is elapsed processing wall time, including
  preemption. `post_dsp_us` measures the following stage, including synchronous
  listeners. The capture `measure_window_us` diagnostic includes window
  finalization and publication work, whereas the internal collection end stamp
  precedes that work.
- MQTT `measured_at_uptime_s` is the collection end truncated to seconds.
  Report `uptime_s` is sampled separately during payload construction.
  Hub `last_seen_at` is the Hub's UTC time when it applies the reported
  state. Uptime differences give approximate snapshot age within a boot,
  and Hub time includes delivery and ingest delay. Existing MQTT and Hub
  fields and cadence are unchanged.

### P4 capture and PCM integrity baseline

Enable `CONFIG_SP_MIC_CAPTURE_DIAG`, `CONFIG_DBMAP_EAR_AUDIO_ENGINE`,
`CONFIG_DBMAP_EAR_AE_DIAG` and `CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG` on the
approved measurement image. Keep `CONFIG_DBMAP_EAR_AE_DIAG_EVERY_N` at its
product default of 10 unless the test calls for a different sampling cadence.
The capture summary remains on its existing approximately 5-second cadence.

Illustrative P4/P5 USB output:

```text
sp_mic: capture_diag totals windows=120 collection_us_min=1000120 collection_us_max=1012100 cadence_n=119 cadence_us_min=1019800 cadence_us_max=1034000 produced=120 replacements=0 slot_counters_saturated=0
sp_mic: capture_diag din=0 read_errors=0 timeouts=0 empty=0 partial=2 incomplete=0 capped_windows=0
ae: timing pcm seq=120 consumed=120 checked=120 stale_before_access=0 reuse_during_access=0 queue_n=120 queue_us_last=24000 queue_us_max=88000 processing_n=120 processing_us_last=402000 processing_us_max=490000 epoch=1 capture_mask=0x3 comparable_mask=0x3
ae: diag pcm seq=120 ch=0 captured=16384 retained=16176 cap_discarded=208 cap_discard_total=7680 cap_discard_max=976
ae: diag pcm seq=120 ch=1 captured=16384 retained=16176 cap_discarded=208 cap_discard_total=7680 cap_discard_max=976
```

- Collection extrema cover finalized software collection windows. Cadence
  extrema are intervals between consecutive producer collection ends.
  `produced` counts PCM publications and `replacements` counts overwrites of an
  unread pending job. DIN fault counts accumulate once per DIN per window.
  A read can contribute to more than one fault counter. Where the existing
  per-window read counters capped, `capped_windows` identifies lower bounds.
- AE spans describe one matched job. A channel bit in `capture_mask` means
  its capture view was available. A bit in `comparable_mask` means its
  captured/retained comparison is valid. Interpret the current cap discard
  only when that bit is set. Totals and maxima cover comparable jobs that
  reached the analysis call in the current AE epoch.
- Queue age is software collection end to the analysis start marker.
  Processing age is collection end to the existing post-DSP end marker.
  Their counts can differ when analysis or signature construction fails.
  The existing DSP timer includes the two diagnostic bank checks on this
  build. `consumed` counts projected jobs and `checked` counts completed
  access brackets, including analysis errors.
- `stale_before_access` detects a generation mismatch before DSP. The
  independent `reuse_during_access` counter detects any change between the
  start and end checks. One job can contribute to both. The producer marks
  a bank before the first writable batch even if the fill never publishes.
  A generation wraps modulo 2^32 and zero is valid. Compare within a boot.
  With AE diagnostics compiled in, the checks bracket the AE and legacy
  DSP calls, including error returns. AE reports the two mismatch classes
  separately. Legacy fallback reports their combined count as
  `legacy_generation_mismatches`. A build with AE disabled cannot enable
  its diagnostic flag through Kconfig. Bank ownership applies in both
  builds. The checks can detect a violation without identifying samples.
- A producer may keep logging after AE stops publishing signatures. Inspect
  the last AE sequence and producer totals before interpreting a quiet AE log.
  These diagnostics do not establish DMA continuity, sample-clock accuracy,
  or CPU utilization.

For each operator-confirmed mono and stereo port, save the unfiltered USB
output separately. Record the board, firmware revision, boot/session, live
mic count and flags. Sample 5 minutes of quiet activity, then 15 minutes of
representative network and naturally occurring Observation activity. Extend
the run if the latter workload does not occur. Start logging at boot if the
approved deployment permits it. Use the exact port and repeat with a distinct
log name for stereo:

```bash
PORT=/dev/serial/by-id/REPLACE_WITH_CONFIRMED_DEVICE_ID
LOG=ear-mono-p4.log
stty -F "$PORT" 115200 raw -echo
timeout 20m cat "$PORT" > "$LOG"
rg 'node_id=|capture_diag|ae: (diag|timing)|res ctx=|L0 perf' "$LOG"
```

Accept the run as a baseline when comparable channel spans reconcile, the
producer and consumer counters are attributable to the run, and the intended
resource and timing summaries are present. Report cap loss, transport faults
and missing jobs separately. Require zero generation mismatches across the
recorded checked jobs for the borrow-integrity check to pass. Record observed
timing maxima, heap minima, sampled largest blocks and stack high-water marks
for each mode. These observations apply to the recorded workload.

### P5 PCM ownership acceptance

With the same capture and AE flags, the capture summary also emits:

```text
sp_mic: capture_diag ownership pending_retired=0 no_bank_drops=0 unpublished_fills=0 projection_failed=0 invalid_releases=0 violations=0 legacy_generation_mismatches=0
```

The Platform has two group banks and one pending slot. A take pins a bank
until the consumer releases its token. A null-output take discards pending
without pinning it. Generation checks run before release and are independent
of the ownership token. The producer keeps metering whenever PCM is dropped.

- `replacements` still totals unread pending jobs replaced by the producer.
  `pending_retired` is the subset retired at fill start to obtain a free bank
  while one was leased. The job's take-time overwrite snapshot keeps a later
  retirement from marking a job already in DSP as a new GAP.
- `no_bank_drops` counts windows in which the producer found no writable bank
  at the first eligible batch. `unpublished_fills` counts reserved fills
  returned to the free pool without publication. Neither counter counts
  cap-discarded samples.
- `projection_failed` counts failed PCM job projections, whose lease is
  released internally. `invalid_releases` counts invalid, stale or duplicate
  release tokens. `violations` counts invalid ownership transitions.
  Pending reset leaves outstanding leases and in-progress fills intact.

For a later approved build, capture from startup when possible and repeat the
mono/stereo procedure above. Compare first and last counters within each
boot and AE epoch. Require zero new invalid releases, ownership violations
and generation mismatches. Account for every pending retirement, no-bank
drop, unpublished fill and projection failure separately. Check that the
first applicable successor reports any missing-job gap and the already-taken
window retains its prior quality. Compare per-channel spans, timing, stack
and heap with P4. On a P5 image, cap loss should still be visible. P6 has
separate span criteria. A separate legacy target run,
if approved, can validate lease behavior and the capture ownership counters
without AE generation reporting.

### P6 frame-bounded PCM acceptance

At 16 kHz and 1,000 ms the normal target is 16,000 complete frames per
active channel. Metering and retained PCM consume the same groups. A scratch
suffix survives the software window boundary and is used before that DIN is
read again. The two-bank lease, pending slot, DMA settings and capture task
stack are unchanged. The existing yield remains between attempts.

An attempt stops early after 250 ms without progress or at the nominal window
duration plus 250 ms, checked between synchronous reads and grouping steps.
A read already in progress can extend observed elapsed time. A nonempty short
span follows the same sample-count and transport status rules and carries
actual counts. An empty attempt advances sequence and updates meters, with no
PCM job. P5 no-bank operation also continues to meter without a job.
The AE GAP calculation still uses sequence and take-time overwrite evidence.

ESP-IDF v5.4.4's `i2s_channel_read()` copies byte ranges and can return bytes
with a timeout. This configuration requests integral 32-bit slot groups and
uses whole hardware frames in DMA. Capture uses complete frames from successful
or timed-out reads. An unexpected trailing fragment is counted once and the
remainder of that frame is skipped from the next read(s). The phase recovery assumes that a fragment's
remaining bytes continue in the returned stream. Independent DIN cursors
align by software ordinals after a known discard; peer prefixes with
those missing ordinals are counted as explicit discards. No partial slots
are stitched together. These ordinals are accounting for returned bytes,
without measurement of physical sample time, DMA loss, or cross-DIN phase.

Alongside the P4/P5 summaries, inspect these P6 fields:

- `capture_diag assembly` shows attempted sequence, target, cumulative
  nonempty `short` and `empty` attempts, stop reasons `stalled` and `deadline`,
  complete groups consumed from carry and known skipped software ordinals.
- The DIN summary adds `frame_fragments` (includes mono), `discarded_frames`
  (complete frames discarded after a read fault or peer alignment), and
  `carry_out` (current complete frames remaining in that DIN scratch).
- `ae: timing pcm` adds `span_end` (0 complete, 1 stalled, 2 deadline),
  `carry_in` (leading groups consumed before the first read of the attempt),
  and `software_missing` (ordinal skips associated with consumed groups).
  Transport counters are charged when the read occurs; carried data does
  not charge the same read fault to another window.

For an approved target run, verify the image identity and effective diagnostic
configuration, then collect concurrent USB from startup and through the same
quiet, acoustic, and recovery phases used for P5 on mono and stereo devices.
Join capture/AE logs by node, boot, AE epoch and window sequence. Require
16,000 captured and retained per active channel for each normal projected
job, zero normal cap discard, a coherent active mask, and zero ownership or
generation mismatches. Keep short/empty attempts, explicit discards, pending
retirements and no-bank drops separate. Compare collection/cadence, queue and
DSP wall time, stack and heap margins, acoustic gates and Observation reports.
Changed span boundaries can change decisions. Capture and playback results
alone cannot verify DMA continuity or physical channel timing. Reserve a
separately approved transport/overload test for intentionally short, empty
or no-bank cases. For future physical 3/4-channel verification, use a
common deterministic channel stimulus to validate order, cross-DIN alignment
and faults across startup, stalls and recovery. TinyML and DoA can share the
same leased PCM groups; DoA additionally needs physical timing and calibration.

## Agents / automation notes

- **Read-only** console attach is fine when the task needs live firmware evidence.
- Do **not** flash, erase, reboot, or reconfigure over serial unless the user
  explicitly asked (see `AGENTS.md` operational safety).
- Cursor’s default command sandbox often **hides** `/dev/ttyACM*`. Reading the
  console requires unrestricted / device-capable execution (e.g. “all”
  permissions), then the steps above with an **explicit** `PORT`.
- If the user names a port (`/dev/ttyACM1`, a `by-id` path, etc.), use that and
  do not silently switch to another device.
- When multiple adapters are present and no port was given, **ask** which board
  to open before attaching.

## Related

- Ear app / diag capture: [firmware/esp-ear/README.md](../firmware/esp-ear/README.md)
- Flash & dialout: [device-registration-and-deployment.md](./device-registration-and-deployment.md)
- USB vs Hub topology: [connectivity.md](./connectivity.md)

## Identity and Hub comparison

EAR and OUT emit `node_id=<id> firmware_version=<version> uptime_s=<seconds>`
at boot and roughly every 60 seconds in their normal status loops. Network
backoff can delay the next line. A Web USB Console attachment identifies the
node from complete lines read during that attachment. USB VID/PID is an
adapter label. After close or disconnect, **Last connected** identifies the
previous attachment, and a fresh attachment verifies identity again.

For EAR, **Changes since Hub snapshot** takes one read-only Hub snapshot after
identity is established. It shows changed counters only when the USB firmware
version and estimated boot time agree with the Hub sample. Boot-time agreement
is approximate. Missing diagnostics pause comparison, and reboot or identity
changes invalidate the baseline. A counter decrease is reported as a possible
reset or baseline mismatch. Increases describe activity since the Hub sample.
The raw console, search, and download remain available for both products.

## EAR signature and promotion decisions

See [Firmware Map → EAR capacities](firmware-map/products/dbmap-ear/capacities.md).
`CONFIG_DBMAP_EAR_OBS_DECISION_DIAG` defaults to OFF in Kconfig. The EAR
target enables it in `sdkconfig.defaults` for the three-node acoustic
baseline. Audio Engine and RMS activity must both be enabled. For an OFF
build, unset the flag via menuconfig or apply a defaults override.
Check the generated sdkconfig
for the compiled state. Firmware Builder **List config** shows the builder
target's value, not the running device or a separately configured local build.
There is no runtime switch. The ON image logs a startup banner when the
activity task starts. Console logging must include INFO for the `ear_dec` tag.

Capture the normal 115200 USB console and retain the raw log. Join the
`ear_dec` lines by `seq`, then join any existing `obs_mgr START cand=… obs=…`
line by Candidate ID and publication by Observation UUID. These message
bodies are from a host test with a qualified AS-002 window and no matching
Candidate owner:

```text
AS001 seq=104 gate=distance q=0 rms=-31.00 bg=-48.34 bg_ok=1 min=-44.84 dist=17.04
AS002 seq=104 gate=tonal rms=-31.00 bg=-55.00 bg_ok=1 level=0 streak=2>3 allow=1
policy seq=104 cid=777 cseq=103 sseq=104 as001=no_class as002=owner_mismatch sel=ineligible decision=2 profile=0
policy seq=105 cid=777 cseq=105 sseq=105 as001=no_class as002=qualified sel=selected decision=1 profile=2
```

- AS-001 reports the first failing gate or match, the background before the current
  window updates it, and distance if scored (`nan` means unavailable).
- AS-002 `level` indicates the background comparison ran on this window.
  `allow` is the streak-level result. Policy also requires a usable tonal vote
  and the matching Candidate window.
- CE action values follow `ce_ingest_action_t` (`4` update, `5` create).
  Policy codes follow `ear_promo_decision_t` (`1` promote, `2` irrelevant).
  Policy emits the first poll result per processed AE window, showing the
  selected Candidate or a representative rejection. An ownership or stale-gap
  rejection takes precedence over a generic ineligible Candidate. Later polls
  of the same window and other Candidates' rejections are omitted. Successful
  START and publication messages remain available for final correlation.

The ON build emits up to four compact records per processed AE window
(roughly four records per second at one-second cadence). It uses scalar
diagnostic fields and existing USB logging, with no new capture buffer.
