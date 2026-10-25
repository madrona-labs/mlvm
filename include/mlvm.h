// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "MLDSPOps.h"

// forward declare AudioContext
namespace ml { class AudioContext; }

namespace mlvm {

// ----------------------------------------------------------------
// Opcodes
//
// The single source of truth for opcodes. Each X(name) line yields the enum
// constant and the assembler mnemonic. To add an opcode, add one line here.
// Additional per-opcode columns (operand format, etc.) can be added later.

#define MLVM_OPCODES(X) \
  /* system */            X(NOOP) X(HALT) \
  /* memory access */     X(MOVE) X(LOAD_I) X(LOAD) X(STORE) \
  /* compare and branch */X(CMP) X(BNE) X(JMP) \
  /* one-input math */    X(SIN) X(LOG) X(EXP) X(NEG) X(ABS) X(RCP) \
  /* two-input math */    X(ADD) X(ADD_WRAP) X(SUB) X(SUB_WRAP) X(MUL) X(DIV) \
  /* three-input math */  X(LERP) X(CLAMP) X(SELECT) \
  /* generators */        X(SINE) X(IMPULSE) X(SAW) X(NOISE) \
  /* filters */           X(SVF) X(FOUR_POLE) \
  /* layout */            X(FILL) X(TRANSPOSE) \
  /* misc */              X(SHIFT) X(INTERP)
  // ... and many more
  // many opcodes will be much bigger chunks of stateful work like oscillators, table lookups,
  // env followers, and in general DSP machinery.

enum Opcode : uint8_t {
#define X(name) name,
  MLVM_OPCODES(X)
#undef X
  NUM_OPCODES
};

// an opcode must fit into one byte
static_assert(NUM_OPCODES < (1 << 8));

inline constexpr std::string_view kOpcodeNames[] = {
#define X(name) #name,
  MLVM_OPCODES(X)
#undef X
};

static_assert(sizeof(kOpcodeNames) / sizeof(kOpcodeNames[0]) == NUM_OPCODES);

constexpr std::string_view opcodeName(Opcode op) {
  return op < NUM_OPCODES ? kOpcodeNames[op] : std::string_view("?");
}

// linear scan: this is only used at assemble time, never on the audio thread.
constexpr std::optional<Opcode> opcodeFromName(std::string_view name) {
  for (size_t i = 0; i < NUM_OPCODES; ++i) {
    if (kOpcodeNames[i] == name) return static_cast<Opcode>(i);
  }
  return std::nullopt;
}

// ----------------------------------------------------------------
// Instructions
//
// An instruction is 8 bytes: a one-byte opcode followed by seven one-byte fields.
// If a field is present it is always in the same position, for all opcodes.

using Instruction = std::array<uint8_t, 8>;

static inline Opcode getOpcode(const Instruction& t) { return static_cast<Opcode>(t[0]); }
static inline uint8_t getDestIdx(const Instruction& t) { return t[1]; }
static inline uint8_t getSrc1Idx(const Instruction& t) { return t[2]; }
static inline uint8_t getBankSize(const Instruction& t) { return t[3]; }

// src 2 comes after bank size so we can have an inst. with dest, src1, bank size and immediate
// and have the immediate float32 aligned.

static inline uint8_t getSrc2Idx(const Instruction& t) { return t[4]; }
static inline uint8_t getSrc3Idx(const Instruction& t) { return t[5]; }
static inline uint8_t getParamsArena(const Instruction& t) { return t[4]; }
static inline uint8_t getParamsChunk(const Instruction& t) { return t[5]; }
static inline uint8_t getStateArena(const Instruction& t) { return t[6]; }
static inline uint8_t getStateChunk(const Instruction& t) { return t[7]; }

// a 32-bit float immediate lives in fields 4-7.
static inline float getFloatImmediate(const Instruction& t) {
  float f;
  std::memcpy(&f, &t[4], sizeof(f));
  return f;
}

static inline void setFloatImmediate(Instruction& t, float f) {
  std::memcpy(&t[4], &f, sizeof(f));
}

// a signed 16-bit program counter offset lives in fields 6-7, little-endian.
// The offset is relative to the instruction following the branch.
static inline int16_t getAddressOffset(const Instruction& t) {
  return static_cast<int16_t>(t[6] | (t[7] << 8));
}

static inline void setAddressOffset(Instruction& t, int16_t offset) {
  uint16_t u = static_cast<uint16_t>(offset);
  t[6] = u & 0xff;
  t[7] = (u >> 8) & 0xff;
}

// ----------------------------------------------------------------
// Program

// An arena is a flat float buffer addressed by an 8-bit chunk index.
// Each arena has its own chunk size, declared in the manifest.
struct ArenaDescription {
  std::string name;
  uint32_t chunkSizeInFloats{0};
};

struct Program {
  std::vector< ArenaDescription > manifest;
  std::vector< Instruction > instructions;
};

// ----------------------------------------------------------------
// VM

struct FloatArena {
  std::vector< float > floatVec;
  size_t chunkSizeInFloats{0};

  void clear() { floatVec.clear(); chunkSizeInFloats = 0; }
};

struct MLVM {
  // uint8_t indices are baked into the design, so we have a number of
  // registers and of arenas fixed at compile time.
  static constexpr size_t kNumRegisters{256};
  static constexpr size_t kNumArenas{256};
  static constexpr size_t kChunksPerArena{256};

  // we set a maximum number of inputs and outputs so programs
  // will know where to find inputs in the register file.
  static constexpr size_t kMaxInputs{8};
  static constexpr size_t kMaxVoices{8};
  static constexpr size_t kMaxOutputs{8};

  // processBlock() copies audio inputs into registers starting here, and
  // copies audio outputs out of registers starting here.
  static constexpr size_t kInputRegisterBase{0};
  static constexpr size_t kOutputRegisterBase{kMaxInputs};

  std::array< ml::SignalBlock, kNumRegisters > registers;
  std::array< FloatArena, kNumArenas > arenas;
  Program program;
  uint32_t programCounter{0};

  // flag set by CMP, read by BNE. Semantics TBD, see architecture doc.
  bool flag{false};

  // NOTES
  // A benefit from compiling the module graph into opcodes is that what looks like a
  // single module in the GUI can turn into different opcodes depending on modes, making
  // decisions at opcode-making ("compile") time.
  // Filter -> West Coast, East Coast (different personalities)
  // projection -> linear, quadratic, log, exp, ...
  // Distortion flavors, Compression flavors
  // body types, Space modes, ...
  // If modules share an interface and basic concept they can be rolled into one with a compile-time
  // switch for the "flavor."
  //
  // For crossfades on changes and super-quick undo, we can keep 2 versions of the program and state.

  // allocate arenas as described in the program manifest and store the instructions.
  bool loadProgram(const Program& newCode);

  // run the loaded program once on the current register file, until HALT.
  // Does not touch any AudioContext, so it can be driven directly by tests.
  void run();

  // process in a given context - the context contains audio i/o, event i/o, and time/beats info.
  // Copies inputs into registers, calls run(), copies output registers back out.
  void processBlock(ml::AudioContext* context);

private:
  // return a float ptr into the arena, which might contain parameters
  // or SignalBlocks.
  float* getArenaPtr(uint8_t arena, uint8_t chunk, uint8_t bankSize);
};

} // namespace mlvm
