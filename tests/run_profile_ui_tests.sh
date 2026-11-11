#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$ROOT/tests/profile_ui" -I"$ROOT/tests/profile_store" -I"$ROOT/include" \
  "$ROOT/src/ConnectionProfile.cpp" "$ROOT/src/ProfileStore.cpp" \
  "$ROOT/src/ProfileUI.cpp" "$ROOT/tests/profile_store/FakePreferences.cpp" \
  "$ROOT/tests/profile_ui/test_profile_ui.cpp" -o "$BUILD/profile_ui_tests"
"$BUILD/profile_ui_tests"
