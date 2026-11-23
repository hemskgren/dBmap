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
uid="$(id -u)"
gid="$(id -g)"
canary=''
sdk="${MOSQUITTO_INCLUDE_DIR:-/usr/include}"
# Override only with compatible local images. No image is pulled by this runner.
hub_image="${DBMAP_TEST_HUB_IMAGE:-dbmap-hub-bootstrap:latest}"
broker_image="${DBMAP_TEST_MQTT_IMAGE:-eclipse-mosquitto:latest}"
cleanup() {
  docker rm -f "$broker" >/dev/null 2>&1 || true
  docker network rm "$name" >/dev/null 2>&1 || true
  if [[ -n "$canary" ]]; then
    rm -f -- "$canary"
  fi
  rm -rf -- "$tmp"
}
trap cleanup EXIT
umask 077
mkdir "$tmp/source" "$tmp/broker" "$tmp/lease"
# A simulated checkout secret, never supplied to the source bundler.
canary="$(mktemp "$repo/.ear-test-secret.XXXXXX")"
printf 'synthetic test fixture only\n' > "$canary"
python3 "$repo/tests/integration/ear_mqtt/bundle.py" "$repo" "$tmp/source"

# Resolve libc and broker symbols in the broker process (including Alpine images).
cc -std=c11 -Wall -Wextra -Werror -fPIC -shared -nostdlib -fno-stack-protector \
  -I"$sdk" "$repo/tests/integration/ear_mqtt/lease_acl.c" -o "$tmp/broker/lease_acl.so"

broker_files="$tmp/broker"

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=dbmap-ephemeral-ear-test   -keyout "$broker_files/ca.key" -out "$broker_files/ca.crt" 2>/dev/null
openssl req -new -newkey rsa:2048 -nodes -subj /CN=mqtt.ear-test.invalid   -keyout "$broker_files/broker.key" -out "$broker_files/broker.csr" 2>/dev/null
printf 'subjectAltName=DNS:mqtt.ear-test.invalid\nextendedKeyUsage=serverAuth\n' > "$broker_files/extensions"
openssl x509 -req -days 1 -in "$broker_files/broker.csr" -CA "$broker_files/ca.crt" -CAkey "$broker_files/ca.key"   -CAcreateserial -extfile "$broker_files/extensions" -out "$broker_files/broker.crt" 2>/dev/null
rm -- "$broker_files/ca.key"

python3 - "$broker_files" <<'PY'
import json
from pathlib import Path
import secrets
import sys
root = Path(sys.argv[1])
passwords = {username: secrets.token_hex(24) for username in (
    "sim-ear-001", "test-hub",
)}
(root / "credentials.json").write_text(json.dumps(passwords))
(root / "passwd").write_text("".join(f"{user}:{pw}\n" for user, pw in passwords.items()))
PY

cat > "$broker_files/acl" <<'EOF'
user sim-ear-001
topic write esp-ear/local/sim-ear-001/observation
topic write esp-ear/local/sim-ear-001/keepalive

user test-hub
topic read esp-ear/local/tst-ear-001/observation
topic read esp-ear/local/tst-ear-001/keepalive
topic read esp-ear/local/sim-ear-001/observation
topic read esp-ear/local/sim-ear-001/keepalive
EOF

cat > "$broker_files/mosquitto.conf" <<'EOF'
listener 8883
allow_anonymous false
password_file /fixture/passwd
acl_file /fixture/acl
certfile /fixture/broker.crt
keyfile /fixture/broker.key
persistence false
plugin /fixture/lease_acl.so
EOF

docker network create --internal \
  --opt com.docker.network.bridge.gateway_mode_ipv4=isolated "$name" >/dev/null
docker run --pull never -d --name "$broker" --network "$name" \
  --network-alias mqtt.ear-test.invalid --user "$uid:$gid" \
  --mount "type=bind,src=$broker_files,dst=/fixture" \
  --mount "type=bind,src=$tmp/lease,dst=/lease,readonly" --entrypoint /bin/sh "$broker_image" \
  -ec 'mosquitto_passwd -U /fixture/passwd && exec mosquitto -c /fixture/mosquitto.conf' >/dev/null

if ! docker run --pull never --rm --network "$name" --user "$uid:$gid" \
  --tmpfs "/data:mode=700,uid=$uid,gid=$gid" \
  --mount "type=bind,src=$tmp/source,dst=/repo,readonly" \
  --mount "type=bind,src=$tmp/lease,dst=/lease" \
  --mount "type=bind,src=$broker_files/ca.crt,dst=/test-ca.crt,readonly" \
  --mount "type=bind,src=$broker_files/credentials.json,dst=/test-credentials.json,readonly" \
  --env DBMAP_DATABASE_URL=sqlite:////data/bootstrap.db --env PYTHONDONTWRITEBYTECODE=1 \
  --env PYTHONPATH=/repo/hub/bootstrap:/repo/simulator/ear \
  --env "CHECKOUT_CANARY=${canary##*/}" \
  --workdir /repo "$hub_image" python tests/integration/ear_mqtt/round_trip.py; then
  docker logs "$broker" >&2
  exit 1
fi
