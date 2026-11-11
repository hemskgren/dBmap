# ADR 0002: Audio Engine and Candidate Engine

- **Status:** Proposed
- **Date:** 2026-11-11
- **Companion:** [Audio Engine and Candidate Engine design](../architecture/audio-and-candidate-engine.md)

## Context

After Sensor Platform Step 1 and multi-mic acquisition (PR #48), Ear firmware still folds continuous acoustic analysis, single-stream activity detection, and MQTT Observation publishing into product modules (`ear_acoustic`, `ear_observation`). That works for today’s mic-0 RMS episodes, but it mixes concepts that must stay separate for multi-source tracking, future classification modules, and Hub reporting.

## Decision (proposed)

Introduce two application-layer components **above** Sensor Platform mic acquisition:

1. **Audio Engine** — transforms Platform audio windows into time-based acoustic features, signatures, and signature history. Does not own I2S/DMA. Does not emit Observations.
2. **Candidate Engine** — maintains zero or more simultaneous **Candidates** (potential sources/events) fed by signature updates. Manages identity, evidence accumulation, and lifecycle. Does not classify product meaning and is not a second reporting system.

Observation publishing, Module Runtime, Workflow Engine, Decision/Fusion, ABC/Drone-ML, and spatial TDOA remain **later** consumers of Candidates (or of promoted results), not owners of acquisition or signature production.

## Consequences

- Platform `mic` stays the sole owner of I2S RX, window capture, health audio telemetry, and PCM hand-off.
- Existing L0/L1/L2 math libraries are reused inside / beside Audio Engine; Observation JSON schemas stay dBmap product schemas.
- Development continues with **two physical microphones** (Mic 0/1 on DIN0). Four-channel Platform support is not a dependency for Audio Engine v1.
- Dual-DIN sample sync / TDOA are **not** assumed.

## Alternatives considered

See companion design §13 (risks and alternatives). Short form: folding Candidates into the existing activity detector would keep a single-stream model; putting Audio Engine into Platform would leak product analysis into shared infrastructure.
