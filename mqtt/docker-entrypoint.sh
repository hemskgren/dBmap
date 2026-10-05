#!/bin/sh
set -eu

CERT_DIR=/mosquitto/certs
RUNTIME_DIR=/mosquitto/data/runtime
DYNSEC_CONFIG=/mosquitto/data/dynamic-security.json
DYNSEC_HARDENED=/mosquitto/data/dynamic-security-hardened
INIT_CONFIG=/mosquitto/data/mosquitto-init.conf
CTRL_OPTIONS=/mosquitto/data/mosquitto-ctrl.options
HEALTH_OPTIONS=/mosquitto/data/mosquitto-health.options

mkdir -p /mosquitto/config /mosquitto/data "$CERT_DIR" "$RUNTIME_DIR"

if [ ! -f "$CERT_DIR/ca.crt" ] || [ ! -f "$CERT_DIR/server.crt" ] || [ ! -f "$CERT_DIR/server.key" ]; then
  echo "MQTT TLS certs missing in $CERT_DIR. Run scripts/gen-mqtt-certs.sh on the host." >&2
  exit 1
fi
if [ -z "${MOSQUITTO_DYNSEC_PASSWORD:-}" ] || [ "${#MOSQUITTO_DYNSEC_PASSWORD}" -lt 32 ]; then
  echo "Set MOSQUITTO_DYNSEC_PASSWORD to a generated secret of at least 32 characters." >&2
  exit 1
fi

rm -f /mosquitto/data/dynamic-security-password-init

cp "$CERT_DIR/ca.crt" "$RUNTIME_DIR/ca.crt"
cp "$CERT_DIR/server.crt" "$RUNTIME_DIR/server.crt"
cp "$CERT_DIR/server.key" "$RUNTIME_DIR/server.key"
chown -R mosquitto:mosquitto /mosquitto/data
chmod 0600 "$RUNTIME_DIR/server.key"
chmod 0644 "$RUNTIME_DIR/ca.crt" "$RUNTIME_DIR/server.crt"

if [ -f "$DYNSEC_HARDENED" ] && [ ! -s "$DYNSEC_CONFIG" ]; then
  echo "Dynamic-security configuration is missing after broker hardening." >&2
  exit 1
fi

if [ ! -f "$DYNSEC_HARDENED" ]; then
  sed 's/listener 8883 0\.0\.0\.0/listener 8883 127.0.0.1/' \
    /mosquitto/config/mosquitto.conf > "$INIT_CONFIG"
  /usr/sbin/mosquitto -c "$INIT_CONFIG" &
  INIT_PID=$!
  trap 'kill -TERM "$INIT_PID" 2>/dev/null || true; wait "$INIT_PID" || true; exit 0' INT TERM

  attempt=0
  while [ ! -s "$DYNSEC_CONFIG" ]; do
    if ! kill -0 "$INIT_PID" 2>/dev/null; then
      wait "$INIT_PID"
      echo "Mosquitto exited before dynamic-security initialization completed." >&2
      exit 1
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 20 ]; then
      echo "Timed out waiting for Mosquitto dynamic-security initialization." >&2
      exit 1
    fi
    sleep 1
  done
  umask 077
  cat > "$CTRL_OPTIONS" <<EOF
--cafile $RUNTIME_DIR/ca.crt
-h 127.0.0.1
-p 8883
-u admin
-P $MOSQUITTO_DYNSEC_PASSWORD
EOF

  attempt=0
  until mosquitto_ctrl -o "$CTRL_OPTIONS" dynsec getClient admin >/dev/null 2>&1; do
    if ! kill -0 "$INIT_PID" 2>/dev/null; then
      wait "$INIT_PID"
      echo "Mosquitto exited before security bootstrap completed." >&2
      exit 1
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 20 ]; then
      echo "Timed out waiting for the local-only Mosquitto security API." >&2
      exit 1
    fi
    sleep 1
  done

  for client in democlient user; do
    if result=$(mosquitto_ctrl -o "$CTRL_OPTIONS" dynsec deleteClient "$client" 2>&1); then
      :
    else
      case "$result" in
        *"Client not found"*) ;;
        *)
          printf '%s\n' "$result" >&2
          echo "Failed to remove default dynamic-security client '$client'." >&2
          exit 1
          ;;
      esac
    fi
  done
  for acl in publishClientSend publishClientReceive subscribe unsubscribe; do
    mosquitto_ctrl -o "$CTRL_OPTIONS" dynsec setDefaultACLAccess "$acl" deny
  done

  touch "$DYNSEC_HARDENED"
  chown mosquitto:mosquitto "$DYNSEC_HARDENED"
  chmod 0600 "$DYNSEC_HARDENED"
  kill -TERM "$INIT_PID"
  wait "$INIT_PID" || true
  trap - INT TERM
  rm -f "$INIT_CONFIG" "$CTRL_OPTIONS"
fi

umask 077
cat > "$HEALTH_OPTIONS" <<EOF
--cafile $RUNTIME_DIR/ca.crt
-h mosquitto
-p 8883
-u admin
-P $MOSQUITTO_DYNSEC_PASSWORD
EOF
chown mosquitto:mosquitto "$HEALTH_OPTIONS"
chmod 0600 "$HEALTH_OPTIONS"

exec /usr/sbin/mosquitto -c /mosquitto/config/mosquitto.conf
