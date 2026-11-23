#!/usr/bin/env bash
# Start only a disposable broker and a Hub listener on a private Docker network.
set -euo pipefail
if [[ "$#" != 1 || "$1" != --live-mqtt ]]; then
  echo 'Usage: tests/integration/ear_mqtt/run.sh --live-mqtt' >&2
  exit 2
fi

repo="$(cd "$(dirname "$0")/../../.." && pwd)"
tmp="$(mktemp -d)"
name="dbmap-ear-test-${tmp##*.}"
broker="${name}-broker"
# Override only with compatible local images. No image is pulled by this runner.
hub_image="${DBMAP_TEST_HUB_IMAGE:-dbmap-hub-bootstrap:latest}"
broker_image="${DBMAP_TEST_MQTT_IMAGE:-eclipse-mosquitto:latest}"
cleanup() {
  docker rm -f "$broker" >/dev/null 2>&1 || true
  docker network rm "$name" >/dev/null 2>&1 || true
  rm -rf -- "$tmp"
}
trap cleanup EXIT
umask 077

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=dbmap-ephemeral-ear-test   -keyout "$tmp/ca.key" -out "$tmp/ca.crt" 2>/dev/null
openssl req -new -newkey rsa:2048 -nodes -subj /CN=mqtt.ear-test.invalid   -keyout "$tmp/broker.key" -out "$tmp/broker.csr" 2>/dev/null
printf 'subjectAltName=DNS:mqtt.ear-test.invalid\nextendedKeyUsage=serverAuth\n' > "$tmp/extensions"
openssl x509 -req -days 1 -in "$tmp/broker.csr" -CA "$tmp/ca.crt" -CAkey "$tmp/ca.key"   -CAcreateserial -extfile "$tmp/extensions" -out "$tmp/broker.crt" 2>/dev/null
rm -- "$tmp/ca.key"

python3 - "$tmp" <<'PY'
import json
from pathlib import Path
import secrets
import sys
root = Path(sys.argv[1])
passwords = {username: secrets.token_hex(24) for username in (
    "tst-ear-001", "sim-ear-001", "test-hub",
)}
(root / "credentials.json").write_text(json.dumps(passwords))
(root / "passwd").write_text("".join(f"{user}:{pw}\n" for user, pw in passwords.items()))
PY

cat > "$tmp/acl" <<'EOF'
user tst-ear-001
topic write esp-ear/local/tst-ear-001/observation
topic write esp-ear/local/tst-ear-001/keepalive

user sim-ear-001
topic write esp-ear/local/sim-ear-001/observation
topic write esp-ear/local/sim-ear-001/keepalive

user test-hub
topic read esp-ear/local/tst-ear-001/observation
topic read esp-ear/local/tst-ear-001/keepalive
topic read esp-ear/local/sim-ear-001/observation
topic read esp-ear/local/sim-ear-001/keepalive
EOF

cat > "$tmp/mosquitto.conf" <<'EOF'
listener 8883
allow_anonymous false
password_file /fixture/passwd
acl_file /fixture/acl
certfile /fixture/broker.crt
keyfile /fixture/broker.key
persistence false
EOF

docker network create --internal \
  --opt com.docker.network.bridge.gateway_mode_ipv4=isolated "$name" >/dev/null
docker run --pull never -d --name "$broker" --network "$name" \
  --network-alias mqtt.ear-test.invalid --user "$(id -u):$(id -g)" \
  --mount "type=bind,src=$tmp,dst=/fixture" --entrypoint /bin/sh "$broker_image" \
  -ec 'mosquitto_passwd -U /fixture/passwd && exec mosquitto -c /fixture/mosquitto.conf' >/dev/null

if ! docker run --pull never --rm --network "$name" --tmpfs /data:mode=700 \
  --mount "type=bind,src=$repo,dst=/repo,readonly" \
  --mount "type=bind,src=$tmp/ca.crt,dst=/test-ca.crt,readonly" \
  --mount "type=bind,src=$tmp/credentials.json,dst=/test-credentials.json,readonly" \
  --env DBMAP_DATABASE_URL=sqlite:////data/bootstrap.db --env PYTHONDONTWRITEBYTECODE=1 \
  --env PYTHONPATH=/repo/hub/bootstrap:/repo/simulator/ear \
  --workdir /repo "$hub_image" python tests/integration/ear_mqtt/round_trip.py; then
  docker logs "$broker" >&2
  exit 1
fi
