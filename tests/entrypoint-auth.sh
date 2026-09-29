#!/usr/bin/env bash
# Exercise the PRINTER_APP_AUTH_SERVICE / PRINTER_APP_ADMIN_GROUP /
# PRINTER_APP_SERVER_OPTIONS validation at the top of
# files/container-entrypoint.sh without building or running the OCI image.
#
# ChairLift ADR-0016 requires that the web admin interface is either
# authenticated or explicitly disabled, never silently open. These checks
# must fail closed (non-zero exit) on any malformed or unknown value instead
# of falling through to an unauthenticated server start.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
entrypoint="$root/files/container-entrypoint.sh"
marker='^state=/var/lib/hplip-printer-app$'

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Everything before the state directory setup is PORT plus auth validation,
# and it is free of side effects, so it can run on its own.
marker_line="$(grep -n -E "$marker" "$entrypoint" | head -n 1 | cut -d: -f1)"
if [ -z "${marker_line:-}" ]; then
    printf 'tests/entrypoint-auth.sh: no line matching %s in %s\n' \
        "$marker" "$entrypoint" >&2
    exit 1
fi

prologue="$work/auth-prologue.sh"
head -n "$((marker_line - 1))" "$entrypoint" >"$prologue"
printf 'printf "%%s\\n" "${extra_opts[*]:-}"\n' >>"$prologue"

failures=0

report() {
    printf 'FAIL: %s\n' "$1" >&2
    failures=$((failures + 1))
}

# run <label> <expected-status> <expected-output> [env=value ...]
run() {
    local label="$1" want_status="$2" want_output="$3"
    shift 3
    local status=0 output
    output="$(env "$@" bash "$prologue" 2>&1)" || status=$?
    if [ "$status" != "$want_status" ]; then
        report "$label: exit status $status, expected $want_status (output: $output)"
        return
    fi
    if [ "$output" != "$want_output" ]; then
        report "$label: output '$output', expected '$want_output'"
        return
    fi
    printf 'ok: %s\n' "$label"
}

run 'no auth env set adds nothing' 0 ''

run 'auth service with unsafe characters is rejected' \
    64 'PRINTER_APP_AUTH_SERVICE must be a PAM service name (letters, digits, ".", "_", "-")' \
    'PRINTER_APP_AUTH_SERVICE=has space'

run 'auth service is refused while PAPPL lacks PAM' \
    64 'PRINTER_APP_AUTH_SERVICE=cups cannot be honoured: PAPPL is built without PAM in this image; use PRINTER_APP_SERVER_OPTIONS=no-web-interface instead' \
    'PRINTER_APP_AUTH_SERVICE=cups'

run 'admin group without auth service is rejected' \
    64 'PRINTER_APP_ADMIN_GROUP requires PRINTER_APP_AUTH_SERVICE; an admin group with no authentication does not restrict anything' \
    'PRINTER_APP_ADMIN_GROUP=root'

run 'admin group with auth service is still refused' \
    64 'PRINTER_APP_AUTH_SERVICE=cups cannot be honoured: PAPPL is built without PAM in this image; use PRINTER_APP_SERVER_OPTIONS=no-web-interface instead' \
    'PRINTER_APP_AUTH_SERVICE=cups' 'PRINTER_APP_ADMIN_GROUP=root'

run 'single valid server option is forwarded' \
    0 '-o server-options=no-web-interface' \
    'PRINTER_APP_SERVER_OPTIONS=no-web-interface'

run 'multiple valid server options are forwarded verbatim' \
    0 '-o server-options=no-web-interface,no-tls' \
    'PRINTER_APP_SERVER_OPTIONS=no-web-interface,no-tls'

run 'unknown server option is rejected' \
    64 'PRINTER_APP_SERVER_OPTIONS has unknown option bogus-option; allowed: none dnssd-host no-multi-queue raw-socket usb-printer no-web-interface web-log web-network web-remote web-security no-tls' \
    'PRINTER_APP_SERVER_OPTIONS=bogus-option'

run 'one bad option among good ones is still rejected' \
    64 'PRINTER_APP_SERVER_OPTIONS has unknown option bogus-option; allowed: none dnssd-host no-multi-queue raw-socket usb-printer no-web-interface web-log web-network web-remote web-security no-tls' \
    'PRINTER_APP_SERVER_OPTIONS=no-web-interface,bogus-option'

if [ "$failures" -ne 0 ]; then
    printf '%s check(s) failed\n' "$failures" >&2
    exit 1
fi
printf 'All auth-service/admin-group/server-options validation checks passed\n'
