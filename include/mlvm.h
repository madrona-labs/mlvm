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
  /* control rate */      X(LOAD_K) X(LOAD_KS) X(LOAD_G) \
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
//
// An arena is either allocated by the VM (ARENA) or external (EXTERN): memory the
// host binds by name before each block, such as audio I/O, voice controls and
// controller values. External arenas cost nothing unless the program reads them.
struct ArenaDescription {
  std::string name;
  uint32_t chunkSizeInFloats{0};
  bool external{false};
};

struct Program {
  std::vector< ArenaDescription > manifest;
  std::vector< Instruction > instructions;
};

// ----------------------------------------------------------------
// VM

// Storage for one arena: either owned by the VM or a view of host memory.
struct FloatArena {
  std::vector< float > owned;
  float* view{nullptr};
  size_t viewSize{0};
  size_t chunkSizeInFloats{0};
  bool external{false};
  bool writable{true};

  float* data() { return external ? view : owned.data(); }
  const float* data() const { return external ? view : owned.data(); }
  size_t size() const { return external ? viewSize : owned.size(); }

  void allocate(size_t chunkSize, size_t nChunks) {
    clear();
    chunkSizeInFloats = chunkSize;
    owned.assign(chunkSize * nChunks, 0.f);
  }

  void declareExternal(size_t chunkSize) {
    clear();
    chunkSizeInFloats = chunkSize;
    external = true;
    writable = false;
  }

  // point an external arena at host memory for the duration of a block.
  void bind(float* p, size_t nFloats, bool isWritable) {
    view = p;
    viewSize = p ? nFloats : 0;
    writable = isWritable;
  }

  void unbind() { view = nullptr; viewSize = 0; }

  void clear() {
    owned.clear();
    view = nullptr;
    viewSize = 0;
    chunkSizeInFloats = 0;
    external = false;
    writable = true;
  }
};

struct MLVM {
  // uint8_t indices are baked into the design, so we have a number of
  // registers and of arenas fixed at compile time.
  static constexpr size_t kNumRegisters{256};
  static constexpr size_t kNumArenas{256};
  static constexpr size_t kChunksPerArena{256};

  // External arenas the host can provide, by manifest name. A program declares the
  // ones it uses with "EXTERN name chunksize" and processBlock() binds them from the
  // AudioContext every block. Reads from an unbound external arena are no-ops, and
  // only audio output is writable.
  //
  //   audioin    chunk = one SignalBlock (kFramesPerBlock floats), chunk index = channel
  //   audioout   same layout, writable
  //   events<c>  event channel c's ml::VoiceControls: chunk = one ml::ControlRow (16 floats,
  //              one per voice), chunk index = row per ml::ControlSignals
  //   ctl<c>     event channel c's ml::ControllerValues: chunk = one float, chunk index =
  //              controller number 0..127
  //
  // Control-rate operators read these rows and keep per-voice state in a chunk the
  // program declares:
  //   LOAD_K  dest, row, state          ramp from the previous value over kControlRampFrames
  //   LOAD_KS dest, gateReg, row, state sample and hold at the gate's rise, else ramp
  //   LOAD_G  dest, gateRow, state      expand gate transition codes to 0/1
  // State chunks hold one float per voice (LOAD_K, LOAD_G: 16 floats) or two
  // (LOAD_KS: 32 floats, previous values then previous gate levels).
  // See "Event and control inputs" in the architecture doc.
  static constexpr size_t kEventRows{10};         // == ml::kNumControlRows, checked in mlvm.cpp
  static constexpr size_t kEventVoices{16};       // == ml::kNumControlVoices
  static constexpr size_t kEventChunkFloats{kEventVoices};
  static constexpr size_t kNumControllers{128};   // == ml::kNumControllerValues

  // control-rate values ramp over this many frames regardless of the block size, so
  // a program sounds the same at any block size. Clamped to the block if it is shorter.
  static constexpr size_t kControlRampFrames{16};

  enum class HostInput : uint8_t { audioIn, audioOut, events, controllers };

  // an external arena resolved at load time, so processBlock() does no name lookups.
  struct ExternalBinding {
    uint8_t arena;
    HostInput kind;
    uint8_t channel;
  };

  std::array< ml::SignalBlock, kNumRegisters > registers;
  std::array< FloatArena, kNumArenas > arenas;
  std::vector< ExternalBinding > externals;
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

  // allocate arenas as described in the program manifest, resolve external arena
  // names, and store the instructions. Fails on an unknown external name or a
  // chunk size that does not match the host data.
  bool loadProgram(const Program& newCode);

  // point an external arena at host memory. Returns false if the arena is not external.
  bool bindArena(uint8_t arenaIdx, float* data, size_t nFloats, bool writable);
  void unbindArenas();

  // run the loaded program once on the current register file, until HALT.
  // Does not touch any AudioContext, so it can be driven directly by tests.
  void run();

  // process in a given context - the context contains audio i/o, event i/o, and time/beats info.
  // Binds the external arenas, calls run(), unbinds them.
  void processBlock(ml::AudioContext* context);

private:
  // return a float ptr into the arena at the given chunk, or nullptr if floatsNeeded
  // floats from there would fall outside the arena (or the arena is unbound). The
  // chunk stride is scaled by the bank size (0 means one copy).
  float* getArenaPtr(uint8_t arena, uint8_t chunk, uint8_t bankSize, size_t floatsNeeded);
};

} // namespace mlvm
