# ADR 0002: Audio Engine and Candidate Engine

- **Status:** Proposed
- **Date:** 2026-11-11
- **Companion:** [Audio Engine and Candidate Engine design](../architecture/audio-and-candidate-engine.md)

## Context

After Sensor Platform Step 1 and multi-mic acquisition (PR #48), Ear firmware still folds continuous acoustic analysis, single-stream activity detection, and MQTT Observation publishing into product modules (`ear_acoustic`, `ear_observation`). That works for today’s mic-0 RMS episodes, but it mixes concepts that must stay separate for multi-source tracking, future classification modules, and Hub reporting.

Sensor Platform must be designed and progressively developed as a **reusable multi-engine foundation**: concurrent peer Engines consume different Platform contracts (e.g. Audio Engine Light on audio; Monitor Engine on sensor/Omnibus) via shared generic contracts, transport direction, lifecycle, and services — without product-specific special paths wherever reasonably possible. Platform owns acquisition/contracts/shared services; Engines own domain processing. The Audio/Candidate design must not invent an audio-only transport or runtime that blocks peer Engines.

## Decision (proposed)

1. Introduce two application-layer components **above** Sensor Platform mic acquisition:
   - **Audio Engine** — consumes the Platform **audio contract**; produces features, signatures, and signature history. Does not own I2S/DMA. Does not emit Observations.
   - **Candidate Engine** — consumes signature updates (engine-to-engine); manages multi-candidate identity and lifecycle over **acoustic signatures**. Not a second reporting system; not a classifier; not generalized beyond audio in this ADR unless a separate proposal justifies it.

2. Treat Platform as a **progressive multi-contract foundation** (audio now; sensor/Omnibus later). Distinguish three horizons in the companion (§0): **Platform today**, **AE/CE v1 uses**, **shared Platform later**. Deferred Platform work (not AE/CE v1): versioned contract catalog, multi-subscriber on-device notify/pull, Engine/Module lifecycle registry, resource policy, formal contract timestamps.

3. Observation publishing, Module Runtime, Workflow Engine, Decision/Fusion, ABC/Drone-ML, spatial TDOA, Monitor Engine, and full Engine Runtime remain **later** consumers or peers — not owners of acquisition. AE/CE v1 must keep interfaces migratable onto that shared foundation without implementing it now.

## Consequences

- Platform `mic` stays the sole owner of I2S RX, window capture, health audio telemetry, and PCM hand-off (first concrete audio contract producer).
- AE/CE stay outside Platform acquisition code; L0/L1/L2 math may be reused by Audio Engine.
- AE→CE is engine-local and multi-listener-capable; it must remain migratable onto a future shared Platform data plane without requiring CE to speak I2S.
- Today’s single-taker PCM job is an allowed AE v1 intake, not the long-term multi-subscriber architectural foundation (companion §0.9).
- Development continues with **two physical microphones** (Mic 0/1 on DIN0). Four-channel Platform support is not a dependency for Audio Engine v1.
- Dual-DIN sample sync / TDOA are **not** assumed.
- A future Monitor Engine is a **peer** Engine on other contracts, not a child of Audio Engine.

## Alternatives considered

See companion design §0 and §12. Short form: folding Candidates into the activity detector keeps a single-stream model; putting Audio Engine into Platform leaks product analysis and blocks peer Engines; building a full Engine Runtime before AE over-scopes this proposal; treating single-consumer PCM as the Platform transport foundation would constrain multi-engine reuse.
