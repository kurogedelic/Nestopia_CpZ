#!/bin/sh
# Fetch the upstream sources listed in PINS into third_party/<name> at the pinned commit (a no-op for ones already there).
set -e
cd "$(dirname "$0")"
grep -v '^#' PINS | while read name url commit license; do
  [ -z "$name" ] && continue
  if [ -d "$name/.git" ] && [ "$(git -C "$name" rev-parse HEAD)" = "$commit" ]; then echo "$name: ok ($commit)"; continue; fi
  rm -rf "$name"; git init -q "$name"; git -C "$name" remote add origin "$url"
  git -C "$name" fetch -q --depth 1 origin "$commit"; git -C "$name" checkout -q FETCH_HEAD
  echo "$name: fetched $commit"
done
