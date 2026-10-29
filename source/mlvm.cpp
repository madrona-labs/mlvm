// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com

#include "mlvm.h"

#include <algorithm>
#include <cstring>

#include "MLAudioContext.h"
#include "MLDSPFilters.h"
#include "MLDSPGens.h"

// a using-directive is fine here in the .cpp: it only affects this translation unit.
using namespace ml;

namespace mlvm {

// the external arenas mirror madronalib's layouts exactly.
static_assert(MLVM::kEventRows == ml::kNumControlRows, "event arena rows must match VoiceControls");
static_assert(MLVM::kEventVoices == ml::kNumControlVoices, "event arena voices must match ControlRow");
static_assert(MLVM::kEventChunkFloats * sizeof(float) == sizeof(ml::ControlRow), "event chunk must be one ControlRow");
static_assert(sizeof(ml::VoiceControls) == MLVM::kEventRows * MLVM::kEventChunkFloats * sizeof(float),
              "one channel of VoiceControls must fill kEventRows consecutive chunks");
static_assert(MLVM::kNumControllers == ml::kNumControllerValues, "controller arena must match ControllerValues");
static_assert(sizeof(ml::ControllerValues) == MLVM::kNumControllers * sizeof(float), "ControllerValues must be packed");
static_assert(sizeof(ml::SignalBlock) == kFramesPerBlock * sizeof(float),
              "audio arenas assume SignalBlocks are packed in the context's buffers");

// a bank of 4 voices is one SignalBlock4, which spans exactly 4 registers.
static_assert(sizeof(ml::SignalBlock4) == MLVM::kLanes * sizeof(ml::SignalBlock), "a SignalBlock4 must span 4 registers");
static_assert(kFramesPerBlock % MLVM::kLanes == 0, "frames per block must be a multiple of 4");

namespace {

constexpr size_t kFrames = kFramesPerBlock;
constexpr size_t kLanes = MLVM::kLanes;

// parse a decimal channel suffix: "events3" -> 3. Returns false if malformed.
bool parseChannelSuffix(std::string_view s, size_t prefixLen, uint8_t& channel) {
  std::string_view digits = s.substr(prefixLen);
  if (digits.empty() || digits.size() > 3) return false;
  unsigned v = 0;
  for (char c : digits) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<unsigned>(c - '0');
  }
  if (v > 255) return false;
  channel = static_cast<uint8_t>(v);
  return true;
}

// map an EXTERN manifest name to a host input, and the chunk size that input has.
bool resolveExternal(std::string_view name, MLVM::HostInput& kind, uint8_t& channel, size_t& chunkSize) {
  channel = 0;
  if (name == "audioin") { kind = MLVM::HostInput::audioIn; chunkSize = kFrames; return true; }
  if (name == "audioout") { kind = MLVM::HostInput::audioOut; chunkSize = kFrames; return true; }
  constexpr std::string_view kEvents = "events";
  constexpr std::string_view kCtl = "ctl";
  if (name.substr(0, kEvents.size()) == kEvents && parseChannelSuffix(name, kEvents.size(), channel)) {
    kind = MLVM::HostInput::events; chunkSize = MLVM::kEventChunkFloats; return true;
  }
  if (name.substr(0, kCtl.size()) == kCtl && parseChannelSuffix(name, kCtl.size(), channel)) {
    kind = MLVM::HostInput::controllers; chunkSize = 1; return true;
  }
  return false;
}

// ---- ramps: over kControlRampFrames frames then hold, block-size independent.

constexpr size_t kRamp = std::min(MLVM::kControlRampFrames, kFrames);

inline SignalBlock controlRamp(float from, float to) {
  SignalBlock out(to);
  const float interval = (to - from) / static_cast<float>(kRamp);
  for (size_t i = 0; i < kRamp; ++i) out[i] = from + interval * static_cast<float>(i + 1);
  return out;
}

// ramp one lane of a vertical group: lane l of the float4 at each frame.
inline void controlRampLane(float* group, size_t lane, float from, float to) {
  const float interval = (to - from) / static_cast<float>(kRamp);
  for (size_t i = 0; i < kRamp; ++i) group[i * kLanes + lane] = from + interval * static_cast<float>(i + 1);
  for (size_t i = kRamp; i < kFrames; ++i) group[i * kLanes + lane] = to;
}

// ramp a whole vertical group with float4 math: from4 and to4 hold one voice per lane.
inline void controlRampGroup(float* group, float4 from4, float4 to4) {
  const float4 interval = (to4 - from4) * float4(1.f / static_cast<float>(kRamp));
  for (size_t i = 0; i < kRamp; ++i) storeFloat4(group + i * kLanes, from4 + interval * float4(static_cast<float>(i + 1)));
  for (size_t i = kRamp; i < kFrames; ++i) storeFloat4(group + i * kLanes, to4);
}

// arena memory is only 4-byte aligned, so gather float4s by value.
inline float4 gather4(const float* p) { return float4(p[0], p[1], p[2], p[3]); }
inline void scatter4(float* p, float4 v) {
  alignas(16) float tmp[4];
  storeFloat4(tmp, v);
  p[0] = tmp[0]; p[1] = tmp[1]; p[2] = tmp[2]; p[3] = tmp[3];
}

// gate detection on one lane of a vertical gate bank: frame of the first rise, or -1.
inline int findRiseLane(const float* group, size_t lane, bool prevLevel) {
  bool prev = prevLevel;
  for (size_t i = 0; i < kFrames; ++i) {
    const bool high = group[i * kLanes + lane] > 0.5f;
    if (high && !prev) return static_cast<int>(i);
    prev = high;
  }
  return -1;
}

inline int findRise(const SignalBlock& gate, bool prevLevel) {
  bool prev = prevLevel;
  for (size_t i = 0; i < kFrames; ++i) {
    const bool high = gate[i] > 0.5f;
    if (high && !prev) return static_cast<int>(i);
    prev = high;
  }
  return -1;
}

// ---- generators and filters

// log pitch in octaves (0 = A440) -> omega in cycles per sample, clamped to the range
// the filters accept. scale = 440 / sampleRate.
inline float4 pitchToOmega4(float4 pitch, float scale) {
  return clamp(expApprox(kLogTwoVec * pitch) * float4(scale), float4(kOmegaMin), float4(kOmegaMax));
}

// the same for generators, whose floor is 0: an LFO at 0.1 Hz is a valid oscillator
// but far below the filters' minimum.
inline float4 pitchToOmegaGen4(float4 pitch, float scale) {
  return clamp(expApprox(kLogTwoVec * pitch) * float4(scale), float4(0.f), float4(kOmegaMax));
}

enum class Wave { saw, sine };

inline float4 shapeWave(Wave w, float4 phase, float4 omega) {
  return (w == Wave::saw) ? phasorToSawSample(phase, omega) : phasorToSineSample(phase);
}

// one-pole coefficient for a time constant in seconds; time <= 0 means instant.
inline float onePoleCoeff(float timeInSeconds, double sampleRate) {
  if (timeInSeconds <= 0.f) return 1.f;
  return 1.f - std::exp(-1.f / (timeInSeconds * static_cast<float>(sampleRate)));
}

// Bank 0 (one horizontal voice) runs the same float4 code as a bank group with the
// voice's values broadcast to all lanes and lane 0 kept. That keeps one implementation
// per operator and makes bank 0 bit-identical to lane 0 of a bank.
inline float lane0(float4 v) {
  alignas(16) float tmp[4];
  storeFloat4(tmp, v);
  return tmp[0];
}

} // anonymous namespace

// allocate arena memory as described in the manifest, resolve external arenas, and
// store the program.
bool MLVM::loadProgram(const Program& newProgram) {
  if (newProgram.manifest.size() > kNumArenas) return false;

  // resolve external names first so a bad program leaves the VM untouched.
  std::vector<ExternalBinding> newExternals;
  for (size_t i = 0; i < newProgram.manifest.size(); ++i) {
    const auto& desc = newProgram.manifest[i];
    if (!desc.external) continue;
    HostInput kind;
    uint8_t channel;
    size_t chunkSize;
    if (!resolveExternal(desc.name, kind, channel, chunkSize)) return false;
    if (desc.chunkSizeInFloats != chunkSize) return false;
    newExternals.push_back({static_cast<uint8_t>(i), kind, channel});
  }

  // constant tables must target owned arenas and fit their chunks.
  for (const auto& d : newProgram.data) {
    if (d.arena >= newProgram.manifest.size()) return false;
    const auto& desc = newProgram.manifest[d.arena];
    if (desc.external || d.values.size() > desc.chunkSizeInFloats) return false;
  }

  for (auto& a : arenas) a.clear();
  for (size_t i = 0; i < newProgram.manifest.size(); ++i) {
    const auto& desc = newProgram.manifest[i];
    if (desc.external) {
      arenas[i].declareExternal(desc.chunkSizeInFloats);
    } else {
      arenas[i].allocate(desc.chunkSizeInFloats, kChunksPerArena);
    }
  }
  for (const auto& d : newProgram.data) {
    FloatArena& a = arenas[d.arena];
    std::copy(d.values.begin(), d.values.end(), a.data() + d.chunk * a.chunkSizeInFloats);
  }

  program = newProgram;
  externals = std::move(newExternals);
  programCounter = 0;
  flag = false;
  return true;
}

bool MLVM::bindArena(uint8_t arenaIdx, float* data, size_t nFloats, bool writable) {
  FloatArena& a = arenas[arenaIdx];
  if (!a.external) return false;
  a.bind(data, nFloats, writable);
  return true;
}

void MLVM::unbindArenas() {
  for (const auto& b : externals) arenas[b.arena].unbind();
}

// A float is the smallest thing in an arena, so arena chunk size 1 means 4 bytes.
// Arena instructions have a bank size operand: a bank of N copies occupies N chunks,
// so the chunk stride is multiplied by the bank size (bank size 0 means one copy).
// Returns nullptr if floatsNeeded floats from the addressed chunk are not fully inside
// the arena, so a bad program cannot read or write outside VM memory. An unbound
// external arena has size 0 and so always returns nullptr.
float* MLVM::getArenaPtr(uint8_t arenaIdx, uint8_t chunk, uint8_t bankSize, size_t floatsNeeded) {
  FloatArena& a = arenas[arenaIdx];
  float* base = a.data();
  if (!base) return nullptr;
  size_t copies = bankSize ? bankSize : 1;
  size_t offset = chunk * a.chunkSizeInFloats * copies;
  if (offset + floatsNeeded > a.size()) return nullptr;
  return base + offset;
}

// Here is the innermost loop that interprets the bytecode program.
// NOTE: Aside from the main switch, there should be few if any branches.
void MLVM::run() {
  const auto& code = program.instructions;
  const size_t codeSize = code.size();

  // registers r..r+n-1 all exist
  auto inRange = [](size_t r, size_t n) { return r + n <= kNumRegisters; };
  // float pointer to the start of a register; a bank's float4 frames follow contiguously.
  auto regPtr = [this](size_t r) { return registers[r].data(); };

  programCounter = 0;
  while (programCounter < codeSize) {
    const Instruction& inst = code[programCounter++];

    // decode operands. The ISA has the property that if an operand exists, it
    // is in the same location for all operations that use it. Also there are
    // not a ton of operands. So we decode them all here before the opcode switch.
    const Opcode opcode = getOpcode(inst);
    const uint8_t b1 = inst[1]; // dest idx
    const uint8_t b2 = inst[2]; // src1 idx
    const uint8_t b3 = inst[3]; // bank size: 0 = horizontal, else voices (multiple of 4)
    const uint8_t b4 = inst[4]; // src2 idx, params arena
    const uint8_t b5 = inst[5]; // src3 idx, params chunk, element #
    const uint8_t b6 = inst[6]; // state arena, projection type, pc offset 0:7
    const uint8_t b7 = inst[7]; // state chunk, pc offset 8:15

    // number of registers an elementwise op touches, and of voices a bank op handles.
    const size_t n = b3 ? b3 : 1;
    const bool banked = (b3 != 0) && (b3 % kLanes == 0);
    const size_t groups = banked ? b3 / kLanes : 0;

    // TODO: by grouping the memory opcodes together and so on, we can do quick bit tests
    // to decide if params and state pointers, values, immediates and so on need
    // to be decoded. For now, decode lazily inside the cases that need them.

    switch (opcode) {
      // ---- system
      case NOOP:
        break;
      case HALT:
        return;

      // ---- memory access
      case MOVE:
        if (!inRange(b1, n) || !inRange(b2, n)) break;
        for (size_t k = 0; k < n; ++k) registers[b1 + k] = registers[b2 + k];
        break;
      case LOAD_I:
        if (!inRange(b1, n)) break;
        for (size_t k = 0; k < n; ++k) registers[b1 + k] = SignalBlock(getFloatImmediate(inst));
        break;
      case LOAD:
        if (const float* src = getArenaPtr(b4, b5, b3, kFrames)) {
          std::memcpy(registers[b1].data(), src, sizeof(SignalBlock));
        }
        break;
      case STORE:
        // the register operand is still field 1; the arena ref is the destination.
        // Read-only external arenas (everything but audio output) ignore stores.
        if (!arenas[b4].writable) break;
        if (float* dest = getArenaPtr(b4, b5, b3, kFrames)) {
          std::memcpy(dest, registers[b1].data(), sizeof(SignalBlock));
        }
        break;

      // ---- control rate
      // These read one control row from fields 4,5 and keep per-voice state in the
      // chunk at fields 6,7. Bank 0 handles voice 0 into one horizontal register. A
      // bank of N voices writes N/4 vertical groups from voices 0..N-1 of the row. A
      // row with one float per chunk (a controller) is broadcast to every voice.
      case LOAD_K: {
        const bool broadcast = arenas[b4].chunkSizeInFloats == 1;
        const float* cv = getArenaPtr(b4, b5, 0, broadcast ? 1 : n);
        float* state = getArenaPtr(b6, b7, 0, n);
        if (!cv || !state || !inRange(b1, n) || (banked && n > kMaxBankVoices)) break;
        if (!banked) {
          registers[b1] = controlRamp(state[0], cv[0]);
          state[0] = cv[0];
          break;
        }
        for (size_t g = 0; g < groups; ++g) {
          const float4 to4 = broadcast ? float4(cv[0]) : gather4(cv + g * kLanes);
          const float4 from4 = gather4(state + g * kLanes);
          controlRampGroup(regPtr(b1) + g * kLanes * kFrames, from4, to4);
          scatter4(state + g * kLanes, to4);
        }
        break;
      }
      case LOAD_KS: {
        // sample and hold synced to the gate signal in src1 (an audio-rate 0/1 block
        // or bank, typically from LOAD_G): hold the previous value until the gate
        // rises, then step to the control value. With no rise in the block, ramp like
        // LOAD_K so continuous values (pitch under glide or bend) stay smooth.
        // State chunk: [0, 16) previous values, [16, 32) previous gate levels.
        const bool broadcast = arenas[b4].chunkSizeInFloats == 1;
        const float* cv = getArenaPtr(b4, b5, 0, broadcast ? 1 : n);
        float* state = getArenaPtr(b6, b7, 0, kEventVoices + n);
        if (!cv || !state || !inRange(b1, n) || !inRange(b2, n) || (banked && n > kMaxBankVoices)) break;
        float* prevGate = state + kEventVoices;
        if (!banked) {
          const SignalBlock& gate = registers[b2];
          const int rise = findRise(gate, prevGate[0] > 0.5f);
          if (rise >= 0) {
            SignalBlock out(cv[0]);
            for (int i = 0; i < rise; ++i) out[i] = state[0];
            registers[b1] = out;
          } else {
            registers[b1] = controlRamp(state[0], cv[0]);
          }
          state[0] = cv[0];
          prevGate[0] = gate[kFrames - 1] > 0.5f ? 1.f : 0.f;
          break;
        }
        for (size_t g = 0; g < groups; ++g) {
          const float* gate = regPtr(b2) + g * kLanes * kFrames;
          float* out = regPtr(b1) + g * kLanes * kFrames;
          for (size_t l = 0; l < kLanes; ++l) {
            const size_t v = g * kLanes + l;
            const float value = broadcast ? cv[0] : cv[v];
            const int rise = findRiseLane(gate, l, prevGate[v] > 0.5f);
            if (rise >= 0) {
              for (size_t i = 0; i < kFrames; ++i) out[i * kLanes + l] = (static_cast<int>(i) < rise) ? state[v] : value;
            } else {
              controlRampLane(out, l, state[v], value);
            }
            state[v] = value;
            prevGate[v] = gate[(kFrames - 1) * kLanes + l] > 0.5f ? 1.f : 0.f;
          }
        }
        break;
      }
      case LOAD_G: {
        // expand gate transition codes (see MLDSPOps.h) to 0/1 blocks. State: gate level.
        const bool broadcast = arenas[b4].chunkSizeInFloats == 1;
        const float* codes = getArenaPtr(b4, b5, 0, broadcast ? 1 : n);
        float* level = getArenaPtr(b6, b7, 0, n);
        if (!codes || !level || !inRange(b1, n) || (banked && n > kMaxBankVoices)) break;
        if (!banked) {
          const bool after = expandGate(codes[0], level[0] > 0.5f, registers[b1]);
          level[0] = after ? 1.f : 0.f;
          break;
        }
        for (size_t g = 0; g < groups; ++g) {
          float* out = regPtr(b1) + g * kLanes * kFrames;
          for (size_t l = 0; l < kLanes; ++l) {
            const size_t v = g * kLanes + l;
            const float gateCode = broadcast ? codes[0] : codes[v];
            const bool before = level[v] > 0.5f;
            const bool after = gateLevelAfter(gateCode, before);
            const size_t frame = gateHasTransition(gateCode) ? std::min(gateFrame(gateCode), kFrames) : 0;
            const float lo = before ? 1.f : 0.f;
            const float hi = after ? 1.f : 0.f;
            for (size_t i = 0; i < kFrames; ++i) out[i * kLanes + l] = (i < frame) ? lo : hi;
            level[v] = hi;
          }
        }
        break;
      }

      // ---- compare and branch
      // TODO: semantics TBD (ALL / ANY / threshold, flag count). For now CMP sets
      // the flag if ANY frame of the first register is greater than the second.
      case CMP: {
        // src1 in field 2, src2 in field 4, as the table says: CMP has no dest.
        const SignalBlock& x = registers[b2];
        const SignalBlock& y = registers[b4];
        bool any = false;
        for (size_t i = 0; i < kFrames; ++i) any |= (x[i] > y[i]);
        flag = any;
        break;
      }
      case BNE:
        if (flag) programCounter += static_cast<uint32_t>(static_cast<int32_t>(getAddressOffset(inst)));
        break;
      case JMP:
        programCounter += static_cast<uint32_t>(static_cast<int32_t>(getAddressOffset(inst)));
        break;

      // ---- elementwise math: layout-blind, so a bank is just n registers in turn.
      case NEG:
        if (!inRange(b1, n) || !inRange(b2, n)) break;
        for (size_t k = 0; k < n; ++k) registers[b1 + k] = SignalBlock(0.f) - registers[b2 + k];
        break;
      case ABS:
        if (!inRange(b1, n) || !inRange(b2, n)) break;
        for (size_t k = 0; k < n; ++k) registers[b1 + k] = abs(registers[b2 + k]);
        break;
      case SIN:
        if (!inRange(b1, n) || !inRange(b2, n)) break;
        for (size_t k = 0; k < n; ++k) registers[b1 + k] = sin(registers[b2 + k]);
        break;
      case ADD:
        if (!inRange(b1, n) || !inRange(b2, n) || !inRange(b4, n)) break;
        for (size_t k = 0; k < n; ++k) registers[b1 + k] = add(registers[b2 + k], registers[b4 + k]);
        break;
      case SUB:
        if (!inRange(b1, n) || !inRange(b2, n) || !inRange(b4, n)) break;
        for (size_t k = 0; k < n; ++k) registers[b1 + k] = subtract(registers[b2 + k], registers[b4 + k]);
        break;
      case MUL:
        if (!inRange(b1, n) || !inRange(b2, n) || !inRange(b4, n)) break;
        for (size_t k = 0; k < n; ++k) registers[b1 + k] = multiply(registers[b2 + k], registers[b4 + k]);
        break;
      case DIV:
        if (!inRange(b1, n) || !inRange(b2, n) || !inRange(b4, n)) break;
        for (size_t k = 0; k < n; ++k) registers[b1 + k] = divide(registers[b2 + k], registers[b4 + k]);
        break;
      case LERP:
        if (!inRange(b1, n) || !inRange(b2, n) || !inRange(b4, n) || !inRange(b5, n)) break;
        for (size_t k = 0; k < n; ++k) registers[b1 + k] = lerp(registers[b2 + k], registers[b4 + k], registers[b5 + k]);
        break;

      // ---- layout
      case FILL:
        // broadcast element b5 of src1 to all elements of dest
        registers[b1] = SignalBlock(registers[b2][b5 % kFrames]);
        break;
      case SPREAD: {
        // horizontal src1 -> bank at dest: each frame's value to all lanes of every group.
        if (!banked || !inRange(b1, n)) break;
        const SignalBlock src = registers[b2];   // copy: src may lie inside the bank
        for (size_t g = 0; g < groups; ++g) {
          float* out = regPtr(b1) + g * kLanes * kFrames;
          for (size_t i = 0; i < kFrames; ++i) storeFloat4(out + i * kLanes, float4(src[i]));
        }
        break;
      }
      case MIX: {
        // bank at src1 -> horizontal dest: sum of all voices, frame by frame.
        if (!banked || !inRange(b2, n)) break;
        SignalBlock out(0.f);
        for (size_t g = 0; g < groups; ++g) {
          const float* in = regPtr(b2) + g * kLanes * kFrames;
          for (size_t i = 0; i < kFrames; ++i) {
            const float* f = in + i * kLanes;
            out[i] += f[0] + f[1] + f[2] + f[3];
          }
        }
        registers[b1] = out;
        break;
      }
      case BANK: {
        // n horizontal voices at src1 (one register each) -> vertical bank at dest.
        // dest and src must not overlap.
        if (!banked || !inRange(b1, n) || !inRange(b2, n)) break;
        for (size_t g = 0; g < groups; ++g) {
          float* out = regPtr(b1) + g * kLanes * kFrames;
          for (size_t l = 0; l < kLanes; ++l) {
            const SignalBlock& voice = registers[b2 + g * kLanes + l];
            for (size_t i = 0; i < kFrames; ++i) out[i * kLanes + l] = voice[i];
          }
        }
        break;
      }
      case UNBANK: {
        // vertical bank at src1 -> n horizontal voices at dest. dest and src must not overlap.
        if (!banked || !inRange(b1, n) || !inRange(b2, n)) break;
        for (size_t g = 0; g < groups; ++g) {
          const float* in = regPtr(b2) + g * kLanes * kFrames;
          for (size_t l = 0; l < kLanes; ++l) {
            SignalBlock& voice = registers[b1 + g * kLanes + l];
            for (size_t i = 0; i < kFrames; ++i) voice[i] = in[i * kLanes + l];
          }
        }
        break;
      }

      // ---- generators and filters
      // Per-voice state is variable-major in the chunk: variable k of voice v at
      // chunk[k * n + v]. The sample rate comes from processBlock() or the test.
      case SAW:
      case SINE: {
        // oscillator at the pitch in src1: polyBLEP sawtooth or sine. state: phase per voice.
        const Wave wave = (opcode == SAW) ? Wave::saw : Wave::sine;
        float* phase = getArenaPtr(b6, b7, 0, n);
        if (!phase || !inRange(b1, n) || !inRange(b2, n)) break;
        const float scale = 440.f / static_cast<float>(sampleRate);
        if (!banked) {
          const SignalBlock& pitch = registers[b2];
          SignalBlock out;
          float4 ph4(phase[0]);
          for (size_t i = 0; i < kFrames; ++i) {
            const float4 om = pitchToOmegaGen4(float4(pitch[i]), scale);
            ph4 = fracPart(ph4 + om);
            out[i] = lane0(shapeWave(wave, ph4, om));
          }
          phase[0] = lane0(ph4);
          registers[b1] = out;
          break;
        }
        for (size_t g = 0; g < groups; ++g) {
          const float* in = regPtr(b2) + g * kLanes * kFrames;
          float* out = regPtr(b1) + g * kLanes * kFrames;
          float4 ph4 = gather4(phase + g * kLanes);
          for (size_t i = 0; i < kFrames; ++i) {
            const float4 om = pitchToOmegaGen4(loadFloat4(in + i * kLanes), scale);
            ph4 = fracPart(ph4 + om);
            storeFloat4(out + i * kLanes, shapeWave(wave, ph4, om));
          }
          scatter4(phase + g * kLanes, ph4);
        }
        break;
      }
      case SVF: {
        // lowpass state variable filter: in = src1, cutoff pitch = src2, k = 1/Q = src3,
        // all audio rate. Coefficients are recomputed every frame as madronalib's
        // signal-rate path does (two sin() per frame per group). TODO: a reduced-rate
        // coefficient update is an easy optimization. state: ic1eq[n] ic2eq[n].
        float* state = getArenaPtr(b6, b7, 0, 2 * n);
        if (!state || !inRange(b1, n) || !inRange(b2, n) || !inRange(b4, n) || !inRange(b5, n)) break;
        const float scale = 440.f / static_cast<float>(sampleRate);
        if (!banked) {
          const SignalBlock& x = registers[b2];
          const SignalBlock& cutoff = registers[b4];
          const SignalBlock& kIn = registers[b5];
          SignalBlock out;
          Lopass<float4> f;
          f.state = {float4(state[0]), float4(state[1])};
          for (size_t i = 0; i < kFrames; ++i) {
            const auto c = Lopass<float4>::makeCoeffs({pitchToOmega4(float4(cutoff[i]), scale), float4(kIn[i])});
            out[i] = lane0(f.nextFrame(float4(x[i]), c));
          }
          state[0] = lane0(f.state[0]);
          state[1] = lane0(f.state[1]);
          registers[b1] = out;
          break;
        }
        for (size_t g = 0; g < groups; ++g) {
          const float* in = regPtr(b2) + g * kLanes * kFrames;
          const float* cutoff = regPtr(b4) + g * kLanes * kFrames;
          const float* kIn = regPtr(b5) + g * kLanes * kFrames;
          float* out = regPtr(b1) + g * kLanes * kFrames;
          Lopass<float4> f;
          f.state = {gather4(state + g * kLanes), gather4(state + n + g * kLanes)};
          for (size_t i = 0; i < kFrames; ++i) {
            const auto c = Lopass<float4>::makeCoeffs({pitchToOmega4(loadFloat4(cutoff + i * kLanes), scale),
                                                        loadFloat4(kIn + i * kLanes)});
            storeFloat4(out + i * kLanes, f.nextFrame(loadFloat4(in + i * kLanes), c));
          }
          scatter4(state + g * kLanes, f.state[0]);
          scatter4(state + n + g * kLanes, f.state[1]);
        }
        break;
      }
      case ONEPOLE: {
        // one-pole lowpass smoother: in = src1, time constant in seconds = src2, read at
        // frame 0 of the block per voice (the time is not modulated within a block).
        // Doubles as the glide for controllers and the attack/release for gates.
        // state: y per voice. In place (dest == src) is fine.
        float* y = getArenaPtr(b6, b7, 0, n);
        if (!y || !inRange(b1, n) || !inRange(b2, n) || !inRange(b4, n)) break;
        if (!banked) {
          const float kc = onePoleCoeff(registers[b4][0], sampleRate);
          const SignalBlock& x = registers[b2];
          SignalBlock out;
          float yv = y[0];
          for (size_t i = 0; i < kFrames; ++i) { yv += kc * (x[i] - yv); out[i] = yv; }
          y[0] = yv;
          registers[b1] = out;
          break;
        }
        for (size_t g = 0; g < groups; ++g) {
          const float* in = regPtr(b2) + g * kLanes * kFrames;
          const float* time = regPtr(b4) + g * kLanes * kFrames;
          float* out = regPtr(b1) + g * kLanes * kFrames;
          const float4 kc(onePoleCoeff(time[0], sampleRate), onePoleCoeff(time[1], sampleRate),
                          onePoleCoeff(time[2], sampleRate), onePoleCoeff(time[3], sampleRate));
          float4 y4 = gather4(y + g * kLanes);
          for (size_t i = 0; i < kFrames; ++i) {
            y4 = y4 + kc * (loadFloat4(in + i * kLanes) - y4);
            storeFloat4(out + i * kLanes, y4);
          }
          scatter4(y + g * kLanes, y4);
        }
        break;
      }

      default:
        // TODO: remaining opcodes are no-ops until implemented.
        break;
    }
  }
}

// Bind the program's external arenas to the context's memory, run the program, and
// unbind. Nothing is copied: a program pays only for the host data it reads. The
// context has finished event processing for this block before calling us, so the
// voice controls and controller values are stable while we run.
void MLVM::processBlock(AudioContext* context) {
  const double sr = context->getSampleRate();
  if (sr > 0.) sampleRate = sr;

  for (const auto& b : externals) {
    switch (b.kind) {
      case HostInput::audioIn: {
        const size_t nIn = context->inputs.size();
        bindArena(b.arena, nIn ? context->inputs[0].data() : nullptr, nIn * kFrames, false);
        break;
      }
      case HostInput::audioOut: {
        const size_t nOut = context->outputs.size();
        bindArena(b.arena, nOut ? context->outputs[0].data() : nullptr, nOut * kFrames, true);
        break;
      }
      case HostInput::events: {
        if (b.channel < context->getNumEventChannels()) {
          const VoiceControls& vc = context->getInputVoiceControls(b.channel);
          bindArena(b.arena, const_cast<float*>(vc.rows[0].data()), sizeof(VoiceControls) / sizeof(float), false);
        }
        break;
      }
      case HostInput::controllers: {
        if (b.channel < context->getNumEventChannels()) {
          const ControllerValues& cv = context->getInputControllerValues(b.channel);
          bindArena(b.arena, const_cast<float*>(cv.data()), cv.size(), false);
        }
        break;
      }
    }
  }

  // TODO: time info (tempo, beat phase, samples since start) as an external arena.

  run();
  unbindArenas();
}

} // namespace mlvm
