// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com
//
// assembler: human-readable mlvm text -> Program.
//
// Syntax (see design/mlvm_architecture.md):
//
//   ARENA svf-params 5          // manifest: name, chunk size in floats; index = declaration order
//   loop:                       // label, alone on a line or before an instruction
//   SVF.4 r2, r1, svf-params/3, svf-states/3   // ".N" suffix is the bank size, default 0
//   LOAD_I r12, 42.333f         // float immediate: contains '.' or ends in 'f'
//   BNE loop                    // label -> signed 16-bit pc offset
//   HALT
//
// Operand kind is determined by its text, and kind determines its field in the
// instruction word: registers fill fields 1, 2, 4, 5 in order of appearance;
// arena refs (name/chunk) fill fields 4,5 then 6,7; a float immediate fills 4-7;
// a bare integer fills field 5; a label fills 6,7. Comments start with // or ;.
// Mnemonics and register names are case-insensitive.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "mlvm.h"

namespace mlvm {

struct AssemblyError {
  int line{0};          // 1-based source line
  std::string message;
};

struct AssemblyResult {
  Program program;
  std::vector<AssemblyError> errors;
  bool ok() const { return errors.empty(); }
};

// Assemble source text. All errors are collected and reported with line
// numbers; the returned program is only meaningful when ok() is true.
AssemblyResult assemble(std::string_view source);

// Human-readable listing of a program: manifest, then one instruction per line
// with its mnemonic and raw fields.
std::string disassemble(const Program& program);

} // namespace mlvm
