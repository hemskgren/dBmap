# dBmap product modules (firmware)

Product-specific firmware for the dBmap application on top of `firmware/platform`.

## Layout

| Path | Role |
| --- | --- |
| `ear/` | Ear node: acoustic analysis + Ear observation / activity |
| `nose/` | Future Nose observation modules (not implemented) |
| `monitor/` | Future Monitor observation modules (not implemented) |

Platform owns microphone hardware, generic audio measurements, Observation Foundation, and generic reported values. Ear-specific interpretation and observation policy live under `ear/`.
