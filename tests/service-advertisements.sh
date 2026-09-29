#!/usr/bin/env bash
# Real host-network observation; run on an otherwise quiet test LAN.
#
# The image inherits Avahi's sample ssh.service and sftp-ssh.service records.
# Nothing in this appliance serves SSH or SFTP, so publishing them under the
# host's name misdirects LAN users. This proves the built image carries no such
# records and that starting or restarting instances of it adds none, while the
# appliance's own IPP queues stay discoverable.
set -euo pipefail

image="${IMAGE:-ghcr.io/projectbluefin/hplip-printer-app:build}"
app="${APP:-hplip-printer-app}"
state_root="${STATE_ROOT:-/var/lib/$app}"
port="${PORT:-18046}"
evidence="${EVIDENCE_DIR:-$(mktemp -d)}"
mkdir -p "$evidence"
state_dir="$(mktemp -d)"
prefix="service-advertisements-$$"
names=("$prefix-alpha" "$prefix-beta")
cleanup() {
  for name in "${names[@]}"; do
    podman logs "$name" > "$evidence/$name.log" 2>&1 || true
    podman rm -f "$name" >/dev/null 2>&1 || true
  done
  podman unshare rm -rf "$state_dir"
}
trap cleanup EXIT
for command in podman avahi-browse timeout curl; do
  command -v "$command" >/dev/null
done
podman image inspect "$image" > "$evidence/image.json"
podman run --rm --entrypoint /usr/bin/bash "$image" -ec '
  test ! -e /etc/avahi/services/ssh.service
  test ! -e /etc/avahi/services/sftp-ssh.service
'

snapshot() {
  local phase="$1" service
  for service in _ssh._tcp _sftp-ssh._tcp _ipp._tcp; do
    timeout 30 avahi-browse --resolve --terminate --parsable "$service" \
      > "$evidence/$phase.$service"
  done
}
remote_records() {
  # Ignore interface/protocol duplication, retain instance, host, address, port.
  awk -F ';' '$1 == "=" {print $4 ";" $5 ";" $6 ";" $7 ";" $8 ";" $9}' "$1" | LC_ALL=C sort -u
}
check_records() {
  local phase="$1" service index
  for service in _ssh._tcp _sftp-ssh._tcp; do
    diff -u <(remote_records "$evidence/before.$service") \
      <(remote_records "$evidence/$phase.$service")
  done
  for index in 0 1; do
    # A resolved queue record must advertise this instance's distinct IPP port.
    awk -F ';' -v port="$((port + index))" -v queue="${names[index]}" '
      $1 == "=" && $9 == port && index($0, "rp=ipp/print/" queue) {found=1}
      END {exit !found}
    ' "$evidence/$phase._ipp._tcp"
  done
}
wait_for_http() {
  local target="$1" container="$2"
  for _ in $(seq 1 90); do
    if curl --fail --silent "http://127.0.0.1:$target/" >/dev/null; then
      return 0
    fi
    sleep 1
  done
  podman logs "$container" >&2 || true
  return 1
}

snapshot before
for index in 0 1; do
  mkdir "$state_dir/$index"
  podman unshare chown 65532:65532 "$state_dir/$index"
  podman run -d --name "${names[index]}" --network host \
    -e PORT="$((port + index))" \
    -v "$state_dir/$index:$state_root:Z" "$image" >/dev/null
  wait_for_http "$((port + index))" "${names[index]}"
  system_uri="ipp://127.0.0.1:$((port + index))/ipp/system"
  drivers="$(podman exec "${names[index]}" "$app" -u "$system_uri" drivers)"
  model="$(printf '%s\n' "$drivers" | grep -i 'deskjet 990c' | sed -n '1s/[[:space:]].*//p')"
  test -n "$model"
  podman exec "${names[index]}" "$app" -u "$system_uri" \
    -d "${names[index]}" -m "$model" -v "cups:socket://127.0.0.1:19999" add
done
# Allow mDNS probing/announcements to settle before each observation.
sleep 5
snapshot started
check_records started
podman restart --time 15 "${names[0]}" >/dev/null
wait_for_http "$port" "${names[0]}"
sleep 5
snapshot restarted
check_records restarted
printf 'PASS: SSH/SFTP records unchanged; both IPP queues resolve after startup/restart.\n'
printf 'Evidence: %s\nNo physical discovery or printed paper was tested.\n' "$evidence"
