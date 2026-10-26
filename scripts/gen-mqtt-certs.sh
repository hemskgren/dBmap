#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROTATE_CA=false
if [[ "${1:-}" == "--rotate-ca" ]]; then
  ROTATE_CA=true
  shift
fi
SHARED="${1:-"$ROOT/shared"}"
LEGACY_CERTS="$ROOT/mqtt/certs"
TRUST_DIR="$SHARED/trust"
CA_DIR="$SHARED/ca"
LAN_IP="${DBMAP_LAN_IP:-}"
PUBLIC_HUB_HOST="${DBMAP_PUBLIC_HUB_HOST:-}"
LOCAL_MQTT_HOST="${DBMAP_LOCAL_MQTT_HOST:-hub.local}"
PUBLIC_MQTT_HOST="${DBMAP_PUBLIC_MQTT_HOST:-${DBMAP_ADVERTISED_MQTT_HOST:-hub.local}}"
if [[ -z "$LAN_IP" ]]; then
  echo "Set DBMAP_LAN_IP (the hub's LAN IPv4 address) before generating certificates." >&2
  exit 1
fi
if [[ ! "$LAN_IP" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
  echo "DBMAP_LAN_IP must be an IPv4 address." >&2
  exit 1
fi

SERVICES=(nginx hub-bootstrap mosquitto dbmap-web opa firmware-builder)

mkdir -p "$TRUST_DIR" "$CA_DIR"
for service in "${SERVICES[@]}"; do
  mkdir -p "$SHARED/$service/certs"
done

migrate_legacy_ca() {
  if [[ -e "$TRUST_DIR/ca.crt" || -e "$CA_DIR/ca.key" ]]; then
    return 0
  fi
  if [[ ! -e "$LEGACY_CERTS/ca.crt" && ! -e "$LEGACY_CERTS/ca.key" ]]; then
    return 0
  fi
  if [[ -e "$LEGACY_CERTS/ca.crt" && -e "$LEGACY_CERTS/ca.key" ]]; then
    cp "$LEGACY_CERTS/ca.crt" "$TRUST_DIR/ca.crt"
    cp "$LEGACY_CERTS/ca.key" "$CA_DIR/ca.key"
    if [[ -e "$LEGACY_CERTS/ca.srl" ]]; then
      cp "$LEGACY_CERTS/ca.srl" "$CA_DIR/ca.srl"
    fi
    echo "Migrated local CA from $LEGACY_CERTS into $SHARED."
    return 0
  fi
  echo "Legacy CA certificate/key pair is incomplete in $LEGACY_CERTS; restore both or use --rotate-ca." >&2
  exit 1
}

if [[ "$ROTATE_CA" == true ]]; then
  rm -f "$TRUST_DIR/ca.crt" "$CA_DIR/ca.key" "$CA_DIR/ca.srl"
  rm -f "$LEGACY_CERTS/ca.crt" "$LEGACY_CERTS/ca.key" "$LEGACY_CERTS/ca.srl"
else
  migrate_legacy_ca
fi

if [[ -e "$TRUST_DIR/ca.crt" && ! -e "$CA_DIR/ca.key" ]] || [[ ! -e "$TRUST_DIR/ca.crt" && -e "$CA_DIR/ca.key" ]]; then
  echo "CA certificate/key pair is incomplete under $SHARED; restore both or use --rotate-ca." >&2
  exit 1
fi
if [[ -e "$TRUST_DIR/ca.crt" ]] && ! openssl x509 -in "$TRUST_DIR/ca.crt" -noout -ext basicConstraints | grep -q 'CA:TRUE'; then
  echo "Existing CA is unsuitable for TLS verification; regenerate it with --rotate-ca." >&2
  exit 1
fi

if [[ ! -e "$TRUST_DIR/ca.crt" ]]; then
  openssl req -x509 -newkey rsa:2048 -days 3650 -nodes \
    -keyout "$CA_DIR/ca.key" \
    -out "$TRUST_DIR/ca.crt" \
    -subj "/CN=dBmap-local-ca" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"
fi

append_san() {
  local -n san_ref=$1
  local host=$2
  [[ -z "$host" ]] && return 0
  local entry
  if [[ "$host" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    entry="IP:${host}"
  elif [[ "$host" =~ ^[A-Za-z0-9.-]+$ ]]; then
    entry="DNS:${host}"
  else
    echo "Certificate host '${host}' must be a DNS name or IPv4 address." >&2
    exit 1
  fi
  case ",${san_ref}," in
    *",${entry},"*) return 0 ;;
  esac
  san_ref="${san_ref:+${san_ref},}${entry}"
}

# service_dir leaf_basename san [cn]
issue_cert() {
  local service=$1
  local name=$2
  local san=$3
  local cn=${4:-$name}
  local out_dir="$SHARED/$service/certs"
  mkdir -p "$out_dir"
  openssl req -newkey rsa:2048 -nodes \
    -keyout "$out_dir/${name}.key" \
    -out "$out_dir/${name}.csr" \
    -subj "/CN=${cn}" \
    -addext "subjectAltName=${san}" \
    -addext "basicConstraints=critical,CA:FALSE" \
    -addext "keyUsage=critical,digitalSignature,keyEncipherment" \
    -addext "extendedKeyUsage=serverAuth"
  openssl x509 -req -in "$out_dir/${name}.csr" -CA "$TRUST_DIR/ca.crt" -CAkey "$CA_DIR/ca.key" \
    -CAserial "$CA_DIR/ca.srl" -CAcreateserial -out "$out_dir/${name}.crt" -days 825 -copy_extensions copy
  openssl x509 -in "$out_dir/${name}.crt" -noout -ext subjectAltName
  rm -f "$out_dir/${name}.csr"
}

# External HTTPS. 127.0.0.1 is the in-container health check; hub.local is the mDNS name.
NGINX_SAN="DNS:hub.local,DNS:localhost,IP:127.0.0.1,IP:${LAN_IP}"
append_san NGINX_SAN "$PUBLIC_HUB_HOST"
append_san NGINX_SAN "$LOCAL_MQTT_HOST"
issue_cert nginx nginx "$NGINX_SAN"

# Internal Hub API. Nginx verifies DNS:hub-bootstrap; the health check uses localhost.
issue_cert hub-bootstrap hub-bootstrap "DNS:hub-bootstrap,DNS:localhost"

# Broker. Internal clients use mosquitto; the entrypoint bootstrap uses 127.0.0.1.
MOSQUITTO_SAN="DNS:mosquitto,IP:127.0.0.1,IP:${LAN_IP}"
append_san MOSQUITTO_SAN "$LOCAL_MQTT_HOST"
append_san MOSQUITTO_SAN "$PUBLIC_MQTT_HOST"
issue_cert mosquitto mosquitto "$MOSQUITTO_SAN"

issue_cert dbmap-web web "DNS:dbmap-web" dbmap-web
issue_cert opa opa "DNS:opa"
issue_cert firmware-builder firmware-builder "DNS:firmware-builder"

# Copy public CA into each service certs dir so Compose can use a single read-only
# directory mount. Docker cannot overlay a file onto a :ro directory mountpoint.
for service in "${SERVICES[@]}"; do
  cp "$TRUST_DIR/ca.crt" "$SHARED/$service/certs/ca.crt"
  chmod 644 "$SHARED/$service/certs/ca.crt"
done

chmod 600 "$CA_DIR/ca.key" \
  "$SHARED/nginx/certs/nginx.key" \
  "$SHARED/hub-bootstrap/certs/hub-bootstrap.key" \
  "$SHARED/mosquitto/certs/mosquitto.key" \
  "$SHARED/dbmap-web/certs/web.key" \
  "$SHARED/opa/certs/opa.key" \
  "$SHARED/firmware-builder/certs/firmware-builder.key"
chmod 644 "$TRUST_DIR/ca.crt"

echo "Wrote dBmap TLS material under $SHARED"
echo "Public CA: $TRUST_DIR/ca.crt (also copied into each shared/<service>/certs/ for mounts)"
echo "CA private key stays host-only at $CA_DIR/ca.key (not mounted into containers)."
