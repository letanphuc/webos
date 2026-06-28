#!/usr/bin/env sh
set -eu

host="${1:-192.168.50.17}"
path="${2:-/STORAGE:/apps/curltest.txt}"
file="${3:-hello from curl}"

curl --fail --show-error --max-time 8 -i \
	-X POST "http://${host}:8080/push" \
	-H 'Content-Type: application/json' \
	-d "{\"path\":\"${path}\",\"file\":\"${file}\"}"
