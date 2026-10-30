# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# libbbcpu.so on ahead-of-time recompiled code: src/guest_cpu.h as the units of a libbbaot.so (BB_AOT=<file>, made by bb-recomp, the
# Bloodborne Android Rebuild Tool) and nothing else: no interpreter, JIT or fallback (docs/RECOMP_ABI.md of the tool, "Strict mode").
# The other guest CPU of AArch64 hosts is FEXCore (cpu/bbcpu.cmake, out/fex/libbbcpu.so); run.sh picks one of the two at start
# (docs/AOT_LAUNCHER_PLAN.md). Both export the same functions under the same soname, so bb-probe and libbbgpu link either.
#
# The sources in this folder (all but this file) are the Android port's cpu/ (github.com/ducvd89/Bloodborne-Android, GPL-2.0-or-later,
# commit 3d8abf7 of cpu/), copied unchanged so that later fixes there apply here too; host_call_arm64.S is cpu/'s own (the same file).
# Built by build.sh on AArch64 hosts into out/aot-cpu/ (not out/cpu/: bb-probe's RUNPATH $ORIGIN/cpu is the FEX library's place in the
# packaged layout).
cmake_minimum_required(VERSION 3.20)
project(bbcpu_aot C CXX ASM)
set(CPU_DIR "${CMAKE_CURRENT_LIST_DIR}")
add_library(bbcpu SHARED "${CPU_DIR}/guest_cpu_aot.cpp" "${CPU_DIR}/guest_cpu_native.cpp" "${CPU_DIR}/../host_call_arm64.S")
target_include_directories(bbcpu PRIVATE "${CPU_DIR}" "${CPU_DIR}/../../src")
target_compile_features(bbcpu PRIVATE cxx_std_20)
target_compile_options(bbcpu PRIVATE -O2 -g -Wall -Wextra -Werror -fno-strict-aliasing)
target_link_libraries(bbcpu PRIVATE ${CMAKE_DL_LIBS} pthread)
set_target_properties(bbcpu PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN TRUE)
