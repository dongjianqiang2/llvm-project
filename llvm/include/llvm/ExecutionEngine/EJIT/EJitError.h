//===-- EJitError.h - EmbeddedJIT Error Types -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITERROR_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITERROR_H

#include <string>

namespace llvm {
namespace ejit {

struct EJitError {
  int code;
  std::string message;
  std::string funcName;
};

} // namespace ejit
} // namespace llvm

#endif
