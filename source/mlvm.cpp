// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com

#include "mlvm.h"

namespace mlvm {

bool MLVM::allocateArenas() {
  bool success{true};

  // clear all arenas
  for(auto& a : arenas) a.clear();
  
  // allocate arenas in manifest
  // TODO
  
  return success;
}

void MLVM::load(const Program& newCode) {
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
    uint8_t b6 = inst[6]; // state arena, projection type, jmp offset 0:7
    uint8_t b7 = inst[7]; // state chunk, jmp offset 8:15
    
    // operations that have a 32 bit float immediate value store it in fields 4–7.

    // by grouping the memory opcodes together and so on, we can do quick bit tests
    // to decide if params and state pointers, values, immediates and so on need
    // to be decoded. for now, do them all.
    float32_t f0 = getFloatImmediate(inst);
    int16_t offset = getAddressOffset(inst);
    float* arenaPtr1 = getArenaPtr(b4, b5, b3); // params, load src or store dest
    float* arenaPtr2 = getArenaPtr(b6, b7, b3); // state


    
    switch (opcode) {
      case NOOP:
        break;
      case END:
        goto endprogram;
      case MOVE:
        registers[b1] = registers[b2];
        break;
      case LOAD:
        memcpy(&registers[b1], arenaPtr1, sizeof(SignalBlock));
        break;
      case STORE:
        // in a store, src and dest are reversed in order to index arena destination
        memcpy(arenaPtr1, &registers[b1], sizeof(SignalBlock));
        break;
      case ADD:
        registers[b1] = add(registers[b2], registers[b4]);
        break;
      case MUL:
        registers[b1] = multiply(registers[b2], registers[b4]);
       break;

    }
  }

  endprogram:
  return;
}

} // namespace ml

  
