#!/bin/sh
set -eu

port="${DBMAP_HTTPS_PORT:-8443}"
case "$port" in
  '' | *[!0-9]*)
    echo "DBMAP_HTTPS_PORT must be an integer from 1 to 65535." >&2
    exit 1
    ;;
esac
if [ "$port" -lt 1 ] || [ "$port" -gt 65535 ]; then
  echo "DBMAP_HTTPS_PORT must be an integer from 1 to 65535." >&2
  exit 1
fi

base_path="${DBMAP_PUBLIC_BASE_PATH:-}"
case "$base_path" in
  '') ;;
  /*) ;;
  *)
    echo "DBMAP_PUBLIC_BASE_PATH must be empty or start with '/'." >&2
    exit 1
    ;;
esac
case "$base_path" in
  */ | *//* | *[!A-Za-z0-9_/-]* | *..*)
    echo "DBMAP_PUBLIC_BASE_PATH must use safe path segments, with no trailing slash." >&2
    exit 1
    ;;
esac
