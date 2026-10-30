#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The ahead-of-time guest CPU's unit tests (cpu/aot; from the Android port's tests/, github.com/ducvd89/Bloodborne-Android, the include
# paths changed to cpu/aot): test_aot_native.cpp (the LSE atomic emulator for unaligned x86 `lock` operations and the CPUID model, any host),
# test_aot_strict.cpp (granular dispatch, the misses log, strict mode, the import-trap peephole) and test_aot_variants.cpp (patch variants
# and native hook slots on synthetic tables); those two include the engine with the test services of tests/aot_test_services.h, which
# exist for x86-64 hosts only, so elsewhere only the first runs. No game files, no libbbaot.so.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
mkdir -p out
tests=(native)
if [[ $HOSTTYPE == x86_64 ]]; then tests+=(strict variants); else echo "aot strict and variants tests: x86-64 hosts only"; fi
for t in "${tests[@]}"; do
    nice -n 19 "${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -Icpu/aot -Isrc tests/test_aot_$t.cpp -ldl -o out/aot-$t-test
    out/aot-$t-test
done
