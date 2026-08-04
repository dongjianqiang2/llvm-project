#===-- GenerateEJitVersion.cmake - bake git commit into EJitVersion.h ------===#
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
#===----------------------------------------------------------------------===//
#
# CMake -P script invoked by a custom target on every build (see CMakeLists.txt
# in this directory). It queries the llvm-project git repository for the current
# HEAD commit and branch, then writes EJitVersion.h with copy_if_different so
# the header's mtime (and therefore EJitRuntime.cpp) only changes when the
# commit actually changes. This keeps ejit_print_version() fresh across
# rebuilds without forcing a full CMake reconfigure.
#
# Inputs:
#   GIT_EXECUTABLE - path to git (may be empty if git was not found)
#   SOURCE_DIR     - a directory inside the llvm-project tree (git walks up)
#   OUT_FILE       - absolute path to the generated EJitVersion.h
#
# When git is unavailable or the source tree is not a checkout, the macros fall
# back to "unknown" so the runtime still builds and ejit_print_version() still
# reports the LLVM release version (from llvm/Config/llvm-config.h).
#
#===----------------------------------------------------------------------===//

set(_commit "")
set(_branch "")

if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE _commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    set(_commit "")
  endif()

  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE _branch
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    set(_branch "")
  endif()
endif()

if(NOT _commit)
  set(_commit "unknown")
endif()
if(NOT _branch)
  set(_branch "unknown")
endif()

# Escape backslashes/quotes so the values survive being placed in a string
# literal. Commit hashes and branch names are otherwise plain ASCII.
string(REPLACE "\\" "\\\\" _commit_esc "${_commit}")
string(REPLACE "\"" "\\\"" _commit_esc "${_commit_esc}")
string(REPLACE "\\" "\\\\" _branch_esc "${_branch}")
string(REPLACE "\"" "\\\"" _branch_esc "${_branch_esc}")

file(WRITE "${OUT_FILE}.tmp"
"//===-- EJitVersion.h - EmbeddedJIT build version (generated) ------------===//
//
// Generated at build time by llvm/lib/ExecutionEngine/EJIT/GenerateEJitVersion.cmake.
// Do not edit by hand; this header is regenerated on every build and overwritten.
//
// EJIT_GIT_COMMIT is the full hash of the llvm-project source tree HEAD at the
// time the runtime was built; EJIT_GIT_BRANCH is its checked-out branch (or
// \"unknown\" when git is unavailable or the source is not a checkout). The LLVM
// release version is intentionally NOT duplicated here: it lives in
// llvm/Config/llvm-config.h (LLVM_VERSION_STRING) and is consumed directly.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EJIT_EJITVERSION_H
#define LLVM_EJIT_EJITVERSION_H

#define EJIT_GIT_COMMIT \"${_commit_esc}\"
#define EJIT_GIT_BRANCH \"${_branch_esc}\"

#endif // LLVM_EJIT_EJITVERSION_H
")

execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${OUT_FILE}.tmp" "${OUT_FILE}")
file(REMOVE "${OUT_FILE}.tmp")
