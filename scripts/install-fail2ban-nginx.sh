#!/usr/bin/env bash
# Install host fail2ban jails for dBmap Hub HTTPS API 401s and MQTT auth failures.
# Either jail bans both the HTTPS and MQTT published ports (DOCKER-USER).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NGINX_LOGDIR="${ROOT}/shared/nginx/logs"
NGINX_LOGPATH="${NGINX_LOGDIR}/access.log"
MQTT_LOGDIR="${ROOT}/shared/mosquitto/logs"
MQTT_LOGPATH="${MQTT_LOGDIR}/mosquitto.log"
FILTER_NGINX="${ROOT}/deploy/fail2ban/filter.d/dbmap-nginx-auth.conf"
FILTER_MQTT="${ROOT}/deploy/fail2ban/filter.d/dbmap-mqtt-auth.conf"
JAIL_NGINX_IN="${ROOT}/deploy/fail2ban/jail.d/dbmap-nginx.local.in"
JAIL_MQTT_IN="${ROOT}/deploy/fail2ban/jail.d/dbmap-mqtt.local.in"
LOGROTATE_IN="${ROOT}/deploy/logrotate/dbmap-hub"

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo $0" >&2
  exit 1
fi

for f in "${FILTER_NGINX}" "${FILTER_MQTT}" "${JAIL_NGINX_IN}" "${JAIL_MQTT_IN}"; do
  if [[ ! -f "${f}" ]]; then
    echo "Missing fail2ban template: ${f}" >&2
    exit 1
  fi
done

read_env_port() {
  local key="$1"
  local default="$2"
  local value="${!key:-}"
  if [[ -z "${value}" && -f "${ROOT}/.env" ]]; then
    value="$(grep -E "^${key}=" "${ROOT}/.env" | tail -n1 | cut -d= -f2- | tr -d '"' | tr -d "'")"
  fi
  if [[ -n "${value}" ]]; then
    printf '%s\n' "${value}"
  else
    printf '%s\n' "${default}"
  fi
}

HTTPS_PORT="$(read_env_port DBMAP_HTTPS_PORT 8443)"
MQTT_PORT="$(read_env_port DBMAP_LOCAL_MQTT_PORT 8883)"
# Compose publishes host MQTT as 8883 today; keep jail in sync with that mapping.
if [[ "${MQTT_PORT}" != "8883" ]]; then
  echo "Note: compose publishes Mosquitto as host port 8883; jail will use 8883." >&2
  MQTT_PORT=8883
fi

mkdir -p "${NGINX_LOGDIR}" "${MQTT_LOGDIR}"
touch "${NGINX_LOGPATH}" "${MQTT_LOGPATH}"
chmod 755 "${NGINX_LOGDIR}" "${MQTT_LOGDIR}"
chmod 644 "${NGINX_LOGPATH}" "${MQTT_LOGPATH}"

if ! command -v fail2ban-client >/dev/null 2>&1; then
  echo "Installing fail2ban…"
  apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq fail2ban
fi

install -m 644 "${FILTER_NGINX}" /etc/fail2ban/filter.d/dbmap-nginx-auth.conf
install -m 644 "${FILTER_MQTT}" /etc/fail2ban/filter.d/dbmap-mqtt-auth.conf

render_jail() {
  local src="$1"
  local dest="$2"
  local tmp
  tmp="$(mktemp)"
  sed \
    -e "s|@@NGINX_LOGPATH@@|${NGINX_LOGPATH}|g" \
    -e "s|@@MQTT_LOGPATH@@|${MQTT_LOGPATH}|g" \
    -e "s|@@HTTPS_PORT@@|${HTTPS_PORT}|g" \
    -e "s|@@MQTT_PORT@@|${MQTT_PORT}|g" \
    "${src}" >"${tmp}"
  install -m 644 "${tmp}" "${dest}"
  rm -f "${tmp}"
}

render_jail "${JAIL_NGINX_IN}" /etc/fail2ban/jail.d/dbmap-nginx.local
render_jail "${JAIL_MQTT_IN}" /etc/fail2ban/jail.d/dbmap-mqtt.local

# Drop older single-service logrotate name if present.
rm -f /etc/logrotate.d/dbmap-nginx
if [[ -f "${LOGROTATE_IN}" ]]; then
  tmp_lr="$(mktemp)"
  sed \
    -e "s|@@NGINX_LOGDIR@@|${NGINX_LOGDIR}|g" \
    -e "s|@@MQTT_LOGDIR@@|${MQTT_LOGDIR}|g" \
    "${LOGROTATE_IN}" >"${tmp_lr}"
  install -m 644 "${tmp_lr}" /etc/logrotate.d/dbmap-hub
  rm -f "${tmp_lr}"
fi

systemctl enable fail2ban >/dev/null
systemctl restart fail2ban

echo
echo "Installed fail2ban jails: dbmap-nginx, dbmap-mqtt"
echo "  nginx log: ${NGINX_LOGPATH}"
echo "  mqtt log:  ${MQTT_LOGPATH}"
echo "  ban ports: ${HTTPS_PORT}/tcp + ${MQTT_PORT}/tcp (DOCKER-USER)"
echo "  rule: 10 failures within 10m → ban 1h (either jail closes both ports)"
echo "  ignore: localhost, 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16"
echo
echo "Check:"
echo "  sudo fail2ban-client status"
echo "  sudo fail2ban-client status dbmap-nginx"
echo "  sudo fail2ban-client status dbmap-mqtt"
echo
echo "Unban one IP (run for each jail that listed it):"
echo "  sudo fail2ban-client set dbmap-nginx unbanip <ip>"
echo "  sudo fail2ban-client set dbmap-mqtt unbanip <ip>"
echo
echo "Stop / disable:"
echo "  sudo fail2ban-client stop dbmap-nginx"
echo "  sudo fail2ban-client stop dbmap-mqtt"
echo "  sudo systemctl stop fail2ban"
echo "  sudo systemctl disable fail2ban"
echo
echo "Ensure log files are mounted:"
echo "  cd ${ROOT} && docker compose up -d --build nginx && docker compose up -d mosquitto"
