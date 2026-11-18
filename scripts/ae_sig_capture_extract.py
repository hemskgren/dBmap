#!/usr/bin/env python3
"""Extract AE_SIG_CSV rows from an EAR USB console capture.

Usage:
  uv run python scripts/ae_sig_capture_extract.py console.txt -o ear_sigs.csv

Does not modify AS-001 profile artifacts. Output is suitable for comparison
with docs/acoustic-profiles/AS-001/artifacts/signatures.csv (feature columns).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

BEGIN_RE = re.compile(r"AE_SIG_CSV_BEGIN\b.*\bheader=([^\r\n]+)")
ROW_RE = re.compile(r"\bAE_SIG_CSV\s+(.+)$")
END_RE = re.compile(
    r"AE_SIG_CSV_END\b.*\bstored=(\d+).*?\bdrained=(\d+).*?\boverflow=(\d+)"
)


def extract(text: str) -> tuple[str, list[str], dict[str, int]]:
    header = ""
    rows: list[str] = []
    meta = {"stored": 0, "drained": 0, "overflow": 0, "begin": 0, "end": 0}
    for line in text.splitlines():
        m = BEGIN_RE.search(line)
        if m:
            header = m.group(1).strip()
            meta["begin"] += 1
            continue
        m = ROW_RE.search(line)
        if m:
            # Avoid matching BEGIN/END which also contain AE_SIG_CSV_
            body = m.group(1).strip()
            if body.startswith(("BEGIN", "END")):
                continue
            rows.append(body)
            continue
        m = END_RE.search(line)
        if m:
            meta["stored"] = int(m.group(1))
            meta["drained"] = int(m.group(2))
            meta["overflow"] = int(m.group(3))
            meta["end"] += 1
    return header, rows, meta


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("console", type=Path, help="USB console capture text file")
    ap.add_argument("-o", "--output", type=Path, required=True, help="Output CSV path")
    ap.add_argument(
        "--require-complete",
        action="store_true",
        help="Exit non-zero if AE_SIG_CSV_END missing or overflow>0",
    )
    args = ap.parse_args()

    text = args.console.read_text(encoding="utf-8", errors="replace")
    header, rows, meta = extract(text)
    if not header:
        # Fallback to firmware contract header if BEGIN was truncated.
        from_fw = (
            "session_id,window_seq,window_end_mono_us,window_ms,sample_rate_hz,"
            "channel,quality,flags,clipped,"
            "rms_dbfs,peak_dbfs,crest_db,"
            "band0_dbfs,band1_dbfs,band2_dbfs,band3_dbfs,"
            "band0_rel,band1_rel,band2_rel,band3_rel,"
            "hl_db,hl_valid,spectral_valid,centroid_hz,bandwidth_hz,dominant_hz,"
            "flatness,as001_matched,as001_rej_elig,as001_skip_spec,as001_profile_id,"
            "as001_dist"
        )
        header = from_fw
        print("warning: AE_SIG_CSV_BEGIN header not found; using built-in header", file=sys.stderr)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header + "\n" + "\n".join(rows) + ("\n" if rows else ""), encoding="utf-8")
    print(
        f"wrote {args.output} rows={len(rows)} "
        f"begin={meta['begin']} end={meta['end']} "
        f"stored={meta['stored']} drained={meta['drained']} overflow={meta['overflow']}"
    )
    if args.require_complete:
        if meta["end"] < 1:
            print("error: missing AE_SIG_CSV_END", file=sys.stderr)
            return 2
        if meta["overflow"] > 0:
            print("error: capture overflow > 0", file=sys.stderr)
            return 3
        if len(rows) == 0:
            print("error: no AE_SIG_CSV rows", file=sys.stderr)
            return 4
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
