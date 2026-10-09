#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if command -v opa >/dev/null 2>&1; then
  exec opa test "$repo_root/policy" -v
fi
if command -v docker >/dev/null 2>&1; then
  exec docker run --rm \
    -v "$repo_root/policy:/policies:ro" \
    openpolicyagent/opa:1.21.1 test /policies -v
fi

echo "Install OPA or Docker to run the Rego policy tests." >&2
exit 1
