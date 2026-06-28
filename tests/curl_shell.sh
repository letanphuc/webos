#!/usr/bin/env sh
set -eu

host="${1:-192.168.50.17}"
cmd="${2:-kernel version}"

curl --fail --show-error --max-time 8 -i \
	-X POST "http://${host}:8080/shell" \
	-H 'Content-Type: application/json' \
	-d "{\"cmd\":\"${cmd}\"}"
