# Monitoring history recovery

This procedure is for a separately approved maintenance window after the monitoring fix has been deployed and verified. The release needs no history migration. Decide separately whether to reset Device Monitor history and whether to reset Hub Monitor history.

## November 21 incident

The approved read-only snapshot for Hub `local-01` ended at `2026-11-22T09:06:54.547296Z`.

| Table | Rows | Findings |
| --- | ---: | --- |
| `hub_monitor_samples` | 33,699 | 32,481 legacy rows and 1,218 sparse rows, including 609 explicit gap markers |
| `hub_monitor_events` | 848 | 617 resume events, including 609 since November 21. The other 231 events record component, health, availability, or service-restart changes. |
| `device_monitor_samples` | 7,000 | 500 per device across 14 devices, including 3,498 unobserved markers. The oldest retained sample was from 00:15 UTC on November 22. |
| `device_monitor_events` | 7,000 | 500 per device, comprising 6,998 monitoring resumes and two heartbeat recoveries. The oldest retained event was from 00:17 UTC on November 22. |

The Hub's retained samples and events started on October 27. Its configured retention was 30 days, and Device Monitor was configured for 90 days plus its two per-device caps. All 609 incident-period Hub gap markers had matching inferred resume events and unchanged adjacent probe states. Of those, 594 fell in the repeating 120–140 second pattern and 15 lasted longer. The database lacks the successful intermediate probe timestamps needed to distinguish each false gap from a real interruption followed by recovery to an identical state.

Using the existing Hub availability calculator at the snapshot cutoff produced the following in-memory comparison. The two removal scenarios are counterfactuals, not corrections.

| Scenario | 24h eligible / gap seconds | 7d eligible / gap seconds | 30d eligible / gap seconds | 30d availability |
| --- | ---: | ---: | ---: | ---: |
| Stored history | 8,464 / 77,935 | 526,717 / 78,082 | 2,025,599 / 200,593 | 99.98% |
| Remove 594 short markers | 84,184 / 2,215 | 602,438 / 2,361 | 2,101,319 / 124,873 | 99.99% |
| Remove all 609 explicit markers | 86,330 / 69 | 604,583 / 216 | 2,103,464 / 122,727 | 99.99% |

Retain Hub history and let the incident age out if clean rolling periods are sufficient. A selective correction needs independent continuity evidence for each interval it would change, plus a verified list of marker and matching event IDs. A timestamp range, unchanged state, or the duration pattern alone cannot supply that evidence. If an immediate clean Hub baseline is required, request separate approval to reset only `hub_monitor_samples` and `hub_monitor_events` for `local-01`. A clean reset discards legitimate Hub outage, component, and restart history as well. The first percentage requires 600 eligible seconds, and 7d and 30d score readiness require half-window coverage.

## Approved Device Monitor reset

The reset scope is all rows with the approved `hub_id` in `device_monitor_samples` and `device_monitor_events`, including any retained rows for devices that are no longer active. Device diagnostics, reported and desired state, device configuration and identity, observations, and Hub Monitor history are outside the reset.

1. Confirm the installation and Hub ID with `GET /api/v1/hub/status`. Schedule downtime and stop the `hub-bootstrap` writer with `docker compose stop hub-bootstrap`. Confirm the service is stopped and no other writer has the volume open. The SQLite database is `/data/bootstrap.db` in the `bootstrap-data` volume. For backup and reset, mount this same volume using a one-off `docker compose run --rm --no-deps` Hub container with its entrypoint overridden to the approved SQLite client. Keep the Hub stopped through validation.
2. Create a consistent backup in a protected, host-mounted backup directory using SQLite's backup API. For example, in the maintenance container with that directory mounted at `/monitor-backup`, run the following Python. Use a new path and keep the backup access restricted because it contains the whole Hub database.

   ```python
   from contextlib import closing
   from pathlib import Path
   import sqlite3

   destination = Path("/monitor-backup/bootstrap-before-device-monitor-reset.db")
   with destination.open("xb"):
       pass
   with (
       closing(sqlite3.connect("file:/data/bootstrap.db?mode=ro", uri=True)) as source,
       closing(sqlite3.connect(destination)) as backup,
   ):
       source.backup(backup)
       assert backup.execute("PRAGMA integrity_check").fetchall() == [("ok",)]
   ```

3. Bind `:hub_id` to the explicitly approved Hub ID. Run the following previews and verify the counts against the maintenance approval. Refresh the counts in the write transaction before deletion. Review any triggers on the target tables before proceeding. Historical counts in this document are evidence from November 22 and do not substitute for these previews.

   ```sql
   SELECT node_id, COUNT(*) AS rows_to_delete, MIN(observed_at), MAX(observed_at)
   FROM device_monitor_samples WHERE hub_id = :hub_id GROUP BY node_id ORDER BY node_id;

   SELECT node_id, COUNT(*) AS rows_to_delete, MIN(occurred_at), MAX(occurred_at)
   FROM device_monitor_events WHERE hub_id = :hub_id GROUP BY node_id ORDER BY node_id;
   ```

4. Enable foreign-key checks before starting the transaction. Use a client that leaves the transaction open for review. Capture each `changes()` result and compare it with the corresponding preview total. Run `ROLLBACK` on a mismatch or any error. Issue `COMMIT` only when both counts match, both remaining counts are zero, and `foreign_key_check` reports no violations.

   ```sql
   PRAGMA foreign_keys = ON;
   BEGIN IMMEDIATE;

   SELECT COUNT(*) FROM device_monitor_events WHERE hub_id = :hub_id;
   SELECT COUNT(*) FROM device_monitor_samples WHERE hub_id = :hub_id;

   DELETE FROM device_monitor_events WHERE hub_id = :hub_id;
   SELECT changes() AS events_deleted;
   DELETE FROM device_monitor_samples WHERE hub_id = :hub_id;
   SELECT changes() AS samples_deleted;

   SELECT COUNT(*) AS remaining_events FROM device_monitor_events WHERE hub_id = :hub_id;
   SELECT COUNT(*) AS remaining_samples FROM device_monitor_samples WHERE hub_id = :hub_id;
   PRAGMA foreign_key_check;
   ```

5. If rollback is needed after commit and before the writer has resumed, attach the verified backup read-only as `before_reset`. Check that the target schemas match and current scoped counts are still zero. Restore only the deleted scope in one transaction, compare counts with the backup, run `foreign_key_check`, and commit only on a match. If the writer already resumed, stop and review the newer monitoring rows before choosing a recovery operation.

   ```sql
   PRAGMA foreign_keys = ON;
   ATTACH DATABASE 'file:/monitor-backup/bootstrap-before-device-monitor-reset.db?mode=ro'
       AS before_reset;
   BEGIN IMMEDIATE;
   INSERT INTO main.device_monitor_samples
       SELECT * FROM before_reset.device_monitor_samples WHERE hub_id = :hub_id;
   INSERT INTO main.device_monitor_events
       SELECT * FROM before_reset.device_monitor_events WHERE hub_id = :hub_id;
   ```

6. Resume the corrected Hub. On eligible devices, confirm one initial connectivity sample and `monitoring_started` event. Compare event IDs across stable cycles even if counts are capped. Verify that eligible time accumulates, 24h availability becomes populated at 600 eligible seconds, and Device Monitor diagnostics remain available. Infrastructure history is now owned by the optional Operations service.

## Deployment verification before any reset

- Record the rollout time, Hub identity, device sample counts, and latest device monitoring event IDs. Use `GET /api/v1/hub/status` for the Hub overview and node detail and monitoring-event endpoints for Device Monitor data. When Operations is enabled, use its status and events APIs for infrastructure history; the retired Hub monitoring-events endpoint is no longer available.
- Once separately approved, deploy the reviewed Hub build. Use the existing Hub Compose volume and configuration. Verify the configured 30-second Device Monitor cadence, 120-second continuity threshold, and 90-second online window. When Operations is enabled, verify its Core monitor interval and gap threshold separately; Hub infrastructure probes no longer run.
- Observe at least 12 minutes with stable device reports. After startup, expect no recurring `monitoring_resumed` events during continuous checks, stable sample counts, and rising eligible time. A first inferred gap is possible when the sparse startup fallback reaches back to an old persisted sample.
- Confirm Device Monitor shows a percentage once 600 eligible seconds are retained and diagnostics still report normally. When Operations is enabled, confirm infrastructure health and availability are present on demand. Review post-rollout event timestamps separately from retained incident data.

## One-time Core Hub history reset

Use this procedure only for a separately approved clean Hub services baseline. First verify the deployed Core checkpoint across a controlled restart. With the Hub writer stopped, read the current checkpoint and newest Core sample. After restart, confirm that the observation gap starts at the checkpoint and that the checkpoint advances with the first committed check. A gap shorter than the normal event threshold may have a sample marker without a resume event.

For the reset, stop only the `hub-bootstrap` writer and use the protected, integrity-checked SQLite Backup API procedure above with a fresh filename. Preview the following counts for the approved Hub ID, including the checkpoint. Verify them again in the write transaction.

```sql
PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;
SELECT COUNT(*) FROM hub_monitor_samples WHERE hub_id = :hub_id;
SELECT COUNT(*) FROM hub_monitor_events WHERE hub_id = :hub_id;
SELECT COUNT(*) FROM hub_monitor_checkpoints WHERE hub_id = :hub_id;
SELECT last_check_at FROM hub_monitor_checkpoints WHERE hub_id = :hub_id;
```

After reviewing the preview, delete only those rows in the same transaction. Clearing the checkpoint with the history prevents an observation from the previous baseline from carrying into the new one.

```sql
DELETE FROM hub_monitor_events WHERE hub_id = :hub_id;
SELECT changes() AS events_deleted;
DELETE FROM hub_monitor_samples WHERE hub_id = :hub_id;
SELECT changes() AS samples_deleted;
DELETE FROM hub_monitor_checkpoints WHERE hub_id = :hub_id;
SELECT changes() AS checkpoints_deleted;
SELECT COUNT(*) FROM hub_monitor_events WHERE hub_id = :hub_id;
SELECT COUNT(*) FROM hub_monitor_samples WHERE hub_id = :hub_id;
SELECT COUNT(*) FROM hub_monitor_checkpoints WHERE hub_id = :hub_id;
PRAGMA foreign_key_check;
```

Commit only when deletion counts match the preview, scoped counts are zero, and foreign-key checks pass. Roll back on any discrepancy. The `public_monitor_*` tables, Device Monitor history, diagnostics, and unrelated tables stay outside the transaction.

Resume the same Hub build. Verify through the API that initial Core history begins at the first new check, the checkpoint advances, and stable states write no extra samples or events. Observe Core, API, MQTT, Web, public endpoints and Device Monitor for at least 15 minutes. Confirm that the fresh 24h Core service windows report percentages once they have 600 eligible seconds. Verify the earlier public and device event IDs are still present. If verification passes and deletion was approved, delete only the new temporary backup after confirming its path and checksum. Retain it if verification fails.
