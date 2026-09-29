#!/usr/bin/env bash
set -euo pipefail

port="${PORT:-18030}"
if [[ ! "$port" =~ ^[0-9]+$ ]] || (( ${#port} > 5 )); then
  printf 'PORT must be a numeric unprivileged TCP port\n' >&2
  exit 64
fi
port=$((10#$port))
if (( port < 1024 || port > 65535 )); then
  printf 'PORT must be between 1024 and 65535\n' >&2
  exit 64
fi

# Web admin is authenticated or disabled, never silently open (ChairLift
# ADR-0016). Every value is validated and the container exits non-zero rather
# than start with a setting the server would silently ignore or weaken: exit 64
# marks a malformed value, exit 78 a value this image cannot honour.
usage_error() {
  printf '%s\n' "$1" >&2
  exit 64
}
config_error() {
  printf '%s\n' "$1" >&2
  exit 78
}

# PAPPL server options this appliance forwards. Everything else pappl-retrofit
# understands either weakens the appliance (no-tls, none) or is already the
# default (web-log, web-network, web-security), so it is not accepted.
allowed_server_options=(no-web-interface)
extra_opts=()
if [[ -n "${PRINTER_APP_SERVER_OPTIONS:-}" ]]; then
  [[ "$PRINTER_APP_SERVER_OPTIONS" =~ ^[a-z-]+(,[a-z-]+)*$ ]] \
    || usage_error 'PRINTER_APP_SERVER_OPTIONS must be a comma-separated list of PAPPL server options'
  IFS=, read -r -a requested_server_options <<< "$PRINTER_APP_SERVER_OPTIONS"
  for option in "${requested_server_options[@]}"; do
    allowed=0
    for candidate in "${allowed_server_options[@]}"; do
      [[ "$option" == "$candidate" ]] && allowed=1
    done
    ((allowed)) || usage_error "PRINTER_APP_SERVER_OPTIONS contains unsupported option '${option}'; supported: ${allowed_server_options[*]}"
  done
  extra_opts+=(-o "server-options=$PRINTER_APP_SERVER_OPTIONS")
fi

# Syntax first (exit 64), then what this image can honour (exit 78), so a
# malformed value is diagnosed the same way on any host.
if [[ -n "${PRINTER_APP_AUTH_SERVICE:-}" ]]; then
  [[ "$PRINTER_APP_AUTH_SERVICE" =~ ^[A-Za-z0-9][A-Za-z0-9_.-]*$ ]] \
    || usage_error 'PRINTER_APP_AUTH_SERVICE must be a PAM service name: letters, digits, "_", "." or "-", not starting with "." or "-"'
fi
if [[ -n "${PRINTER_APP_ADMIN_GROUP:-}" ]]; then
  [[ "$PRINTER_APP_ADMIN_GROUP" =~ ^[A-Za-z_][A-Za-z0-9_.-]*$ ]] \
    || usage_error 'PRINTER_APP_ADMIN_GROUP must be a group name: letters, digits, "_", "." or "-", starting with a letter or "_"'
fi

# The shared printing base builds PAPPL with --disable-libpam, so
# pappl_authenticate_user() rejects every credential no matter which PAM
# service is named, and once auth-service is set even localhost loses the
# unauthenticated path. The presence of /etc/pam.d/<name> (the image ships
# /etc/pam.d/cups with CUPS) says nothing about whether PAPPL can use it, so
# refuse every value until the base builds PAPPL with PAM.
if [[ -n "${PRINTER_APP_AUTH_SERVICE:-}" ]]; then
  config_error "PRINTER_APP_AUTH_SERVICE=${PRINTER_APP_AUTH_SERVICE} cannot be honoured: this image's PAPPL is built without PAM, so auth-service would answer every administration request with 401; set PRINTER_APP_SERVER_OPTIONS=no-web-interface to disable web administration instead"
fi

# admin-group only restricts who may administer once auth-service
# authenticates them, and auth-service is always refused above.
if [[ -n "${PRINTER_APP_ADMIN_GROUP:-}" ]]; then
  config_error 'PRINTER_APP_ADMIN_GROUP requires PRINTER_APP_AUTH_SERVICE; a group cannot be enforced without authentication'
fi

state=/var/lib/hplip-printer-app
mkdir -p "$state/ppd" "$state/spool" "$state/usb" "$state/cups/ssl" "$state/snmp" "$state/run" /run/dbus /run/avahi-daemon /run/hplip-printer-app
if [[ -O "$state" ]]; then chmod 0700 "$state"; fi
if [[ ! -e "$state/cups/snmp.conf" && -f /etc/cups/snmp.conf ]]; then
  cp /etc/cups/snmp.conf "$state/cups/snmp.conf"
fi
if [[ ! -e "$state/usb/org.cups.usb-quirks" && -f /usr/share/cups/usb/org.cups.usb-quirks ]]; then
  cp /usr/share/cups/usb/org.cups.usb-quirks "$state/usb/"
fi

export HOME="$state"
export BACKEND_DIR=/usr/lib/cups/backend
export CUPS_SERVERBIN=/usr/lib/cups
export CUPS_SERVERROOT="$state/cups"
export FILTER_DIR=/usr/lib/cups/filter
export PATH="$FILTER_DIR:/usr/bin:/usr/sbin"
export PPD_PATHS="/usr/share/ppd/:$state/ppd/"
export PPDC_DATADIR=/usr/share/ppdc
export PYTHONPATH=/usr/share/hplip
export SPOOL_DIR="$state/spool"
export STATE_DIR="$state"
export STATE_FILE="$state/hplip-printer-app.state"
export TESTPAGE_DIR=/usr/share/hplip-printer-app
export TMPDIR=/tmp
export USB_QUIRK_DIR="$state"

children=()
stop_children() {
  local index pid
  for ((index = ${#children[@]} - 1; index >= 0; index--)); do
    pid="${children[index]}"
    kill -TERM "$pid" 2>/dev/null || true
  done
  if ((${#children[@]})); then
    wait "${children[@]}" 2>/dev/null || true
  fi
}
handle_signal() {
  trap - TERM INT EXIT
  stop_children
  exit 143
}
trap handle_signal TERM INT
trap stop_children EXIT

dbus-daemon --system --nofork --nopidfile &
children+=("$!")
for _ in $(seq 1 30); do
  [[ -S /run/dbus/system_bus_socket ]] && break
  sleep 0.1
done
[[ -S /run/dbus/system_bus_socket ]]

avahi-daemon --no-drop-root --no-chroot &
children+=("$!")
for _ in $(seq 1 30); do
  [[ -f /run/avahi-daemon/pid ]] && break
  sleep 0.1
done
[[ -f /run/avahi-daemon/pid ]]

hplip-printer-app -o "server-port=$port" -o "log-file=$state/hplip-printer-app.log" "${extra_opts[@]}" server &
children+=("$!")

if wait -n "${children[@]}"; then
  status=1
else
  status=$?
fi
stop_children
trap - TERM INT EXIT
exit "$status"
