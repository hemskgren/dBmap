# Hub map analysis CLI

Laptop / Hub-host tool that runs the existing Cross-EAR `analyze()` pipeline on a
**complete** Hub Observation snapshot and optionally publishes an
**analysis-only** overlay to Analysis scratch for the Hub map dataset selector.

Scratch data is **temporary** (TTL-bound). It is not durable Event history.

## Prerequisites

- Hub reachable over HTTPS with project `.env` (`DBMAP_LAN_IP`, `DBMAP_ADMIN_TOKEN`, CA).
- Analysis Compose profile enabled (`COMPOSE_PROFILES=analysis`) for `publish`.
- Ears (or SIM-EAR) must have already ingested Observation reports into Hub.

## Analyze only (no scratch write)

```bash
uv run python scripts/hub_map_analysis.py analyze
```

Optional client filters (applied **after** the complete snapshot is fetched):

```bash
uv run python scripts/hub_map_analysis.py analyze \
  --node-id EAR-001 --node-id EAR-002 \
  --since 2026-11-07T10:00:00+00:00 \
  --until 2026-11-07T12:00:00+00:00
```

The command refuses to run when Hub pagination reports an incomplete snapshot
(`complete_snapshot=false`). There is no silent partial-data path in this version.

Printed counts include:

- `reports_fetched`
- `reports_after_filter`
- `observations_assembled`
- `observations_after_filter`
- `events`
- `localizations_published`
- `localizations_withheld`

## Publish to the map

Publishes **one analysis-only** scratch object in namespace `map-demo`. Re-running
with the same `--dataset` deletes prior objects for that dataset only, then
uploads the new layer. Unrelated datasets (for example the Helicopter demo) are
not cleared.

```bash
uv run python scripts/hub_map_analysis.py publish \
  --dataset live-site-a \
  --dataset-title "Live site A" \
  --ttl-seconds 3600
```

Then open the Hub map and select dataset **Live site A**.

### Map semantics

| Marker | Coordinates |
| --- | --- |
| Observation | EAR **installation** lat/lon only (skipped if missing) |
| Event / Localization | Published `LocalizationResult` only |
| Ground-truth route | **Never** published by this tool |

Withheld or missing localizations do not get map coordinates. Confidence is
omitted unless a calibrated uncertainty model is present (today: never).

`allow_long_range_sim_ears` defaults to **false**. Use
`--allow-long-range-sim-ears` only for sparse SIM-EAR baselines.

## Safety notes

- Uses Hub and Analysis **HTTPS APIs** only — never Hub SQLite.
- Incomplete snapshots block both `analyze` and `publish`.
- Dataset replacement is scoped by `metadata.dataset`; the tool never issues a
  namespace-wide scratch clear.
- Browser continues to call Hub and Analysis independently.
