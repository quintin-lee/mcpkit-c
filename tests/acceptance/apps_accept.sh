#!/bin/sh
# Apps acceptance: tool-level _meta, result _meta.ui, ui:// resource read over stdio.
# argv: <cli> <apps-server>
set -e

CLI="$1"
APPS_SRV="$2"

# tools/list advertises tool-level _meta.ui + resources/list shows ui:// resource
out="$("$CLI" inspect "$APPS_SRV" 2>/dev/null)"
case "$out" in *resourceUri*ui://app/greet*) ;; *) echo "FAIL: inspect missing tool _meta.ui"; exit 1 ;; esac
case "$out" in *text/html*profile=mcp-app*) ;; *) echo "FAIL: inspect missing ui:// resource"; exit 1 ;; esac

# tools/call result carries _meta.ui
out="$("$CLI" call "$APPS_SRV" greet '{"name":"qi"}' 2>/dev/null)"
case "$out" in *hello*qi*resourceUri*ui://app/greet*) ;; *) echo "FAIL: call missing greeting or result _meta.ui"; exit 1 ;; esac

# resources/read over a direct stdio pipe (CLI has no read subcommand)
out="$(printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"acc","version":"1"}}}' \
  '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
  '{"jsonrpc":"2.0","id":2,"method":"resources/read","params":{"uri":"ui://app/greet"}}' \
  | "$APPS_SRV" 2>/dev/null)"
case "$out" in *serverInfo*apps-server*) ;; *) echo "FAIL: apps-server handshake"; exit 1 ;; esac
case "$out" in *Content-Security-Policy*default-src*) ;; *) echo "FAIL: resources/read missing CSP meta"; exit 1 ;; esac
case "$out" in *apps-server*'</p>'*) ;; *) echo "FAIL: resources/read missing HTML body"; exit 1 ;; esac

echo "apps acceptance OK"
