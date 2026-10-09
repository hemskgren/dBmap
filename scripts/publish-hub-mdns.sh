#!/usr/bin/env sh
set -u

HUB_IP="${1:-${DBMAP_LAN_IP:-}}"
if [ -z "$HUB_IP" ]; then
  echo "Warning: hub.local was not published: pass the hub LAN IPv4 address or set DBMAP_LAN_IP." >&2
  exit 0
fi
case "$HUB_IP" in
  *[!0-9.]* | '' | *.*.*.*.*)
    echo "Warning: hub.local was not published: '$HUB_IP' is not a valid IPv4 address." >&2
    exit 0
    ;;
esac
if ! printf '%s\n' "$HUB_IP" | awk -F. 'NF == 4 { for (i = 1; i <= 4; i++) if ($i !~ /^[0-9]+$/ || $i > 255) exit 1; exit 0 } { exit 1 }'; then
  echo "Warning: hub.local was not published: '$HUB_IP' is not a valid IPv4 address." >&2
  exit 0
fi
if ! command -v avahi-publish >/dev/null 2>&1; then
  echo "Warning: hub.local was not published: avahi-publish is unavailable on this host." >&2
  exit 0
fi

echo "Publishing hub.local as $HUB_IP; stop this process to withdraw the record."
if ! avahi-publish -a -R hub.local "$HUB_IP"; then
  echo "Warning: Avahi could not publish hub.local; the Hub remains available by IP." >&2
  exit 0
fi
