#!/usr/bin/env bash
# Drive the four stdin fuzz harnesses against valid + invalid inputs.
# A harness may exit 1 on a *clean rejection*; that is acceptable.
# Only a crash (killed by a signal, or exit code >= 128) is a failure.
# Usage: fuzz_run.sh <build-dir>
set -u
BUILD_DIR="${1:?usage: fuzz_run.sh <build-dir>}"

fail=0

run_input() {
  # $1 = harness, $2 = label, $3 = input string
  local harness="$1" label="$2" input="$3"
  local rc
  printf '%s' "$input" | "$harness"
  rc=$?
  if (( rc >= 128 )); then
    echo "CRASH  $label (rc=$rc)"
    fail=1
  else
    echo "ok     $label (rc=$rc)"
  fi
}

declare -A H=( [json]=fuzz_json_stdin [schema]=fuzz_schema_stdin \
               [message]=fuzz_message_stdin [http]=fuzz_http_stdin )

for name in json schema message http; do
  bin="$BUILD_DIR/tests/${H[$name]}"
  [[ -f "$bin" ]] || continue
  echo "== $name =="
  case "$name" in
    json)    run_input "$bin" "$name-valid"   '{"a":1,"b":[true,null]}' ;;
    schema)  run_input "$bin" "$name-valid"   '{"type":"string"}' ;;
    message) run_input "$bin" "$name-valid"   '{"jsonrpc":"2.0","id":1,"method":"ping"}' ;;
    http)    run_input "$bin" "$name-valid"   $'GET / HTTP/1.1\r\nHost: x\r\n\r\n' ;;
  esac
  case "$name" in
    json)    run_input "$bin" "$name-invalid"  '{"a":' ;;
    schema)  run_input "$bin" "$name-invalid"  '{"type":' ;;
    message) run_input "$bin" "$name-invalid"  '{"jsonrpc":' ;;
    http)    run_input "$bin" "$name-invalid"  'GET / HTT' ;;
  esac
  run_input "$bin" "$name-empty" ''
done

if (( fail )); then
  echo "FUZZ RUN: FAILED (crash detected)"
  exit 1
fi
echo "FUZZ RUN: all harnesses clean"
exit 0
