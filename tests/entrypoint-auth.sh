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

pam_syntax='PRINTER_APP_AUTH_SERVICE must be a PAM service name: letters, digits, "_", "." or "-", not starting with "." or "-"'
group_syntax='PRINTER_APP_ADMIN_GROUP must be a group name: letters, digits, "_", "." or "-", starting with a letter or "_"'
no_pam="this image's PAPPL is built without PAM, so auth-service would answer every administration request with 401; set PRINTER_APP_SERVER_OPTIONS=no-web-interface to disable web administration instead"
group_needs_auth='PRINTER_APP_ADMIN_GROUP requires PRINTER_APP_AUTH_SERVICE; a group cannot be enforced without authentication'
options_syntax='PRINTER_APP_SERVER_OPTIONS must be a comma-separated list of PAPPL server options'

run 'auth service with unsafe characters is rejected' \
    64 "$pam_syntax" 'PRINTER_APP_AUTH_SERVICE=has space'
run 'auth service path traversal is rejected' \
    64 "$pam_syntax" 'PRINTER_APP_AUTH_SERVICE=../shadow'
run 'auth service starting with a dash is rejected' \
    64 "$pam_syntax" 'PRINTER_APP_AUTH_SERVICE=-login'

# Refused whether or not /etc/pam.d/<name> exists (cups ships one with CUPS).
run 'auth service cups is refused while PAPPL lacks PAM' \
    78 "PRINTER_APP_AUTH_SERVICE=cups cannot be honoured: $no_pam" \
    'PRINTER_APP_AUTH_SERVICE=cups'
run 'auth service without a PAM file is refused the same way' \
    78 "PRINTER_APP_AUTH_SERVICE=chairlift-printer cannot be honoured: $no_pam" \
    'PRINTER_APP_AUTH_SERVICE=chairlift-printer'

run 'malformed admin group is rejected' \
    64 "$group_syntax" 'PRINTER_APP_ADMIN_GROUP=bad;group'
run 'admin group without auth service is refused' \
    78 "$group_needs_auth" 'PRINTER_APP_ADMIN_GROUP=root'
run 'admin group with auth service is still refused' \
    78 "PRINTER_APP_AUTH_SERVICE=cups cannot be honoured: $no_pam" \
    'PRINTER_APP_AUTH_SERVICE=cups' 'PRINTER_APP_ADMIN_GROUP=root'

run 'no-web-interface is forwarded' \
    0 '-o server-options=no-web-interface' \
    'PRINTER_APP_SERVER_OPTIONS=no-web-interface'

# Only no-web-interface is accepted: no-tls and none weaken the appliance.
for option in no-tls none web-remote bogus-option; do
    run "server option $option is rejected" \
        64 "PRINTER_APP_SERVER_OPTIONS contains unsupported option '$option'; supported: no-web-interface" \
        "PRINTER_APP_SERVER_OPTIONS=no-web-interface,$option"
done
run 'trailing comma is rejected' \
    64 "$options_syntax" 'PRINTER_APP_SERVER_OPTIONS=no-web-interface,'
run 'space-separated options are rejected' \
    64 "$options_syntax" 'PRINTER_APP_SERVER_OPTIONS=no-web-interface no-tls'

if [ "$failures" -ne 0 ]; then
    printf '%s check(s) failed\n' "$failures" >&2
    exit 1
fi
printf 'All auth-service/admin-group/server-options validation checks passed\n'
