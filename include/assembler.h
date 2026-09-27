//
//  assembler.h
//  mlvm
//
//  Created by Randy Jones on 6/27/25.
//

#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <cctype>
#include <algorithm>

#include "mlvm.h"

namespace mlvm {

// Assembler implementation
class Assembler {
private:
  std::unordered_map<std::string, operations> opMap;
  
  void initializeOpMap() {
    opMap["NOOP"] = NOOP;
    opMap["END"] = END;
    opMap["MOV"] = MOVE;
    opMap["MOVE"] = MOVE;
    opMap["LDR"] = LOAD;
    opMap["LOAD"] = LOAD;
    opMap["STR"] = STORE;
    opMap["STORE"] = STORE;
    opMap["CMP"] = CMP;
    opMap["BNE"] = BNE;
    opMap["JMP"] = JMP;
    opMap["ADD"] = ADD;
    opMap["MUL"] = MUL;
    opMap["SHIFT"] = SHIFT;
    opMap["INTERP"] = INTERP;
    opMap["SVF"] = SVF;
  }
  
  std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
  }
  
  std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::stringstream ss(line);
    std::string token;
    
    // Split by spaces and commas
    while (ss >> token) {
      // Remove commas
      token.erase(std::remove(token.begin(), token.end(), ','), token.end());
      if (!token.empty()) {
        tokens.push_back(token);
      }
    }
    return tokens;
  }
  
  bool isRegister(const std::string& token) {
    return (token.length() >= 2 &&
            (token[0] == 'R' || token[0] == 'r') &&
            std::isdigit(token[1]));
  }
  
  bool isImmediate(const std::string& token) {
    return token.length() >= 2 && token[0] == '#';
  }
  
  bool isMemoryArena(const std::string& token) {
    return token.length() >= 3 && token[0] == '[' && token.back() == ']';
  }
  
  bool isLiteral(const std::string& token) {
    return token[0] == '=' ||
    (token.find('.') != std::string::npos &&
     std::isdigit(token[0]));
  }
  
  int parseRegisterNumber(const std::string& token) {
    if (!isRegister(token)) return -1;
    
    std::string numStr = token.substr(1);
    try {
      int regNum = std::stoi(numStr);
      if (regNum >= 0 && regNum < (int)kNumRegisters) {
        return regNum;
      }
    } catch (...) {
      return -1;
    }
    return -1;
  }
  
  float parseImmediate(const std::string& token) {
    if (!isImmediate(token)) return 0.0f;
    
    std::string valueStr = token.substr(1); // Remove '#'
    try {
      return std::stof(valueStr);
    } catch (...) {
      return 0.0f;
    }
  }
  
  // this needs to be redone - a lot is based on register address modes,
  // and we don't know how those work yet
  std::pair<int, int> parseMemoryArena(const std::string& token) {
    
    if (!isMemoryArena(token)) return {-1, -1};
    
    std::string inner = token.substr(1, token.length() - 2);
    size_t commaPos = inner.find(',');
    
    if (commaPos == std::string::npos) {
      std::string innerTrimmed = trim(inner);
      // immediate format
      if(isImmediate(innerTrimmed))
      {
        int offset = parseImmediate(innerTrimmed);
        return {0, offset}; // This is a simplification
      }
      else
      {
        // Simple [R1] format
        int baseReg = parseRegisterNumber(innerTrimmed);
        return {baseReg, 0}; // Base register, no offset register
      }
    }
    
    // Parse [R1, R2] or [R1, #offset] format
    std::string basePart = trim(inner.substr(0, commaPos));
    std::string offsetPart = trim(inner.substr(commaPos + 1));
    
    int baseReg = parseRegisterNumber(basePart);
    if (baseReg == -1) return {-1, -1};
    
    if (isRegister(offsetPart)) {
      int offsetReg = parseRegisterNumber(offsetPart);
      return {baseReg, offsetReg};
    } else if (isImmediate(offsetPart)) {
      // For immediate offsets, we could encode them differently
      // For now, treat as literal value
      float offset = parseImmediate(offsetPart);
      return {baseReg, (int)offset}; // This is a simplification
    }
    
    return {baseReg, 0};
  }
  
  float parseLiteral(const std::string& token) {
    std::string valueStr = token;
    if (token[0] == '=') {
      valueStr = token.substr(1);
    }
    
    try {
      return std::stof(valueStr);
    } catch (...) {
      return 0.0f;
    }
  }
  
  Operand createRegisterOperand(const std::string& token) {
    if (isRegister(token)) {
      int regNum = parseRegisterNumber(token);
      if (regNum >= 0) {
        return (REGISTER << kOperandIndexBits) | regNum;
      }
    } else if (isImmediate(token)) {
      float immediateVal = parseImmediate(token);
      // Encode immediate as 7-bit value (simplified)
      int encodedImm = std::min(127, std::max(0, (int)immediateVal));
      return (IMMEDIATE << kOperandIndexBits) | encodedImm;
    }
    
    // Default to register 0 if parsing fails
    return (REGISTER << kOperandIndexBits) | 0;
  }
  
  std::pair<Operand, Operand> createMemoryOperands(const std::string& token, uint16_t literalIndex) {
    if (isMemoryArena(token)) {
      auto [baseReg, offsetReg] = parseMemoryArena(token);
      if (baseReg >= 0) {
        // Encode 14-bit arena address across two operands
        // For now, simple encoding: baseReg in high 7 bits, offsetReg in low 7 bits
        uint16_t address = (baseReg << 7) | (offsetReg & 0x7F);
        uint8_t highBits = (address >> 7) & 0x7F;
        uint8_t lowBits = address & 0x7F;
        
        Operand src1 = (ARENA << kOperandIndexBits) | highBits;
        Operand src2 = (ARENA << kOperandIndexBits) | lowBits;
        return {src1, src2};
      }
    } else if (isLiteral(token)) {

      // Split 14-bit index across two 7-bit operands
      uint8_t highBits = (literalIndex >> 7) & 0x7F;
      uint8_t lowBits = literalIndex & 0x7F;
      
      Operand src1 = (LITERAL << kOperandIndexBits) | highBits;
      Operand src2 = (LITERAL << kOperandIndexBits) | lowBits;
      return {src1, src2};
    }
    
    // Default: arena mode, address 0
    return {(ARENA << kOperandIndexBits) | 0, (ARENA << kOperandIndexBits) | 0};
  }
  
  
public:
  Assembler() {
    initializeOpMap();
  }
  
  Program assemble(const std::string& assemblyCode) ;
  void printProgram(const Program& program) ;
};

}
