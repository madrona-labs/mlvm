// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include "madronalib.h"

namespace mlvm {


// OPCODES tell our virtual machine what to do. They have an operation and a mode.

using Opcode = uint8_t;

constexpr int kOpcodeOperationBits{6};
constexpr int kNumOperations{1 << kOpcodeOperationBits};
constexpr uint8_t kOpcodeOperationMask{ (uint8_t)kNumOperations - 1 };
constexpr uint8_t kOpcodeModeMask{ (uint8_t)~kOpcodeOperationMask };

inline size_t getOperationMode(Opcode m) { return (m&kOpcodeModeMask) >> kOpcodeOperationBits; }
inline size_t getOperation(Opcode m) { return (m&kOpcodeOperationMask); }

enum operations {
  NOOP = 0,
  END,
  MOVE,     // register -> register
  MOVE1,
  LOAD,     // memory -> register
  LOAD1,
  STORE,    // register -> memory
  CMP,
  BNE,
  JMP,
  ADD,
  TEST1,
  TEST2,
  MUL,
  SHIFT,
  INTERP,
  SVF,       // dest, src, state
  // ... and many more
  // many opcodes will be much bigger chunks of stateful work like oscillators, table lookups,
  // env followers, and in general DSP machinery.
  NUM_OPERATIONS
};

static_assert(NUM_OPERATIONS < kNumOperations);

// reserved
enum opcodeModes {
  MODE_0 = 0,
  MODE_1
};


// OPERANDS specify sources and destinations for operations. They have an index and a mode.
// The mode specifies how the index will be used to calculate a source or destination.

using Operand = uint8_t;

constexpr size_t kOperandIndexBits{7};
constexpr size_t kNumOperandIndexes{1 << kOperandIndexBits};
constexpr size_t kNumRegisters{kNumOperandIndexes};
constexpr uint8_t kOperandIndexMask{ (uint8_t)kNumOperandIndexes - 1 };
constexpr uint8_t kOperandModeMask{ (uint8_t)~kOperandIndexMask };

inline size_t getOperandMode(Operand m) { return (m&kOperandModeMask) >> kOperandIndexBits; }
inline size_t getIndex(Operand m) { return (m&kOperandIndexMask); }


// Register operands have two modes: register and immediate.
// In the register mode the source or destination is one of the registers.
// In the immediate mode the source is a float encoded as a 7-bit value.

enum registerAddressModes {
  REGISTER = 0,
  IMMEDIATE = 1
};

// NOTE how to encode immediates? 2^n with an offset, or possibly even a table of 128 useful values.

// SILLY - JUST MAKE ROOM FOR FLOAT32

// table idea: 0, [1/64 -- 1/2], [1 -- 64]
inline float getImmediate(Operand op) { return float(getIndex(op)); }

// Memory operands have two modes: arena and literal.
// In the arena mode the program's persistent working memory is the source or destination.
// Arena loads / stores will use two of the operands to make a memory offset.
// In the literal mode the source is stored in the program memory, in what would be the
// following instruction.

enum memoryAddressModes {
  ARENA = 0,
  LITERAL = 1
};

// INSTRUCTIONS are combinations of opcodes and operands.

struct Instruction {
  Opcode opcode;
  Operand dest;
  Operand src1;
  Operand src2;
};

// Instructions are 4 bytes long.

static_assert(sizeof(Instruction) == 4);

// for a few instructions like MUL_ADD, the operands can be restricted to registers, so we
// can pack four register indices (6 bits * 4) as operands if we want to.

struct MemoryRequirements {
  // number of vectors a module or program needs to store its persistent state.
  size_t stateVectors;
  
  // number of scratch memory vectors a module needs for temporary storage -
  // not saved between process() calls. Typically a program will allocate scratch
  // storage for the module needing the most scratch, and all modules will share
  // that scratch area.
  size_t scratchVectors;
};

struct Program {
  std::vector< Instruction > instructions;
  std::vector< float > literalPool;
  MemoryRequirements memReqs;
};

struct MLVM {
  std::vector< DSPVector > registers;
  std::vector< DSPVector > arena;
  Program program;
  uint32_t programCounter;
  
  //void compile(const JSON& dspGraphInput, Program& programOutput); // TODO - takes JSON list of modules and connections and parameters, makes opcodes and memory needs
  
  // NOTES
  // A benefit from compiling the module graph into opcodes is that we can take care of any mode-switch
  // decisions at opcode-making ("compile") time.
  // Filter -> West Coast, East Coast (different personalities)
  // projection -> linear, quadratic, log, exp, ...
  // Distortion flavors, Compression flavors
  // body types, Space modes, ...
  // If modules share an interface and basic concept they can be rolled into one with a compile-time
  // switch for the "flavor."
  //
  // For crossfades on changes and super-quick undo, we can keep N versions of the program.

  bool allocateMemory(const MemoryRequirements&);
  void setProgram(const Program& newCode);
  void process(AudioContext* context);
  
private:
  DSPVector getValue(Operand op1);
  DSPVector getValue2(Operand op1, Operand op2, const std::vector< float >& literals);
  DSPVector* getDest2(Operand op1, Operand op2);

};

} // namespace ml

