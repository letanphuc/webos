#!/usr/bin/env sh
set -eu

host="${1:-192.168.50.17}"
curl --fail --show-error --max-time 5 -i "http://${host}:8080/"
