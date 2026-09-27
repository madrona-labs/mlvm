// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com

#include "mlvm.h"

namespace mlvm {

bool MLVM::allocateMemory(const MemoryRequirements& memReqs) {
  // TODO errors
  registers.resize(kNumRegisters);
  
  // TODO errors
  arenas.resize(memReqs.stateVectors + memReqs.scratchVectors);
  return true;
}

void MLVM::setProgram(const Program& newCode) {
  program = newCode;
}

// update docs:
// a float is the smallest thing in an arena, so arena chunk size 1 means 4 bytes.
// arena instructions all have a bank size operand, so multiply the arena pointer
// by the bank size.

chug chug
float* MLVM::getArenaPtr(uint8_t arenaIdx, uint8_t chunk, uint8_t bankSize)
{
  size_t arenaChunkSizeInBytes = arenas[arenaIdx].chunkSize*4*bankSize;
  return arenas[arenaIdx].floatVec.data() + chunk*arenaChunkSizeInBytes;
}

void MLVM::process(AudioContext* context) {
  
  // operands
  uint8_t destIdx;
  SignalBlock src1, src2, src3;
  SignalBlock v1, v2;
  
  // main inputs / outputs are dynamic, so check them
  if (context->outputs.size() < 1) return;
  
  // copy inputs to registers
  for(int i=0; i<context->inputs.size(); ++i)
  {
    registers[i] = context->inputs[i];
  }

  // Here is the innermost loop that interprets the bytecode program.
  // The program will generate one SignalBlock of output.
  // NOTE: Aside from the main switch, there should be few if any branches.
  
  programCounter = 0;
  while(1) {
    auto inst = program.instructions[programCounter++];
    
    // decode operands. The ISA has the property that if an operand exists, it
    // is in the same location for all operations that use it. Also there are
    // not a ton of operands. So we decode them all here before the opcode switch.
    
    uint8_t opcode = getOpcode(inst);
    uint8_t b1 = inst[1]; // dest idx
    uint8_t b2 = inst[2]; // src1 idx
    uint8_t b3 = inst[3]; // bank size
    uint8_t b4 = inst[4]; // src2 idx, params arena
    uint8_t b5 = inst[5]; // src3 idx, params chunk, element #
    uint8_t b6 = inst[6]; // state arena, projection type, jmp offset 8:15
    uint8_t b7 = inst[7]; // state chunk, jmp offset 8:15

    // by grouping the memory opcodes together and so on, we can do quick bit tests
    // to decide if pointers, values immediates and so on need to be decoded. for now,
    // do them all.
    float32_t f0 = getFloatImmediate(inst);
    int16_t offset = getAddressOffset(inst);
    SignalBlock* paramsPtr = getArenaPtr(b4, b5, b3);
    SignalBlock* statePtr = getArenaPtr(b6, b7, b3);

    switch (opcode) {
      case NOOP:
        break;
      case END:
        goto endprogram;
      case MOVE:
        registers[destIdx] = registers[src1Idx];
        break;
      case LOAD:
        registers[destIdx] = getValue2(inst.src1, inst.src2);
        break;
      case STORE:
        // in a store, src and dest are reversed
        *(getDest2(inst.src1, inst.src2)) = getValue(inst.dest);
        break;
      case ADD:
        registers[destIdx] = add(v1, v2);
        break;
      case MUL:
        registers[destIdx] = multiply(v1, v2);
        break;

    }
  }

  endprogram:
  
  // copy registers to outputs
  for(int i=0; i<context->outputs.size(); ++i)
  {
    context->outputs[i] = registers[i];
  }

}

} // namespace ml

  
