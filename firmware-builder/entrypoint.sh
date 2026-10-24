#!/usr/bin/env bash
set -e

# Preserve the image behavior for repositories mounted from another user.
if [ -n "${IDF_GIT_SAFE_DIR+x}" ]; then
  echo "Adding following directories into git's safe.directory"
  echo "$IDF_GIT_SAFE_DIR" | tr ':' '\n' | while read -r dir; do
    git config --global --add safe.directory "$dir"
    echo "  $dir"
  done
fi

# export.sh prints the toolchain activation essay on stderr. That is not
# request traffic, and it crowds out the container log tail.
if ! . "${IDF_PATH}/export.sh" >/tmp/idf-activate.log 2>&1; then
  echo "ESP-IDF environment activation failed" >&2
  cat /tmp/idf-activate.log >&2
  exit 1
fi
rm -f /tmp/idf-activate.log
exec "$@"
