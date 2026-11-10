/**
 * Observation lifecycle summary helpers for Hub Web.
 * Keep in sync with web/observation_lifecycle.py and CLI hub_event_track summary.
 * Unique = START/COMPLETE observation_ids; Completed = START+END or COMPLETE;
 * durations from END/COMPLETE episode_duration_ms only.
 */
(function (global) {
  function observationFields(observation) {
    const identity = observation && observation.identity;
    const context = observation && observation.context;
    const acoustic = observation && observation.acoustic;
    const spatial = observation && observation.spatial;
    if (
      identity && typeof identity === 'object'
      && context && typeof context === 'object'
    ) {
      let classification = null;
      let bearing = null;
      let signal = null;
      if (acoustic && typeof acoustic === 'object') {
        classification = acoustic.classification;
        signal = acoustic.signal_level_dbfs;
      }
      if (spatial && typeof spatial === 'object') {
        bearing = spatial.bearing;
      }
      return {
        observation_id: identity.observation_id,
        node_id: identity.node_id,
        sequence_number: identity.sequence_number,
        event_time_utc: context.event_time_utc,
        duration_ms: context.interval_duration_ms,
        episode_duration_ms: context.episode_duration_ms,
        emission_kind: context.emission_kind,
        classification,
        bearing,
        signal_level_dbfs: signal,
      };
    }
    return observation && typeof observation === 'object' ? observation : {};
  }

  function observationTimeMs(observation) {
    const fields = observationFields(observation);
    const value = fields.event_time_utc;
    if (typeof value !== 'string' || !value) return null;
    const ms = Date.parse(value);
    return Number.isFinite(ms) ? ms : null;
  }

  function observationReportKey(observation) {
    const fields = observationFields(observation);
    const nodeId = fields.node_id;
    const sequence = fields.sequence_number;
    if (typeof nodeId === 'string' && Number.isInteger(sequence)) {
      return `${nodeId}:${sequence}`;
    }
    return typeof fields.observation_id === 'string' ? fields.observation_id : null;
  }

  function emissionKindToken(value) {
    if (typeof value !== 'string') return 'unknown';
    const token = value.trim().toLowerCase();
    if (token === 'start' || token === 'periodic' || token === 'end' || token === 'complete') {
      return token;
    }
    return 'unknown';
  }

  function finiteNonnegativeMs(value) {
    if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) return null;
    return value;
  }

  function validSequenceNumber(value) {
    return Number.isInteger(value) ? value : null;
  }

  function formatObservationDurationMs(durationMs) {
    if (durationMs < 1000) return `${Math.round(durationMs)} ms`;
    const seconds = durationMs / 1000;
    if (seconds < 60) return `${Math.round(durationMs)} ms (${seconds.toFixed(1)}s)`;
    const minutes = seconds / 60;
    return `${Math.round(durationMs)} ms (${minutes.toFixed(1)} min)`;
  }

  function countReportsByNode(reports) {
    const counts = new Map();
    for (const item of reports || []) {
      const nodeId = observationFields(item).node_id;
      if (typeof nodeId === 'string' && nodeId) {
        counts.set(nodeId, (counts.get(nodeId) || 0) + 1);
      }
    }
    return Object.fromEntries(counts.entries());
  }

  function dedupeObservationReports(reports) {
    const deduped = [];
    const seen = new Set();
    let dropped = 0;
    for (const row of reports || []) {
      const key = observationReportKey(row);
      if (key == null) {
        deduped.push(row);
        continue;
      }
      if (seen.has(key)) {
        dropped += 1;
        continue;
      }
      seen.add(key);
      deduped.push(row);
    }
    return { reports: deduped, duplicateDeliveryDropped: dropped };
  }

  function finalizeObservationSnapshot(options) {
    const reports = options.reports || [];
    const totalCount = Number(options.totalCount) || 0;
    const totalPages = Number(options.totalPages) || 0;
    const snapshotId = options.snapshotId == null ? null : options.snapshotId;
    const order = options.order || 'oldest';
    const { reports: deduped, duplicateDeliveryDropped } = dedupeObservationReports(reports);
    const fetchedCount = deduped.length;
    const complete = fetchedCount === totalCount && (totalPages <= 1 || snapshotId != null);
    const meta = {
      fetched_report_count: fetchedCount,
      total_count: totalCount,
      total_pages: totalPages,
      complete_snapshot: complete,
      duplicate_delivery_dropped: duplicateDeliveryDropped,
      order,
      snapshot_id: snapshotId,
      snapshot_freeze: 'Observation.id <= snapshot_id (Hub); later ingest ids excluded',
    };
    if (!complete) {
      meta.completeness_warning =
        `fetched ${fetchedCount} reports but Hub total_count=${totalCount} `
        + `for snapshot_id=${JSON.stringify(snapshotId)}`;
    }
    return { reports: deduped, meta };
  }

  function emptyDeviceStats(nodeId) {
    return {
      node_id: nodeId,
      unique_observations: 0,
      completed_observations: 0,
      open_observations: 0,
      shortest_completed_ms: null,
      longest_completed_ms: null,
      total_completed_ms: 0,
      completed_with_duration: 0,
      quality_notes: [],
    };
  }

  function pickTerminalEpisodeMs(candidates) {
    if (!candidates.length) return null;
    const ranked = [...candidates].sort((a, b) => {
      const aSeqMissing = a.sequence == null ? 1 : 0;
      const bSeqMissing = b.sequence == null ? 1 : 0;
      if (aSeqMissing !== bSeqMissing) return aSeqMissing - bSeqMissing;
      const aSeq = a.sequence == null ? -1 : a.sequence;
      const bSeq = b.sequence == null ? -1 : b.sequence;
      if (aSeq !== bSeq) return aSeq - bSeq;
      const aTimeMissing = a.eventTimeMs == null ? 1 : 0;
      const bTimeMissing = b.eventTimeMs == null ? 1 : 0;
      if (aTimeMissing !== bTimeMissing) return aTimeMissing - bTimeMissing;
      const aTime = a.eventTimeMs == null ? 0 : a.eventTimeMs;
      const bTime = b.eventTimeMs == null ? 0 : b.eventTimeMs;
      return aTime - bTime;
    });
    return ranked[ranked.length - 1].episode_duration_ms;
  }

  function summarizeObservationLifecycles(observations, options) {
    const fetchedAt = options && options.fetchedAt instanceof Date
      ? options.fetchedAt
      : (options && options.fetchedAt) || null;
    const groups = new Map();
    const groupOrder = [];
    const qualityNotes = [];
    let skippedMissingIdentity = 0;
    let oldestReportEventTimeMs = null;

    for (const item of observations || []) {
      const fields = observationFields(item);
      const eventTimeMs = observationTimeMs(item);
      if (eventTimeMs != null && (oldestReportEventTimeMs == null || eventTimeMs < oldestReportEventTimeMs)) {
        oldestReportEventTimeMs = eventTimeMs;
      }
      const nodeId = fields.node_id;
      const observationId = fields.observation_id;
      if (typeof nodeId !== 'string' || !nodeId) {
        skippedMissingIdentity += 1;
        continue;
      }
      if (typeof observationId !== 'string' || !observationId) {
        skippedMissingIdentity += 1;
        continue;
      }
      const key = `${nodeId}\0${observationId}`;
      let group = groups.get(key);
      if (!group) {
        group = {
          node_id: nodeId,
          observation_id: observationId,
          counts: { start: 0, periodic: 0, end: 0, complete: 0, unknown: 0 },
          terminal_candidates: [],
        };
        groups.set(key, group);
        groupOrder.push(key);
      }
      const kind = emissionKindToken(fields.emission_kind);
      group.counts[kind] += 1;
      if (kind === 'end' || kind === 'complete') {
        group.terminal_candidates.push({
          kind,
          sequence: validSequenceNumber(fields.sequence_number),
          episode_duration_ms: finiteNonnegativeMs(fields.episode_duration_ms),
          eventTimeMs,
        });
      }
    }

    if (skippedMissingIdentity) {
      qualityNotes.push(
        `skipped ${skippedMissingIdentity} report(s) missing node_id or observation_id`
      );
    }

    const perDevice = new Map();
    const combined = emptyDeviceStats('ALL');
    let noStartCount = 0;
    let duplicateStartCount = 0;
    let duplicateEndCount = 0;
    let missingEpisodeMsCount = 0;

    function ensureDevice(nodeId) {
      let stats = perDevice.get(nodeId);
      if (!stats) {
        stats = emptyDeviceStats(nodeId);
        perDevice.set(nodeId, stats);
      }
      return stats;
    }

    for (const key of groupOrder) {
      const group = groups.get(key);
      const counts = group.counts;
      const device = ensureDevice(group.node_id);
      const hasStart = counts.start > 0 || counts.complete > 0;
      const hasEnd = counts.end > 0 || counts.complete > 0;
      if (counts.start > 1) duplicateStartCount += 1;
      if (
        counts.end > 1
        || (counts.end > 0 && counts.complete > 0)
        || counts.complete > 1
      ) {
        duplicateEndCount += 1;
      }
      if (!hasStart) {
        noStartCount += 1;
        continue;
      }

      device.unique_observations += 1;
      combined.unique_observations += 1;

      if (hasStart && hasEnd) {
        device.completed_observations += 1;
        combined.completed_observations += 1;
        const durationMs = pickTerminalEpisodeMs(group.terminal_candidates);
        if (durationMs == null) {
          missingEpisodeMsCount += 1;
          continue;
        }
        device.completed_with_duration += 1;
        combined.completed_with_duration += 1;
        device.total_completed_ms += durationMs;
        combined.total_completed_ms += durationMs;
        if (device.shortest_completed_ms == null || durationMs < device.shortest_completed_ms) {
          device.shortest_completed_ms = durationMs;
        }
        if (device.longest_completed_ms == null || durationMs > device.longest_completed_ms) {
          device.longest_completed_ms = durationMs;
        }
        if (combined.shortest_completed_ms == null || durationMs < combined.shortest_completed_ms) {
          combined.shortest_completed_ms = durationMs;
        }
        if (combined.longest_completed_ms == null || durationMs > combined.longest_completed_ms) {
          combined.longest_completed_ms = durationMs;
        }
      } else {
        device.open_observations += 1;
        combined.open_observations += 1;
      }
    }

    if (noStartCount) {
      qualityNotes.push(
        `${noStartCount} observation_id group(s) had PERIODIC/END/unknown `
        + 'reports without START/COMPLETE (excluded from unique count)'
      );
    }
    if (duplicateStartCount) {
      qualityNotes.push(
        `${duplicateStartCount} observation_id group(s) had duplicate START `
        + 'reports (counted once)'
      );
    }
    if (duplicateEndCount) {
      qualityNotes.push(
        `${duplicateEndCount} observation_id group(s) had duplicate `
        + 'END/COMPLETE reports (duration taken from latest sequence)'
      );
    }
    if (missingEpisodeMsCount) {
      qualityNotes.push(
        `${missingEpisodeMsCount} completed observation(s) lacked usable `
        + 'END/COMPLETE episode_duration_ms (excluded from duration stats)'
      );
    }

    const devices = [...perDevice.keys()].sort().map((nodeId) => perDevice.get(nodeId));
    return {
      devices,
      combined,
      report_count: (observations || []).length,
      oldest_report_event_time_ms: oldestReportEventTimeMs,
      fetched_at: fetchedAt,
      quality_notes: qualityNotes,
    };
  }

  function durationCell(stats, which) {
    let value = null;
    if (which === 'shortest') value = stats.shortest_completed_ms;
    else if (which === 'longest') value = stats.longest_completed_ms;
    else value = stats.completed_with_duration ? stats.total_completed_ms : null;
    if (value == null) return 'n/a';
    return formatObservationDurationMs(value);
  }

  global.observationFields = observationFields;
  global.observationReportKeyFromLifecycle = observationReportKey;
  global.formatObservationDurationMs = formatObservationDurationMs;
  global.countReportsByNode = countReportsByNode;
  global.finalizeObservationSnapshot = finalizeObservationSnapshot;
  global.summarizeObservationLifecycles = summarizeObservationLifecycles;
  global.observationLifecycleDurationCell = durationCell;
})(typeof window !== 'undefined' ? window : globalThis);
