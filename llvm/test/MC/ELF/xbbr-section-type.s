## Test that the @llvm_xbbr_attr section type mnemonic assembles to
## SHT_LLVM_XBBR_ATTR and that llvm-readobj reports it symbolically.
## Covers ELFAsmParser.cpp (parse) and MCSectionELF.cpp (print).

# RUN: llvm-mc -triple x86_64-pc-linux -filetype=obj -o %t %s
# RUN: llvm-readobj -S %t | FileCheck %s

.section .llvm_xbbr_attr,"e",@llvm_xbbr_attr
.byte 42

# CHECK: Name: .llvm_xbbr_attr
# CHECK: Type: SHT_LLVM_XBBR_ATTR (0x6FFF4C0E)
