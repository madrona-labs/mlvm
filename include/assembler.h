// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com
//
// assembler: human-readable mlvm text -> Program.
//
// Syntax (see design/mlvm_architecture.md):
//
//   ARENA svf-params 5          // manifest: name, chunk size in floats; index = declaration order
//   EXTERN audioin 64           // manifest: an arena the host binds by name before each block
//   loop:                       // label, alone on a line or before an instruction
//   SVF.4 r2, r1, svf-params/3, svf-states/3   // ".N" suffix is the bank size, default 0
//   LOAD_I r12, 42.333f         // float immediate: contains '.' or ends in 'f'
//   BNE loop                    // label -> signed 16-bit pc offset
//   HALT
//
// Operand kind is determined by its text: rN is a register, name/chunk an arena
// reference, 1.0f a float immediate, a bare integer an element number, anything
// else a label. Where an operand lands in the instruction word is decided by the
// opcode's format, its row in the instruction table (see OpFormat and formatSpec
// in mlvm.h), never by its position among other kinds. Operand count and kinds
// are checked against the format. Comments start with // or ;. A line ending in a
// backslash continues on the next line. Mnemonics and register names are
// case-insensitive.

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
