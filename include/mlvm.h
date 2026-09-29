// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include "madronalib.h"


// TODO make external interface


namespace mlvm {

using Opcode = uint8_t;

enum operations {
  // System
  NOOP = 0,
  END,
  // One-input math
  SIN,
  LOG,
  EXP,
  NEG,
  ABS,
  RCP,
  // Two-input math
  ADD,
  ADD_WRAP,
  SUB,
  SUB_WRAP,
  MUL,
  DIV,
  // Three-input math
  LERP,
  CLAMP,
  SELECT,
  // Generators
  SINE,
  IMPULSE,
  SAW,
  NOISE,
  // Filters
  SVF,
  // Memory access
  MOVE,     // register -> register
  MOVE1,
  LOAD,     // memory -> register
  LOAD1,
  STORE,    // register -> memory
  CMP,
  BNE,
  JMP,

  SHIFT,
  INTERP,
  // ... and many more
  // many opcodes will be much bigger chunks of stateful work like oscillators, table lookups,
  // env followers, and in general DSP machinery.
  NUM_OPCODES
};

static_assert(NUM_OPCODES < (1<<8));


using Instruction = uint8_t[8];

// THIS is newer than document! update that!

static inline Opcode getOpcode(Instruction t) { return t[0]; }
static inline uint8_t getDestIdx(Instruction t) { return t[1]; }
static inline uint8_t getSrc1Idx(Instruction t) { return t[2]; }
static inline uint8_t getBankSize(Instruction t) { return t[3]; }

// src 2 comes after bank size so we can have an inst. with dest, src1, bank size and immediate
// and have the immediate float32 aligned.

static inline uint8_t getSrc2Idx(Instruction t) { return t[4]; }
static inline uint8_t getSrc3Idx(Instruction t) { return t[5]; }
static inline uint8_t getStateArena(Instruction t) { return t[6]; }
static inline uint8_t getStateChunk(Instruction t) { return t[7]; }

static inline float32_t getFloatImmediate(Instruction t) {
  return *(reinterpret_cast<float32_t*>(&t[4]));
}

static inline uint16_t getAddressOffset(Instruction t) {
  return *(reinterpret_cast<uint16_t*>(&t[6]));
}


struct Program {
  std::vector< Instruction > instructions;
  MemoryRequirements memReqs;
};

struct FloatArena {
  std::vector< float > floatVec;
  size_t chunkSizeInFloats;
  
  void clear() { floatVec.clear(); chunkSizeInFloats = 0; }
};

struct MLVM {
  // uint8_t indices are baked into the design, so we just use std::array and
  // grab 2^8 registerrs and arenas.
  std::array< SignalBlock, 256 > registers;
  std::array< FloatArena, 256 > arenas;
  Program program;
  uint32_t programCounter;
  
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


  void load(const Program& newCode);
  
  // process in a given context - the context contains audio i/o, event i/o, and time/beats info.
  void process(AudioContext* context);
  
private:
  // return a float ptr into the arena, which might contain parameters
  // or SignalBlocks.
  float* getArenaPtr(uint8_t arena, uint8_t chunk, uint8_t bankSize);


};

} // namespace ml

