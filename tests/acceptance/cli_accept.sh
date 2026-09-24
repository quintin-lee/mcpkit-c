#!/bin/sh
# CLI acceptance: prove mcpkit-cli inspect/call/test against P3 and P7 services.
# argv: <cli> <stdio-server> <threadpool-server> <apps-host>
set -e

CLI="$1"
STDIO_SRV="$2"
POOL_SRV="$3"
APPS_HOST="$4"

# P3: stdin-driven echo server.
out="$("$CLI" inspect "$STDIO_SRV")"
case "$out" in *echo*) ;; *) echo "FAIL: inspect stdio-server missing echo"; exit 1 ;; esac

out="$("$CLI" call "$STDIO_SRV" echo '{"text":"hi"}')"
case "$out" in *hi*) ;; *) echo "FAIL: call echo missing 'hi'"; exit 1 ;; esac

# discover: stateless capability probe
out="$("$CLI" discover "$STDIO_SRV")"
case "$out" in *stdio-server*) ;; *) echo "FAIL: discover stdio-server missing stdio-server"; exit 1 ;; esac

# listen: real-time notification stream subscription with timeout
"$CLI" listen "$STDIO_SRV" all 1
"$CLI" listen "$STDIO_SRV" tools 1

"$CLI" test "$STDIO_SRV"
"$CLI" test "$POOL_SRV"

# P7: apps-host is a scripted demo (its serve loop owns sessions, so CLI
# cannot drive it); run it directly and check the _meta.ui stamp it prints.
out="$("$APPS_HOST")"
case "$out" in *"_meta"*"resourceUri"*) ;; *) echo "FAIL: apps-host demo missing _meta.ui"; exit 1 ;; esac

# manifest and registry tooling
TMP_MANIFEST="test_mcp.json"
rm -f "$TMP_MANIFEST"
"$CLI" manifest init "$TMP_MANIFEST"
out="$("$CLI" manifest validate "$TMP_MANIFEST")"
case "$out" in *valid*) ;; *) echo "FAIL: manifest validate failed"; rm -f "$TMP_MANIFEST"; exit 1 ;; esac
rm -f "$TMP_MANIFEST"

out="$("$CLI" registry search echo)"
case "$out" in *echo*) ;; *) echo "FAIL: registry search missing echo"; exit 1 ;; esac

out="$("$CLI" registry info "io.modelcontextprotocol/filesystem")"
case "$out" in *filesystem*) ;; *) echo "FAIL: registry info missing filesystem"; exit 1 ;; esac

echo "CLI acceptance: PASS"

