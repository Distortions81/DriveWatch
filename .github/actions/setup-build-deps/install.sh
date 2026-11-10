#!/usr/bin/env bash
set -euo pipefail
manifest="${1:?Pass the build dependency manifest}"
archives="${RUNNER_TEMP:?RUNNER_TEMP must be set}/m45-apt-archives"
packages=()
while IFS= read -r package || [[ -n "$package" ]]; do
  [[ "$package" =~ ^[[:space:]]*(#|$) ]] && continue
  if [[ ! "$package" =~ ^[a-z0-9][a-z0-9+.-]*(:[a-z0-9-]+)?$ ]]; then
    printf 'Invalid package name in %s: %s\n' "$manifest" "$package" >&2
    exit 1
  fi
  packages+=("$package")
done < "$manifest"
(( ${#packages[@]} > 0 )) || { echo 'Build dependency manifest is empty' >&2; exit 1; }
mkdir -p "$archives"
# Refresh package indexes even on a cache hit. APT still resolves dependencies,
# verifies archives and installs current packages; missing versions are fetched.
sudo apt-get update
sudo apt-get -o "Dir::Cache::archives=$archives" \
  -o APT::Keep-Downloaded-Packages=true \
  install -y --no-install-recommends "${packages[@]}"
