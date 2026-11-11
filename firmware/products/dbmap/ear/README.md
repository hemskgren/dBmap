# dBmap Ear product modules

| Component | Responsibility |
| --- | --- |
| `ear_acoustic` | Acoustic L0/L1/L2 schemas and runtime analysis over Platform PCM windows |
| `ear_observation` | Ear activity detector, acoustic Observation payload, publisher / OTA close |

Uses Platform `mic` and `observation` services. Does not implement Nose or Monitor.
