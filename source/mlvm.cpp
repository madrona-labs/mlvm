// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com

#include "mlvm.h"

#include <algorithm>
#include <cstring>

#include "MLAudioContext.h"

// a using-directive is fine here in the .cpp: it only affects this translation unit.
using namespace ml;

namespace mlvm {

// the event arena mirrors madronalib's control-rate layout exactly.
static_assert(MLVM::kEventRows == ml::kNumControlRows, "event arena rows must match VoiceControls");
static_assert(MLVM::kEventChunkFloats * sizeof(float) == sizeof(ml::ControlRow), "event chunk must be one ControlRow");
static_assert(MLVM::kControlBlockFloats * sizeof(float) == sizeof(ml::ControlBlock), "ControlBlock must be one float4");
static_assert(sizeof(ml::VoiceControls) == MLVM::kEventRows * MLVM::kEventChunkFloats * sizeof(float),
              "one channel of VoiceControls must fill kEventRows consecutive chunks");

// allocate arena memory as described in the manifest and store the program.
bool MLVM::loadProgram(const Program& newProgram) {
  if (newProgram.manifest.size() > kNumArenas) return false;

  // the event arena, if declared, must have the ControlRow chunk size.
  int newEventArenaIdx = -1;
  for (size_t i = 0; i < newProgram.manifest.size(); ++i) {
    if (newProgram.manifest[i].name == kEventArenaName) {
      if (newProgram.manifest[i].chunkSizeInFloats != kEventChunkFloats) return false;
      newEventArenaIdx = static_cast<int>(i);
    }
  }

  for (auto& a : arenas) a.clear();
  for (size_t i = 0; i < newProgram.manifest.size(); ++i) {
    const auto& desc = newProgram.manifest[i];
    arenas[i].chunkSizeInFloats = desc.chunkSizeInFloats;
    arenas[i].floatVec.assign(kChunksPerArena * desc.chunkSizeInFloats, 0.f);
  }

  program = newProgram;
  eventArenaIdx = newEventArenaIdx;
  programCounter = 0;
  flag = false;
  return true;
}

// A float is the smallest thing in an arena, so arena chunk size 1 means 4 bytes.
// Arena instructions have a bank size operand: a bank of N copies occupies N chunks,
// so the chunk stride is multiplied by the bank size (bank size 0 means one copy).
// Returns nullptr if the addressed region is not fully inside the arena, so a bad
// program cannot read or write outside VM memory.
float* MLVM::getArenaPtr(uint8_t arenaIdx, uint8_t chunk, uint8_t bankSize) {
  FloatArena& a = arenas[arenaIdx];
  size_t copies = bankSize ? bankSize : 1;
  size_t regionFloats = a.chunkSizeInFloats * copies;
  size_t offset = chunk * regionFloats;
  // a register load or store always moves one whole SignalBlock.
  size_t needed = std::max(regionFloats, kFramesPerBlock);
  if (offset + needed > a.floatVec.size()) return nullptr;
  return a.floatVec.data() + offset;
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
      case LOAD_K: {
        // control-rate chunk (one ml::ControlRow: 16 ControlBlocks, one per voice) to
        // audio rate. A ramp moves from start to end, landing on end at the last frame;
        // a step holds start until its frame, then end. Bank size 0 converts voice 0
        // into dest. Bank size N converts voices 0..N-1 into dest..dest+N-1, one voice
        // per register, horizontal.
        // TODO: replace the banked case with the vertical 4x4 layout once the bank
        // layout TODO in the architecture doc is resolved.
        const float* chunk = getArenaPtr(b4, b5, 0);  // one chunk holds all voices: no bank stride
        if (!chunk) break;
        const size_t nVoices = std::min<size_t>(b3 ? b3 : 1, kEventChunkFloats / kControlBlockFloats);
        for (size_t v = 0; v < nVoices && (b1 + v) < kNumRegisters; ++v) {
          ControlBlock c;
          std::memcpy(&c, chunk + kControlBlockFloats * v, sizeof(c));
          registers[b1 + v] = interpolateBlockLinear(c);
        }
        break;
      }
      case LOAD:
        if (float* src = getArenaPtr(b4, b5, b3)) {
          std::memcpy(registers[b1].data(), src, sizeof(SignalBlock));
        }
        break;
      case STORE:
        // the register operand is still field 1; the arena ref is the destination.
        if (float* dest = getArenaPtr(b4, b5, b3)) {
          std::memcpy(dest, registers[b1].data(), sizeof(SignalBlock));
        }
        break;

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

// Copy audio inputs from the AudioContext into our input registers, run the
// program, and copy output registers back to the context. We implement decisions
// here about maximum numbers of inputs/outputs/event signals that are not
// limitations of the AudioContext.
void MLVM::processBlock(AudioContext* context) {
  const size_t nInputs = std::min<size_t>(context->inputs.size(), kMaxInputs);
  for (size_t i = 0; i < nInputs; ++i) {
    registers[kInputRegisterBase + i] = context->inputs[static_cast<int>(i)];
  }

  // copy control-rate event data for each channel into the event arena, if the
  // program declared one. One channel's VoiceControls fills kEventRows chunks.
  if (eventArenaIdx >= 0) {
    FloatArena& events = arenas[eventArenaIdx];
    const size_t floatsPerChannel = kEventRows * kEventChunkFloats;
    const size_t nChannels = std::min<size_t>(context->getNumEventChannels(), kMaxEventChannels);
    for (size_t ch = 0; ch < nChannels; ++ch) {
      if ((ch + 1) * floatsPerChannel > events.floatVec.size()) break;
      std::memcpy(events.floatVec.data() + ch * floatsPerChannel,
                  &context->getInputVoiceControls(static_cast<int>(ch)), sizeof(VoiceControls));
    }
  }

  // TODO: time info (tempo, beat phase, samples since start) as a control-rate chunk.

  run();

  const size_t nOutputs = std::min<size_t>(context->outputs.size(), kMaxOutputs);
  for (size_t i = 0; i < nOutputs; ++i) {
    context->outputs[static_cast<int>(i)] = registers[kOutputRegisterBase + i];
  }
}

} // namespace mlvm
