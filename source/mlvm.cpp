// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com

#include "mlvm.h"

namespace mlvm {

bool MLVM::allocateMemory(const MemoryRequirements& memReqs) {
  // TODO errors
  registers.resize(kNumRegisters);
  
  // TODO errors
  arena.resize(memReqs.stateVectors + memReqs.scratchVectors);
  return true;
}

void MLVM::setProgram(const Program& newCode) {
  program = newCode;
}

// get the value from the operand, handling the register addressing modes.
SignalBlock MLVM::getValue(Operand op)
{
  SignalBlock result;
  switch(getOperandMode(op))
  {
    case REGISTER:
      result = registers[getIndex(op)];
      break;
    case IMMEDIATE:
      // fill vector with float immediate
      result = SignalBlock(getImmediate(op));
      break;
  }
  return result;
}

// get the value from the two operands, handling the memory addressing modes.
SignalBlock MLVM::getValue2(Operand op1, Operand op2, const std::vector< float >& literals)
{
  SignalBlock result;
  size_t offset = (getIndex(op1) << 7) | getIndex(op2);
  switch(getOperandMode(op1))
  {
    case ARENA:
      result = arena[offset];
      break;
    case LITERAL:
      // fill vector with float literal
      result = SignalBlock(literals[offset]);
      break;
  }
  return result;
}

// get destination from the two operands, handling the memory addressing modes.
SignalBlock* MLVM::getDest2(Operand op1, Operand op2)
{
  SignalBlock* result{nullptr};
  size_t offset = (getIndex(op1) << 7) | getIndex(op2);
  switch(getOperandMode(op1))
  {
    case ARENA:
      result = &arena[offset];
      break;
    default:
      // return null and probably crash - literal mode
      // for dest doesn't make sense
      break;
  }
  return result;
}

void MLVM::process(AudioContext* context) {
  size_t destIdx, srcIdx1, srcIdx2;
  SignalBlock v1, v2;
  
  // main inputs / outputs are dynamic, so check them
  if (context->outputs.size() < 1) return;
  
  // copy inputs to registers
  for(int i=0; i<context->inputs.size(); ++i)
  {
    registers[i] = context->inputs[i];
  }

  // Here is the innermost loop that interprets the bytecode program.
  // The program will generate one vector of output.
  // NOTE: Aside from the main switch, there should be few if any branches.
  
  programCounter = 0;
  while(1) {
    auto inst = program.instructions[programCounter++];
    destIdx = getIndex(inst.dest);

    v1 = getValue(inst.src1);
    v2 = getValue(inst.src2);

    switch (inst.opcode) {
      case NOOP:
        break;
      case MOVE:
        registers[destIdx] = v1;
        break;
      case LOAD:
        registers[destIdx] = getValue2(inst.src1, inst.src2, program.literalPool);
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
      case END:
        goto endprogram;
    }
  }

  endprogram:
  
  // copy registers to outputs
  for(int i=0; i<context->outputs.size(); ++i)
  {
    context->outputs[i] = registers[i];
  }

  // TEMP
  static int testCounter{0};
  const int nSamples = context->getSampleRate();
  auto timeInfo = context->getTimeInfo();
  testCounter += kFramesPerBlock;
  if (testCounter >= nSamples) {
    testCounter -= nSamples;
    std::cout << "bpm:" << timeInfo.bpm << "\n";
    std::cout << "phase:" << timeInfo.quarterNotesPhase_[0] << "\n";
    std::cout << "samples since start:" << timeInfo.samplesSinceStart << "\n";
    std::cout << "output 0-0:" << context->outputs[0][0] << "\n";
  }
}

} // namespace ml

  
