//
//  assembler.cpp
//  mlvm
//
//  Created by Randy Jones on 6/27/25.
//

#include "assembler.h"

namespace mlvm {


Program Assembler::assemble(const std::string& assemblyCode) {
  Program program;
  program.memReqs = {0, 0}; // Default memory requirements
  
  std::stringstream ss(assemblyCode);
  std::string line;
  
  while (std::getline(ss, line)) {
    line = trim(line);
    
    // Skip empty lines and comments
    if (line.empty() || line[0] == ';' || line.substr(0, 2) == "//") {
      continue;
    }
    
    std::vector<std::string> tokens = tokenize(line);
    if (tokens.empty()) continue;
    
    // Parse instruction
    std::string opName = tokens[0];
    std::transform(opName.begin(), opName.end(), opName.begin(), ::toupper);
    
    if (opMap.find(opName) == opMap.end()) {
      std::cerr << "Unknown operation: " << opName << std::endl;
      continue;
    }
    
    Instruction instr = {};
    instr.opcode = opMap[opName]; // MODE_0 by default
    
    // Parse operands based on instruction type
    operations op = opMap[opName];
    
    if (op == LOAD || op == STORE) {
      // LOAD/STORE: dest is register, src is memory (uses src1+src2)
      if (tokens.size() >= 2) {
        instr.dest = createRegisterOperand(tokens[1]);
      }
      if (tokens.size() >= 3) {
        uint16_t literalIndex{0};
        if(isLiteral(tokens[2]))
        {
          // Add float to literal pool and get index
          program.literalPool.push_back(parseLiteral(tokens[2]));
          literalIndex = program.literalPool.size() - 1;
          literalIndex &= 0xCFFF; // toy impl, wrap
        }
        auto [src1, src2] = createMemoryOperands(tokens[2], literalIndex);
        instr.src1 = src1;
        instr.src2 = src2;
      }
    } else {
      // Other instructions use register/immediate operands
      if (tokens.size() >= 2) {
        instr.dest = createRegisterOperand(tokens[1]);
      }
      if (tokens.size() >= 3) {
        instr.src1 = createRegisterOperand(tokens[2]);
      }
      if (tokens.size() >= 4) {
        instr.src2 = createRegisterOperand(tokens[3]);
      }
    }
    
    program.instructions.push_back(instr);
  }
  
  return program;
}
  
void Assembler::printProgram(const Program& program) {
  std::cout << "Program with " << program.instructions.size() << " instructions:\n";
  
  for (size_t i = 0; i < program.instructions.size(); ++i) {
    const auto& instr = program.instructions[i];
    operations op = (operations)getOperation(instr.opcode);
    
    std::cout << i << ": ";
    std::cout << "Op=" << op << " ";
    
    if (op == LOAD || op == STORE) {
      // For LOAD/STORE, combine src1+src2 to show memory address
      uint16_t memAddr = ((getIndex(instr.src1) << 7) | getIndex(instr.src2));
      bool isLiteral = (getOperandMode(instr.src1) == LITERAL);
      std::cout << "Dest=R" << getIndex(instr.dest) << " ";
      std::cout << "MemAddr=" << memAddr << "(" << (isLiteral ? "LITERAL" : "ARENA") << ")";
    } else {
      std::cout << "Dest=R" << getIndex(instr.dest) << " ";
      std::cout << "Src1=";
      if (getOperandMode(instr.src1) == IMMEDIATE) {
        std::cout << "#" << getIndex(instr.src1);
      } else {
        std::cout << "R" << getIndex(instr.src1);
      }
      std::cout << " Src2=";
      if (getOperandMode(instr.src2) == IMMEDIATE) {
        std::cout << "#" << getIndex(instr.src2);
      } else {
        std::cout << "R" << getIndex(instr.src2);
      }
    }
    std::cout << "\n";
  }
  
  if (!program.literalPool.empty()) {
    std::cout << "\nLiteral Pool:\n";
    for (size_t i = 0; i < program.literalPool.size(); ++i) {
      std::cout << i << ": " << program.literalPool[i] << "\n";
    }
  }
}


}
