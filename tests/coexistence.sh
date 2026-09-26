#!/usr/bin/env bash
# Prove that two synthetic printer-family appliances built from this image
# can run at the same time without claiming the same port, state volume or
# DNS-SD advertisement. This is a coexistence check for issue #5: it does
# not touch real hardware and does not assert anything about physical USB
# devices being shared correctly, only that two independent instances of
# this appliance never collide with each other on the host.
set -euo pipefail

image=ghcr.io/projectbluefin/hplip-printer-app:build

name_a=hplip-printer-app-coexist-a
name_b=hplip-printer-app-coexist-b
host_a=hplip-printer-app-coexist-a
host_b=hplip-printer-app-coexist-b
port_a="${PORT_A:-18040}"
port_b="${PORT_B:-18041}"

state_a="$(mktemp -d)"
state_b="$(mktemp -d)"

cleanup() {
  podman rm -f "$name_a" "$name_b" >/dev/null 2>&1 || true
  podman unshare rm -rf "$state_a" "$state_b"
}
trap cleanup EXIT

wait_for_http() {
  local expected_port="$1" container="$2" response
  for _ in $(seq 1 90); do
    if response="$(curl --fail --silent "http://127.0.0.1:${expected_port}/" 2>/dev/null)" &&
       [[ "$response" == *'<title>HPLIP Printer Application</title>'* ]]; then
      return 0
    fi
    sleep 1
  done
  podman logs "$container" >&2 || true
  return 1
}

podman unshare chown 65532:65532 "$state_a" "$state_b"

podman run -d --name "$name_a" --network host \
  --hostname "$host_a" -e PORT="$port_a" \
  -v "$state_a:/var/lib/hplip-printer-app:Z" "$image" >/dev/null
podman run -d --name "$name_b" --network host \
  --hostname "$host_b" -e PORT="$port_b" \
  -v "$state_b:/var/lib/hplip-printer-app:Z" "$image" >/dev/null

wait_for_http "$port_a" "$name_a"
wait_for_http "$port_b" "$name_b"

# Both instances are simultaneously reachable on their own ports.
curl --fail --silent "http://127.0.0.1:${port_a}/" | grep -q '<title>HPLIP Printer Application</title>'
curl --fail --silent "http://127.0.0.1:${port_b}/" | grep -q '<title>HPLIP Printer Application</title>'

# Each instance advertises under its own container hostname, not a shared
# or default one, so DNS-SD records for the two families cannot collide.
reported_host_a="$(podman exec "$name_a" hostname)"
reported_host_b="$(podman exec "$name_b" hostname)"
[[ "$reported_host_a" == "$host_a" ]]
[[ "$reported_host_b" == "$host_b" ]]
[[ "$reported_host_a" != "$reported_host_b" ]]

# Each instance owns an isolated, non-shared state volume: adding a printer
# to one must never appear in the other's configuration.
system_uri_a="ipp://127.0.0.1:${port_a}/ipp/system"
system_uri_b="ipp://127.0.0.1:${port_b}/ipp/system"
drivers_a="$(podman exec "$name_a" hplip-printer-app -u "$system_uri_a" drivers)"
model_a="$(printf '%s\n' "$drivers_a" | grep '^hp--deskjet-990-c--en ' | sed -n '1s/[[:space:]].*//p')"
[[ -n "$model_a" ]]
podman exec "$name_a" hplip-printer-app -u "$system_uri_a" \
  -d family-a-printer -m "$model_a" -v "cups:socket://127.0.0.1:9100" add

printers_a="$(podman exec "$name_a" hplip-printer-app -u "$system_uri_a" printers)"
printers_b="$(podman exec "$name_b" hplip-printer-app -u "$system_uri_b" printers)"
[[ "$printers_a" == *family-a-printer* ]]
[[ "$printers_b" != *family-a-printer* ]]

# Each instance's persistent state directory is only its own; USB quirk
# state and CUPS config are not shared between coexisting appliances. Use
# `podman unshare` for the host-side existence checks: the state dirs are
# chowned to the mapped 65532 uid and the entrypoint chmods them 0700, so
# the invoking host user cannot stat inside them directly.
podman exec "$name_a" /usr/bin/bash -c 'test -s /var/lib/hplip-printer-app/usb/org.cups.usb-quirks'
podman exec "$name_b" /usr/bin/bash -c 'test -s /var/lib/hplip-printer-app/usb/org.cups.usb-quirks'
if podman unshare test -e "$state_a/family-a-only-marker"; then
  echo 'unexpected marker present before it was created' >&2
  exit 1
fi
podman exec "$name_a" /usr/bin/bash -c 'touch /var/lib/hplip-printer-app/family-a-only-marker'
podman unshare test -e "$state_a/family-a-only-marker"
if podman unshare test -e "$state_b/family-a-only-marker"; then
  echo 'marker leaked into the other appliance state volume' >&2
  exit 1
fi

printf 'OK: two synthetic printer-family appliances coexisted on distinct ports, hostnames and state\n'
