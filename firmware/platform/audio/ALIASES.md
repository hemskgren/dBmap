# Compatibility aliases for Platform acoustic DSP

Canonical ownership: `firmware/platform/audio` (`sp_acoustic_l0/l1/l2`).

Temporary EAR-facing aliases remain under:

- `firmware/products/dbmap/ear/ear_acoustic/include/dbmap_acoustic_l0.h`
- `…/dbmap_acoustic_l1.h`
- `…/dbmap_acoustic_l2.h`

These headers only `#include` the Platform headers and map historical `dbmap_l*` /
`DBMAP_L*` names to `sp_acoustic_l*` / `SP_ACOUSTIC_L*`. They do **not** contain a
second implementation.

Why they remain: AE, Observation, and older host tests still include
`dbmap_acoustic_l*.h`. Removing aliases is a follow-up rename sweep after A–D.
