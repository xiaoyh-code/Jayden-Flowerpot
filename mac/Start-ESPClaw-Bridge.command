#!/bin/zsh
set -eu
bridge_dir="${0:A:h}"
if [[ ! -f "$bridge_dir/private/bridge.json" ]]; then
  print "First create the bridge configuration using this Mac's private LAN address:"
  print "python3 '$bridge_dir/espclaw_bridge.py' init --bind 192.168.1.10"
  print "Replace the example address with this Mac's current LAN address."
  read -r "?Press Enter to close. "
  exit 1
fi
exec /usr/bin/env python3 "$bridge_dir/espclaw_bridge.py" serve
