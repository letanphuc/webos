#!/usr/bin/env sh
set -eu

host="${1:-192.168.50.17}"
image="${2:-build/app/zephyr/zephyr.signed.bin}"

curl -v --fail --show-error --max-time 120 -i \
	-X POST "http://${host}:8080/ota" \
	--data-binary "@${image}"
