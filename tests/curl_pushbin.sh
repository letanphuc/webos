#!/usr/bin/env sh
set -eu

host="${1:-192.168.50.17}"
local="${2:-}"
path="${3:-/STORAGE:/apps/curltest.bin}"

if [ -z "$local" ]; then
	echo "usage: $0 <host> <local-file> [remote-path]" >&2
	exit 1
fi

curl --fail --show-error --max-time 30 -i \
	-X POST "http://${host}:8080/pushbin" \
	-H "X-Webos-Path: ${path}" \
	-H 'Content-Type: application/octet-stream' \
	--data-binary "@${local}"
