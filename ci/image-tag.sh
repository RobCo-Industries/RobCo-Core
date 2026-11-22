#!/bin/sh
# Print the robco-ci image tag: first 12 hex chars of sha256(ci/Dockerfile).
set -eu
dir=$(cd "$(dirname "$0")" && pwd)
if command -v sha256sum >/dev/null 2>&1; then
  sum=$(sha256sum "$dir/Dockerfile")
else
  sum=$(shasum -a 256 "$dir/Dockerfile")
fi
printf '%s\n' "$sum" | cut -c1-12
