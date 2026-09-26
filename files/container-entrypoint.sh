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
# ADR-0016). name_re bounds auth-service and admin-group to values PAM and
# getent can look up safely; server-options is checked against PAPPL's own
# documented token set instead of being passed through verbatim.
name_re='^[A-Za-z0-9_.-]+$'
auth_service="${PRINTER_APP_AUTH_SERVICE:-}"
admin_group="${PRINTER_APP_ADMIN_GROUP:-}"
server_options="${PRINTER_APP_SERVER_OPTIONS:-}"
extra_opts=()

if [[ -n "$auth_service" ]]; then
  if [[ ! "$auth_service" =~ $name_re ]]; then
    printf 'PRINTER_APP_AUTH_SERVICE must be a PAM service name (letters, digits, ".", "_", "-")\n' >&2
    exit 64
  fi
  if [[ ! -e "/etc/pam.d/$auth_service" ]]; then
    printf 'PRINTER_APP_AUTH_SERVICE=%s has no /etc/pam.d/%s in this image; ship that PAM service or use PRINTER_APP_SERVER_OPTIONS=no-web-interface instead\n' \
      "$auth_service" "$auth_service" >&2
    exit 64
  fi
  extra_opts+=(-o "auth-service=$auth_service")
fi

if [[ -n "$admin_group" ]]; then
  if [[ -z "$auth_service" ]]; then
    printf 'PRINTER_APP_ADMIN_GROUP requires PRINTER_APP_AUTH_SERVICE; an admin group with no authentication does not restrict anything\n' >&2
    exit 64
  fi
  if [[ ! "$admin_group" =~ $name_re ]]; then
    printf 'PRINTER_APP_ADMIN_GROUP must be a group name (letters, digits, ".", "_", "-")\n' >&2
    exit 64
  fi
  if ! getent group "$admin_group" >/dev/null; then
    printf 'PRINTER_APP_ADMIN_GROUP=%s does not exist in this image\n' "$admin_group" >&2
    exit 64
  fi
  extra_opts+=(-o "admin-group=$admin_group")
fi

if [[ -n "$server_options" ]]; then
  allowed_options=(none dnssd-host no-multi-queue raw-socket usb-printer no-web-interface web-log web-network web-remote web-security no-tls)
  IFS=',' read -r -a requested_options <<<"$server_options"
  for option in "${requested_options[@]}"; do
    known=0
    for allowed in "${allowed_options[@]}"; do
      if [[ "$option" == "$allowed" ]]; then
        known=1
        break
      fi
    done
    if [[ "$known" -ne 1 ]]; then
      printf 'PRINTER_APP_SERVER_OPTIONS has unknown option %s; allowed: %s\n' \
        "$option" "${allowed_options[*]}" >&2
      exit 64
    fi
  done
  extra_opts+=(-o "server-options=$server_options")
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
