# Home Assistant controlled audio (EAR microphone stimulus)

Host-side CLI to drive a Home Assistant **media player** (for example a Nest Mini) with explicit volume and playable URLs. Purpose: give EAR microphone comparison a **repeatable electrical/speaker stimulus**, not calibrated acoustic SPL.

```bash
uv run scripts/homeassistant.py ping
uv run scripts/homeassistant.py status
uv run scripts/homeassistant.py audio list
uv run scripts/homeassistant.py audio status
uv run scripts/homeassistant.py audio status --entity media_player.other
uv run scripts/homeassistant.py audio volume --set 0.25
uv run scripts/homeassistant.py audio play --url https://…/test_signal.wav --volume 0.25
uv run scripts/homeassistant.py audio play --url https://…/test_signal.mp3 \
  --volume 0.50 --verify
uv run scripts/homeassistant.py audio pause
uv run scripts/homeassistant.py audio resume
uv run scripts/homeassistant.py audio stop
uv run scripts/homeassistant.py audio scenario validate \
  tests/audio/microphone-volume-comparison.json
uv run scripts/homeassistant.py audio scenario run --dry-run \
  tests/audio/microphone-volume-comparison.json
uv run scripts/homeassistant.py audio scenario run \
  tests/audio/microphone-volume-comparison.json
```

Connection settings load from the project `.env` via `scripts/lib/hub_cli.load_project_env`. Explicit CLI flags always win over env defaults. The token is never printed.

## Configuration

| Variable / flag | Role |
| --- | --- |
| `HOMEASSISTANT_URL` / `--ha-url` | HA base URL, e.g. `http://homeassistant.local:8123` |
| `HOMEASSISTANT_TOKEN` | Long-lived access token (Profile → Security) |
| `HOMEASSISTANT_MEDIA_PLAYER` / `--entity` | Target entity, e.g. `media_player.nest_mini_workroom` |
| `HOMEASSISTANT_VOLUME` | **Default only** for commands that need a volume when `--volume` is omitted |

Commands never silently pick an arbitrary media player: they use `HOMEASSISTANT_MEDIA_PLAYER` or an explicit `--entity`.

## Connectivity (`ping` / `status`)

| Command | Behaviour |
| --- | --- |
| `ping` | Authenticated `GET /api/`. Reports reachable/unreachable, HTTP status, and elapsed ms. |
| `status` | Read-only Hub-style HA API + configured media-player probe. Distinguishes API availability from media-player availability. Does not play/pause/resume/stop. |

## Discovery (`audio list` / `audio status`)

| Command | Behaviour |
| --- | --- |
| `audio list` | Discover all `media_player.*` entities. Shows entity id, friendly name, state, `supported_features`, and `volume_level` when present. Marks the configured default (`HOMEASSISTANT_MEDIA_PLAYER`). Sorted by entity id. Missing volume on unavailable/off players is shown as `n/a`, not a fatal error. |
| `audio status [--entity …]` | Inspect one player (state, volume, media position/title, attributes). Uses the configured default when `--entity` is omitted; never selects an arbitrary player if no default is configured. |

## Volume and playback controls

| Command | HA service | Notes |
| --- | --- | --- |
| `audio volume` / `--set` / `--up` / `--down` | `volume_set` / state read | Levels are **\[0.0, 1.0\]**, not calibrated SPL |
| `audio play --url` | `play_media` (+ optional `volume_set`) | http(s) only |
| `audio play … --verify` | same + state poll | Optional; see below |
| `audio pause` | `media_pause` | Intended to preserve position |
| `audio resume` | `media_play` | Resume after pause |
| `audio stop` | `media_stop` | May lose position |

**Acceptance ≠ audible confirmation.** A successful HTTP response means Home Assistant accepted the service call.

### Optional `--verify` on `audio play`

Without `--verify`, behavior and output are unchanged.

With `--verify`:

1. Send `play_media` once (no automatic retries of play).
2. If HA rejects the request, fail immediately.
3. Poll the selected player up to **3** times with **1 s** between checks (fixed constants).
4. Require `state == playing` and, when HA exposes it, `media_content_id` equal to the requested URL.
5. Report `volume_level`, `media_position`, and `media_duration` when available.

| Result | Meaning |
| --- | --- |
| `verified` | Player reports `playing` and `media_content_id` matches the requested URL |
| `accepted_unverified` | HA accepted play, but playing/matching ID could not be confirmed in the poll window (or ID not exposed) |
| `verification_failed` | Explicit error/unavailable state, wrong `media_content_id`, or missing entity |

This verifies **Home Assistant player state only**, not acoustic playback.

Local filesystem paths / `file://` are rejected. Nest Mini cannot play arbitrary paths on the machine running this CLI.

## Scenario runner (`schema_version: 1`)

```bash
uv run scripts/homeassistant.py audio scenario list
uv run scripts/homeassistant.py audio scenario validate \
  tests/audio/microphone-volume-comparison.json
uv run scripts/homeassistant.py audio scenario run --dry-run \
  tests/audio/microphone-volume-comparison.json
uv run scripts/homeassistant.py audio scenario run \
  tests/audio/microphone-volume-comparison.json \
  --entity media_player.example
```

### Actions

| Action | Behaviour |
| --- | --- |
| `play` | Resolve `source` via `media_sources` → http(s) URL; optional volume then `play_media` |
| `wait` | Sleep N seconds (does **not** imply playback finished) |
| `stop` | `media_stop` |
| `set_volume` | Absolute `volume_set`, then read back when available (requested vs observed) |
| `pause` / `resume` | `media_pause` / `media_play` |

Steps run strictly in JSON order. Each step reports number, action, UTC start/end, duration, and success/failure. The runner **stops immediately** on the first failed HA operation (no automatic retries).

### Volume safety

Before a live run the runner reads and saves the current `volume_level` when available.

By default it restores that volume after:

- successful completion
- a failed step
- Ctrl-C interruption (also attempts `media_stop`)

The final report states whether restore was attempted and whether readback **verified** the restored level. Restoration is never claimed successful without readback evidence.

If the initial volume cannot be read **and** the scenario changes volume (`set_volume` or `play` with `volume`), the runner refuses to start unless you pass `--no-restore` (explicit acceptance that the Nest Mini may be left at a different level).

```bash
uv run scripts/homeassistant.py audio scenario run scenario.json --no-restore
```

### Dry-run

`--dry-run`:

- fully validates the scenario
- resolves all `media_sources` URLs
- prints the execution sequence, effective media player, volume-related steps, and estimated explicit wait time
- makes **no** Home Assistant service calls and changes no device state

### Failure and interruption

- Explicit HTTP timeouts on HA calls
- Stop on failed operations; do not continue
- Ctrl-C: attempt stop + volume restore; cleanup failures are listed separately and never hide the original error
- Timestamps are UTC internally and in the report

### Final report fields

- Scenario name and schema version
- Target media player
- Total duration
- Successful step count / failed step (if any)
- Play requests accepted by HA
- Volume restoration status
- Cleanup errors (if any)

Service acceptance is never reported as proof of audible playback.

### Example document

```json
{
  "schema_version": 1,
  "name": "microphone-volume-comparison",
  "media_sources": {
    "test_signal": {
      "url": "https://example.com/audio/test_signal.wav",
      "media_content_type": "music"
    }
  },
  "steps": [
    { "action": "play", "source": "test_signal", "volume": 0.25 },
    { "action": "wait", "seconds": 10 },
    { "action": "play", "source": "test_signal", "volume": 0.5 },
    { "action": "wait", "seconds": 10 },
    { "action": "stop" }
  ]
}
```

Rules: only the allowed actions; no shell/Python/templates/arbitrary HA services; `play.source` must map through `media_sources`.

Example file: [`tests/audio/microphone-volume-comparison.json`](../tests/audio/microphone-volume-comparison.json).

## Swedish outdoor warning signals (TEST ONLY)

**Warning:** Playing these signals on a speaker can be mistaken for a real outdoor warning by people nearby. Use only for controlled audio / EAR microphone testing, at low volume, indoors, and with informed people present. These scenarios are **TEST ONLY**. They do not create Home Assistant automations or schedules, and they never run automatically — only an explicit `audio scenario run` starts them. Volume restoration remains the default runner behaviour (do not leave Nest Mini volume permanently changed).

**Hesa Fredrik** is the common name for Sweden’s outdoor warning system, not a fifth separate signal type.

### Official source

- MCF page (source of truth):  
  https://www.mcf.se/sv/rad-till-privatpersoner/skyddsrum-utrymning-och-varningssystem/viktigt-meddelande-till-allmanheten-vma-och-andra-varningar/utomhusvarning--hesa-fredrik/
- Official quarterly test (MCF): first non-holiday Monday in March, June, September, December at **15:00** (VMA), then **Faran över** at **15:05**. This repository does **not** schedule that playback.

### Signal definitions (MCF)

| Signal | Pattern | Approx. duration |
| --- | --- | --- |
| **VMA** (Viktigt meddelande) | 7 s signal / 14 s pause, repeated 6 times | ~2 minutes |
| **Beredskapslarm** | 30 s signal / 15 s pause | 5 minutes |
| **Flyglarm** | 2 s signal / 2 s pause | 1 minute |
| **Faran över** | continuous signal | 30 seconds |

### Official media URLs (remote; local copies later)

| `media_sources` key | URL |
| --- | --- |
| `vma` | https://www.mcf.se/contentassets/ffc078def2284745ae47c8282f83839f/01-viktigt-meddelande.mp3 |
| `beredskapslarm` | https://www.mcf.se/contentassets/ffc078def2284745ae47c8282f83839f/04-beredskapslarm.mp3 |
| `flyglarm` | https://www.mcf.se/contentassets/ffc078def2284745ae47c8282f83839f/03-flyglarm.mp3 |
| `faran_over` | https://www.mcf.se/contentassets/ffc078def2284745ae47c8282f83839f/02-faran-over-.mp3 |

Files are referenced only via scenario `media_sources` (not downloaded into this repo yet).

### Scenario files

| File | Purpose |
| --- | --- |
| [`tests/audio/vma-signal.json`](../tests/audio/vma-signal.json) | Play VMA sample |
| [`tests/audio/beredskapslarm.json`](../tests/audio/beredskapslarm.json) | Play Beredskapslarm sample |
| [`tests/audio/flyglarm.json`](../tests/audio/flyglarm.json) | Play Flyglarm sample |
| [`tests/audio/faran-over.json`](../tests/audio/faran-over.json) | Play Faran över sample |
| [`tests/audio/vma-quarterly-test.json`](../tests/audio/vma-quarterly-test.json) | Quarterly-style sequence |

`vma-quarterly-test.json` timing:

1. `play` VMA (HA accepts the request; audio is ~2 minutes).
2. `wait` **300 seconds measured from that play request** (not 300 seconds after the file ends).
3. `play` Faran över.

That leaves roughly three minutes of silence after the VMA clip ends before Faran över, matching the official ~five-minute gap from the initial play. Do **not** add another 300-second wait after VMA finishes.

```bash
uv run scripts/homeassistant.py audio scenario validate \
  tests/audio/vma-quarterly-test.json
uv run scripts/homeassistant.py audio scenario run --dry-run \
  tests/audio/vma-quarterly-test.json
# Live run only when safe and intentional:
# uv run scripts/homeassistant.py audio scenario run tests/audio/vma-signal.json
```

## Reproducing the same test at either installation

1. Same media URL bytes/host path from the player's point of view.
2. Same media-player device class / room notes.
3. Same `volume_level` values via CLI/scenario (not “last UI setting”).
4. Same wait durations.
5. Same EAR placement relative to the speaker.
6. Record HA URL, entity, volumes, and scenario name in EAR test job metadata.

## Safety / non-goals

- Not a general Home Assistant remote or acoustic calibrator.
- Token values must never appear in CLI output or error text.
- Nest Mini volume percentage is **not** calibrated SPL.
