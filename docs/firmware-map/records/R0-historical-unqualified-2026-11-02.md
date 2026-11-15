# Historical / unqualified — EAR-001 L0 bring-up (2026-11-02)

**Value class:** historical / unqualified — **not** a current `measured` Map record for main @ PR #54.  
**Why retained:** explains audit citations of 129408 B PCM and ~8.46/8.25 MB heap/SPIRAM.  
**Source note:** `docs/diagnostics/ear-l0-firmware-integration-notes.md`.

## Partial identity

| Field | Value |
| --- | --- |
| date | 2026-11-02 (local flash note) |
| node | EAR-001 |
| pcm_cap / pcm_bytes | 16176 / **129408** (mono) |
| free_heap / free_spiram after PCM | 8458520 / 8251648 |
| L0 analyze | us_last≈160–169 ms, overwrite=0 over 89 windows |
| SERIAL_DIAG | y |

## Missing for Map `measured`

- git_sha of that flash
- firmware_version / platform_version
- runtime `cpu_mhz` on the same log table
- microphone_count confirmation on that run
- L1/L2/AE/CE (pre current stack)
- Multi-mic Stage 1 not present

**Do not** use these timings as current verified limits.
