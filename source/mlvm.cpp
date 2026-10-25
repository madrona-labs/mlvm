// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com

#include "mlvm.h"

#include <algorithm>
#include <cstring>

#include "MLAudioContext.h"

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

namespace {

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
  if (name == "audioin") { kind = MLVM::HostInput::audioIn; chunkSize = kFramesPerBlock; return true; }
  if (name == "audioout") { kind = MLVM::HostInput::audioOut; chunkSize = kFramesPerBlock; return true; }
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

// ramp from `from` to `to` over kControlRampFrames frames, landing on `to` at the last
// ramp frame and holding it for the rest of the block. Block-size independent.
inline SignalBlock controlRamp(float from, float to) {
  constexpr size_t n = std::min(MLVM::kControlRampFrames, kFramesPerBlock);
  SignalBlock out(to);
  const float interval = (to - from) / static_cast<float>(n);
  for (size_t i = 0; i < n; ++i) out[i] = from + interval * static_cast<float>(i + 1);
  return out;
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

  for (auto& a : arenas) a.clear();
  for (size_t i = 0; i < newProgram.manifest.size(); ++i) {
    const auto& desc = newProgram.manifest[i];
    if (desc.external) {
      arenas[i].declareExternal(desc.chunkSizeInFloats);
    } else {
      arenas[i].allocate(desc.chunkSizeInFloats, kChunksPerArena);
    }
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

  programCounter = 0;
  while (programCounter < codeSize) {
    const Instruction& inst = code[programCounter++];

    // decode operands. The ISA has the property that if an operand exists, it
    // is in the same location for all operations that use it. Also there are
    // not a ton of operands. So we decode them all here before the opcode switch.
    const Opcode opcode = getOpcode(inst);
    const uint8_t b1 = inst[1]; // dest idx
    const uint8_t b2 = inst[2]; // src1 idx
    const uint8_t b3 = inst[3]; // bank size
    const uint8_t b4 = inst[4]; // src2 idx, params arena
    const uint8_t b5 = inst[5]; // src3 idx, params chunk, element #
    const uint8_t b6 = inst[6]; // state arena, projection type, pc offset 0:7
    const uint8_t b7 = inst[7]; // state chunk, pc offset 8:15

    // TODO: by grouping the memory opcodes together and so on, we can do quick bit tests
    // to decide if params and state pointers, values, immediates and so on need
    // to be decoded. For now, decode lazily inside the cases that need them.

    // TODO: bank sizes > 0 are decoded but inner implementations are scalar for now.

    switch (opcode) {
      // ---- system
      case NOOP:
        break;
      case HALT:
        return;

      // ---- memory access
      case MOVE:
        registers[b1] = registers[b2];
        break;
      case LOAD_I:
        registers[b1] = SignalBlock(getFloatImmediate(inst));
        break;
      case LOAD:
        if (const float* src = getArenaPtr(b4, b5, b3, kFramesPerBlock)) {
          std::memcpy(registers[b1].data(), src, sizeof(SignalBlock));
        }
        break;
      case STORE:
        // the register operand is still field 1; the arena ref is the destination.
        // Read-only external arenas (everything but audio output) ignore stores.
        if (!arenas[b4].writable) break;
        if (float* dest = getArenaPtr(b4, b5, b3, kFramesPerBlock)) {
          std::memcpy(dest, registers[b1].data(), sizeof(SignalBlock));
        }
        break;

      // ---- control rate
      // These read one ml::ControlRow chunk (one float per voice) or one controller
      // value from fields 4,5 and keep per-voice state in the chunk at fields 6,7.
      // Bank size 0 handles voice 0 into dest; bank size N handles voices 0..N-1 into
      // dest..dest+N-1, one voice per register, horizontal. One chunk holds all
      // voices, so no bank stride on the arena pointers.
      // TODO: replace the banked case with the vertical 4x4 layout once the bank
      // layout TODO in the architecture doc is resolved.
      case LOAD_K: {
        // ramp from the previous value to the control value over kControlRampFrames.
        const size_t nVoices = std::min<size_t>(b3 ? b3 : 1, kEventVoices);
        const float* cv = getArenaPtr(b4, b5, 0, nVoices);
        float* state = getArenaPtr(b6, b7, 0, nVoices);
        if (!cv || !state) break;
        for (size_t v = 0; v < nVoices && (b1 + v) < kNumRegisters; ++v) {
          registers[b1 + v] = controlRamp(state[v], cv[v]);
          state[v] = cv[v];
        }
        break;
      }
      case LOAD_KS: {
        // sample and hold synced to the gate signal in src1 (an audio-rate 0/1 block,
        // typically from LOAD_G): hold the previous value until the gate rises, then
        // step to the control value. With no rise in the block, ramp like LOAD_K so
        // continuous values (pitch under glide or bend) stay smooth.
        // State chunk: [0, 16) previous values, [16, 32) previous gate levels.
        const size_t nVoices = std::min<size_t>(b3 ? b3 : 1, kEventVoices);
        const float* cv = getArenaPtr(b4, b5, 0, nVoices);
        float* state = getArenaPtr(b6, b7, 0, kEventVoices + nVoices);
        if (!cv || !state) break;
        float* prevGate = state + kEventVoices;
        for (size_t v = 0; v < nVoices && (b1 + v) < kNumRegisters && (b2 + v) < kNumRegisters; ++v) {
          const SignalBlock& gate = registers[b2 + v];
          bool prev = prevGate[v] > 0.5f;
          int rise = -1;
          for (size_t i = 0; i < kFramesPerBlock; ++i) {
            const bool high = gate[i] > 0.5f;
            if (high && !prev) { rise = static_cast<int>(i); break; }
            prev = high;
          }
          if (rise >= 0) {
            SignalBlock out(cv[v]);
            for (int i = 0; i < rise; ++i) out[i] = state[v];
            registers[b1 + v] = out;
          } else {
            registers[b1 + v] = controlRamp(state[v], cv[v]);
          }
          state[v] = cv[v];
          prevGate[v] = gate[kFramesPerBlock - 1] > 0.5f ? 1.f : 0.f;
        }
        break;
      }
      case LOAD_G: {
        // expand gate transition codes (see MLDSPOps.h) to 0/1 blocks. State: gate level.
        const size_t nVoices = std::min<size_t>(b3 ? b3 : 1, kEventVoices);
        const float* codes = getArenaPtr(b4, b5, 0, nVoices);
        float* level = getArenaPtr(b6, b7, 0, nVoices);
        if (!codes || !level) break;
        for (size_t v = 0; v < nVoices && (b1 + v) < kNumRegisters; ++v) {
          const bool after = expandGate(codes[v], level[v] > 0.5f, registers[b1 + v]);
          level[v] = after ? 1.f : 0.f;
        }
        break;
      }

      // ---- compare and branch
      // TODO: semantics TBD (ALL / ANY / threshold, flag count). For now CMP sets
      // the flag if ANY frame of the first register is greater than the second.
      case CMP: {
        const SignalBlock& x = registers[b1];
        const SignalBlock& y = registers[b2];
        bool any = false;
        for (size_t i = 0; i < kFramesPerBlock; ++i) any |= (x[i] > y[i]);
        flag = any;
        break;
      }
      case BNE:
        if (flag) programCounter += static_cast<uint32_t>(static_cast<int32_t>(getAddressOffset(inst)));
        break;
      case JMP:
        programCounter += static_cast<uint32_t>(static_cast<int32_t>(getAddressOffset(inst)));
        break;

      // ---- one-input math
      case NEG:
        registers[b1] = SignalBlock(0.f) - registers[b2];
        break;
      case ABS:
        registers[b1] = abs(registers[b2]);
        break;
      case SIN:
        registers[b1] = sin(registers[b2]);
        break;

      // ---- two-input math
      case ADD:
        registers[b1] = add(registers[b2], registers[b4]);
        break;
      case SUB:
        registers[b1] = subtract(registers[b2], registers[b4]);
        break;
      case MUL:
        registers[b1] = multiply(registers[b2], registers[b4]);
        break;
      case DIV:
        registers[b1] = divide(registers[b2], registers[b4]);
        break;

      // ---- three-input math
      case LERP:
        registers[b1] = lerp(registers[b2], registers[b4], registers[b5]);
        break;

      // ---- layout
      case FILL:
        // broadcast element b5 of src1 to all elements of dest
        registers[b1] = SignalBlock(registers[b2][b5 % kFramesPerBlock]);
        break;

      // ---- generators and filters
      // TODO: stateful operators need params (fields 4,5) and state (fields 6,7)
      // in the arena. Until the state layouts are defined these pass src1 through.
      case SAW:
      case SVF:
        (void)b6; (void)b7;
        registers[b1] = registers[b2];
        break;

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
  for (const auto& b : externals) {
    switch (b.kind) {
      case HostInput::audioIn: {
        const size_t n = context->inputs.size();
        bindArena(b.arena, n ? context->inputs[0].data() : nullptr, n * kFramesPerBlock, false);
        break;
      }
      case HostInput::audioOut: {
        const size_t n = context->outputs.size();
        bindArena(b.arena, n ? context->outputs[0].data() : nullptr, n * kFramesPerBlock, true);
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
