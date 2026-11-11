#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/tab5-profile-tests.XXXXXX")"
trap 'rm -rf -- "$build"' EXIT
compiler="${CXX:-g++}"
flags=(-std=c++11 -Wall -Wextra -Wpedantic -Werror -O1 -g)
# Optional: SANITIZE=1 ./tests/run_profile_tests.sh
if [[ "${SANITIZE:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie)
fi
"$compiler" "${flags[@]}" \
    -I"$root/tests/profile_store" -I"$root/include" \
    "$root/src/ConnectionProfile.cpp" "$root/src/ProfileStore.cpp" \
    "$root/tests/profile_store/FakePreferences.cpp" \
    "$root/tests/profile_store/test_profile_store.cpp" \
    -o "$build/profile_tests"
"$build/profile_tests"
