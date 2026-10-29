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
// The single source of truth for opcodes. Each X(name, format) line yields the
// enum constant, the assembler mnemonic and the opcode's operand format. To add
// an opcode, add one line here.
//
// The format names the opcode's row in the instruction table of the architecture
// doc. formatSpec() below says which field each operand of that row occupies. The
// assembler places operands from it, and the VM's switch reads the same fields;
// the format tests assemble one instruction per opcode to keep the two honest.

enum class OpFormat : uint8_t {
  kNoOperands,      // (none)                               NOOP, HALT
  kDestSrc,         // dest, src1                           MOVE, NEG, SIN, SPREAD, MIX, BANK, UNBANK
  kDestSrcSrc,      // dest, src1, src2                     ADD, MUL ...
  kDestSrc3,        // dest, src1, src2, src3               LERP, CLAMP, SELECT
  kGen,             // dest, src1 (pitch), state            SAW, SINE, IMPULSE, NOISE
  kFilter2,         // dest, src1, src2, state              ONEPOLE
  kFilter3,         // dest, src1, src2, src3, state        SVF, FOUR_POLE
  kParamsState,     // dest, params, state                  LOAD_K, LOAD_G
  kSrcParamsState,  // dest, src1, params, state            LOAD_KS
  kImmediate,       // dest, float immediate                LOAD_I
  kArenaAccess,     // register (field 1), arena (4,5)      LOAD, STORE
  kElement,         // dest, src1, element number           FILL
  kCompare,         // src1 (field 2), src2 (field 4)       CMP
  kBranch,          // label -> pc offset (6,7)             BNE, JMP
};

#define MLVM_OPCODES(X) \
  /* system */            X(NOOP, kNoOperands) X(HALT, kNoOperands) \
  /* memory access */     X(MOVE, kDestSrc) X(LOAD_I, kImmediate) X(LOAD, kArenaAccess) X(STORE, kArenaAccess) \
  /* control rate */      X(LOAD_K, kParamsState) X(LOAD_KS, kSrcParamsState) X(LOAD_G, kParamsState) \
  /* compare and branch */X(CMP, kCompare) X(BNE, kBranch) X(JMP, kBranch) \
  /* one-input math */    X(SIN, kDestSrc) X(LOG, kDestSrc) X(EXP, kDestSrc) X(NEG, kDestSrc) X(ABS, kDestSrc) X(RCP, kDestSrc) \
  /* two-input math */    X(ADD, kDestSrcSrc) X(ADD_WRAP, kDestSrcSrc) X(SUB, kDestSrcSrc) X(SUB_WRAP, kDestSrcSrc) X(MUL, kDestSrcSrc) X(DIV, kDestSrcSrc) \
  /* three-input math */  X(LERP, kDestSrc3) X(CLAMP, kDestSrc3) X(SELECT, kDestSrc3) \
  /* generators */        X(SINE, kGen) X(IMPULSE, kGen) X(SAW, kGen) X(NOISE, kGen) \
  /* filters */           X(SVF, kFilter3) X(ONEPOLE, kFilter2) X(FOUR_POLE, kFilter3) \
  /* layout */            X(FILL, kElement) X(SPREAD, kDestSrc) X(MIX, kDestSrc) X(BANK, kDestSrc) X(UNBANK, kDestSrc) \
  /* misc */              X(SHIFT, kGen) X(INTERP, kDestSrcSrc)
  // ... and many more
  // many opcodes will be much bigger chunks of stateful work like oscillators, table lookups,
  // env followers, and in general DSP machinery.

enum Opcode : uint8_t {
#define X(name, format) name,
  MLVM_OPCODES(X)
#undef X
  NUM_OPCODES
};

// an opcode must fit into one byte
static_assert(NUM_OPCODES < (1 << 8));

inline constexpr std::string_view kOpcodeNames[] = {
#define X(name, format) #name,
  MLVM_OPCODES(X)
#undef X
};

inline constexpr OpFormat kOpcodeFormats[] = {
#define X(name, format) OpFormat::format,
  MLVM_OPCODES(X)
#undef X
};

static_assert(sizeof(kOpcodeNames) / sizeof(kOpcodeNames[0]) == NUM_OPCODES);
static_assert(sizeof(kOpcodeFormats) / sizeof(kOpcodeFormats[0]) == NUM_OPCODES);

constexpr std::string_view opcodeName(Opcode op) {
  return op < NUM_OPCODES ? kOpcodeNames[op] : std::string_view("?");
}

constexpr OpFormat opcodeFormat(Opcode op) {
  return op < NUM_OPCODES ? kOpcodeFormats[op] : OpFormat::kNoOperands;
}

// ----------------------------------------------------------------
// Operand placement per format: the instruction table as data.
//
// An operand slot names the kind of operand and the first field it occupies: a
// register or element number takes one field, an arena reference two (arena,
// chunk), a float immediate fields 4-7, a label fields 6-7 as a pc offset.

enum class OperandKind : uint8_t { kReg, kArena, kFloat, kByte, kLabel };

struct OperandSlot {
  OperandKind kind;
  uint8_t field;
};

struct FormatSpec {
  OperandSlot slots[5];
  uint8_t count;
  bool hasBank;   // field 3 is a bank size for this row
};

constexpr FormatSpec formatSpec(OpFormat f) {
  using K = OperandKind;
  switch (f) {
    case OpFormat::kNoOperands:     return {{}, 0, false};
    case OpFormat::kDestSrc:        return {{{K::kReg, 1}, {K::kReg, 2}}, 2, true};
    case OpFormat::kDestSrcSrc:     return {{{K::kReg, 1}, {K::kReg, 2}, {K::kReg, 4}}, 3, true};
    case OpFormat::kDestSrc3:       return {{{K::kReg, 1}, {K::kReg, 2}, {K::kReg, 4}, {K::kReg, 5}}, 4, true};
    case OpFormat::kGen:            return {{{K::kReg, 1}, {K::kReg, 2}, {K::kArena, 6}}, 3, true};
    case OpFormat::kFilter2:        return {{{K::kReg, 1}, {K::kReg, 2}, {K::kReg, 4}, {K::kArena, 6}}, 4, true};
    case OpFormat::kFilter3:        return {{{K::kReg, 1}, {K::kReg, 2}, {K::kReg, 4}, {K::kReg, 5}, {K::kArena, 6}}, 5, true};
    case OpFormat::kParamsState:    return {{{K::kReg, 1}, {K::kArena, 4}, {K::kArena, 6}}, 3, true};
    case OpFormat::kSrcParamsState: return {{{K::kReg, 1}, {K::kReg, 2}, {K::kArena, 4}, {K::kArena, 6}}, 4, true};
    case OpFormat::kImmediate:      return {{{K::kReg, 1}, {K::kFloat, 4}}, 2, true};
    case OpFormat::kArenaAccess:    return {{{K::kReg, 1}, {K::kArena, 4}}, 2, false};
    case OpFormat::kElement:        return {{{K::kReg, 1}, {K::kReg, 2}, {K::kByte, 5}}, 3, true};
    case OpFormat::kCompare:        return {{{K::kReg, 2}, {K::kReg, 4}}, 2, false};
    case OpFormat::kBranch:         return {{{K::kLabel, 6}}, 1, false};
  }
  return {{}, 0, false};
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

  // Banks. The bank size field is a voice count, 0 or a multiple of 4. Bank 0 is one
  // horizontal SignalBlock: a time series, one float per frame. A bank of N voices at
  // register r occupies registers r..r+N-1 as N/4 madronalib SignalBlock4s in the
  // "vertical" layout: one float4 per frame holding four voices, so voice v of the
  // bank is lane v%4 of group v/4:
  //
  //   register r      (group 0)   v0[0] v1[0] v2[0] v3[0]  v0[1] v1[1] v2[1] v3[1] ...
  //   register r+4    (group 1)   v4[0] v5[0] v6[0] v7[0]  ...
  //
  // Elementwise operators are layout-blind: with a bank size they apply to N
  // registers in turn, which is correct for both layouts. Time-dependent operators
  // (ramps, gates, filters, generators) and layout operators (SPREAD, MIX, BANK,
  // UNBANK) know the arrangement. Control-rate operators with a bank size write banks
  // directly, and read a one-float row (a controller) broadcast to every lane.
  static constexpr size_t kLanes{4};
  static constexpr size_t kMaxBankVoices{kEventVoices};   // a control row has 16 voices

  // Generators and filters. Pitch-like inputs are log pitch in octaves, 0 = A440,
  // madronalib's convention; the operators convert to omega (cycles per sample)
  // internally, so cutoff modulation is plain octave arithmetic. Their per-voice
  // state lives in a chunk the program declares, laid out variable-major: variable
  // k of voice v is at chunk[k * N + v] for a bank of N (N = 1 for bank 0), so a
  // group's four lanes are contiguous.
  //   SAW.N     dest, pitch, state/chunk             polyBLEP sawtooth. state: phase (N floats)
  //   SVF.N     dest, in, cutoffPitch, k, state/chunk lowpass SVF, k = 1/Q, all audio rate.
  //                                                  state: ic1eq[N] ic2eq[N] (2N floats)
  //   ONEPOLE.N dest, in, time, state/chunk          one-pole lowpass, time in seconds read
  //                                                  at frame 0 of each block. state: y (N)
  // The sample rate is needed for all of these; processBlock() takes it from the
  // AudioContext every block, and tests set it directly.
  double sampleRate{48000.0};

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
