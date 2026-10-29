// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com

#include "assembler.h"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <unordered_map>
#include <variant>

namespace mlvm {

namespace {

// ----------------------------------------------------------------
// operand kinds. Kind is decided purely from the token text.

struct RegisterRef { uint8_t idx; };
struct ArenaRef { uint8_t arena; uint8_t chunk; };
struct FloatImm { float value; };
struct ByteImm { uint8_t value; };
struct LabelRef { std::string name; };

using Operand = std::variant<RegisterRef, ArenaRef, FloatImm, ByteImm, LabelRef>;

OperandKind kindOf(const Operand& o) {
  if (std::get_if<RegisterRef>(&o)) return OperandKind::kReg;
  if (std::get_if<ArenaRef>(&o)) return OperandKind::kArena;
  if (std::get_if<FloatImm>(&o)) return OperandKind::kFloat;
  if (std::get_if<ByteImm>(&o)) return OperandKind::kByte;
  return OperandKind::kLabel;
}

std::string_view kindName(OperandKind k) {
  switch (k) {
    case OperandKind::kReg: return "a register (r0..r255)";
    case OperandKind::kArena: return "an arena reference (name/chunk)";
    case OperandKind::kFloat: return "a float immediate (e.g. 1.0f)";
    case OperandKind::kByte: return "an integer 0..255";
    case OperandKind::kLabel: return "a label";
  }
  return "?";
}

// "dest, src1, state" style description of a format, for error messages.
std::string describe(const FormatSpec& spec) {
  std::string s;
  for (size_t i = 0; i < spec.count; ++i) {
    if (i) s += ", ";
    switch (spec.slots[i].kind) {
      case OperandKind::kReg: s += "r"; break;
      case OperandKind::kArena: s += "arena/chunk"; break;
      case OperandKind::kFloat: s += "float"; break;
      case OperandKind::kByte: s += "element"; break;
      case OperandKind::kLabel: s += "label"; break;
    }
  }
  return s.empty() ? "no operands" : s;
}

// a branch whose target label is resolved after all instructions are seen.
struct Fixup {
  size_t instructionIdx;
  std::string label;
  int line;
};

// ----------------------------------------------------------------
// text helpers

std::string_view trim(std::string_view s) {
  const char* ws = " \t\r\n";
  size_t start = s.find_first_not_of(ws);
  if (start == std::string_view::npos) return {};
  size_t end = s.find_last_not_of(ws);
  return s.substr(start, end - start + 1);
}

std::string_view stripComment(std::string_view s) {
  size_t slashes = s.find("//");
  size_t semi = s.find(';');
  size_t cut = std::min(slashes, semi);
  return cut == std::string_view::npos ? s : s.substr(0, cut);
}

std::string toUpper(std::string_view s) {
  std::string r(s);
  for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return r;
}

// split on whitespace and commas.
std::vector<std::string_view> splitTokens(std::string_view s) {
  std::vector<std::string_view> tokens;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) ++i;
    size_t start = i;
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != ',') ++i;
    if (i > start) tokens.push_back(s.substr(start, i - start));
  }
  return tokens;
}

bool isAllDigits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

bool parseUInt(std::string_view s, unsigned& out) {
  if (!isAllDigits(s) || s.size() > 9) return false;
  out = 0;
  for (char c : s) out = out * 10 + (c - '0');
  return true;
}

// a float literal must contain '.' or end in 'f' so that bare integers stay integers.
bool parseFloatLiteral(std::string_view s, float& out) {
  if (s.empty()) return false;
  bool suffix = (s.back() == 'f' || s.back() == 'F');
  if (!suffix && s.find('.') == std::string_view::npos) return false;
  std::string body(suffix ? s.substr(0, s.size() - 1) : s);
  if (body.empty()) return false;
  char* end = nullptr;
  out = std::strtof(body.c_str(), &end);
  return end == body.c_str() + body.size();
}

bool isIdentifier(std::string_view s) {
  if (s.empty()) return false;
  unsigned char c0 = static_cast<unsigned char>(s[0]);
  if (!(std::isalpha(c0) || c0 == '_')) return false;
  for (char c : s) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (!(std::isalnum(uc) || uc == '_' || uc == '-')) return false;
  }
  return true;
}

// ----------------------------------------------------------------
// Parser: one pass over the lines, then label fixups.

class Parser {
public:
  AssemblyResult parse(std::string_view source) {
    line_ = 0;
    size_t pos = 0;
    while (pos <= source.size()) {
      size_t nl = source.find('\n', pos);
      std::string_view rawLine = source.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
      ++line_;
      parseLine(trim(stripComment(rawLine)));
      if (nl == std::string_view::npos) break;
      pos = nl + 1;
    }
    resolveFixups();
    return std::move(result_);
  }

private:
  AssemblyResult result_;
  std::unordered_map<std::string, uint8_t> arenaIndices_;
  std::unordered_map<std::string, size_t> labels_;   // label -> index of the next instruction
  std::vector<Fixup> fixups_;
  int line_{0};

  void error(std::string msg) {
    result_.errors.push_back({line_, std::move(msg)});
  }

  void parseLine(std::string_view text) {
    if (text.empty()) return;
    auto tokens = splitTokens(text);
    if (tokens.empty()) return;

    // label?
    if (tokens[0].back() == ':') {
      std::string_view name = tokens[0].substr(0, tokens[0].size() - 1);
      if (!isIdentifier(name)) {
        error("bad label '" + std::string(name) + "'");
      } else if (labels_.count(std::string(name))) {
        error("duplicate label '" + std::string(name) + "'");
      } else {
        labels_[std::string(name)] = result_.program.instructions.size();
      }
      tokens.erase(tokens.begin());
      if (tokens.empty()) return;
    }

    std::string head = toUpper(tokens[0]);
    if (head == "ARENA") {
      parseArena(tokens, false);
    } else if (head == "EXTERN") {
      parseArena(tokens, true);
    } else if (head == "DATA") {
      parseData(tokens);
    } else {
      parseInstruction(tokens);
    }
  }

  // DATA name/chunk v0 v1 ...   constants written into a chunk of an owned arena at load
  void parseData(const std::vector<std::string_view>& tokens) {
    if (tokens.size() < 3) {
      error("DATA takes an arena/chunk and at least one value");
      return;
    }
    auto target = parseOperand(tokens[1]);
    if (!target) return;
    auto* ref = std::get_if<ArenaRef>(&*target);
    if (!ref) {
      error("DATA's first operand must be an arena reference (name/chunk)");
      return;
    }
    if (ref->arena >= result_.program.manifest.size()) {
      error("DATA refers to arena " + std::to_string(ref->arena) + " which is not declared");
      return;
    }
    const ArenaDescription& desc = result_.program.manifest[ref->arena];
    if (desc.external) {
      error("DATA cannot initialize an external arena ('" + desc.name + "')");
      return;
    }
    DataInit init{ref->arena, ref->chunk, {}};
    for (size_t i = 2; i < tokens.size(); ++i) {
      float f = 0.f;
      unsigned u = 0;
      if (parseFloatLiteral(tokens[i], f)) {
        init.values.push_back(f);
      } else if (parseUInt(tokens[i], u)) {
        init.values.push_back(static_cast<float>(u));
      } else {
        error("bad DATA value '" + std::string(tokens[i]) + "'");
        return;
      }
    }
    if (init.values.size() > desc.chunkSizeInFloats) {
      error("DATA has " + std::to_string(init.values.size()) + " values but a chunk of '" + desc.name +
            "' holds " + std::to_string(desc.chunkSizeInFloats));
      return;
    }
    result_.program.data.push_back(std::move(init));
  }

  // ARENA name chunkSizeInFloats    allocated by the VM
  // EXTERN name chunkSizeInFloats   bound by the host before each block
  void parseArena(const std::vector<std::string_view>& tokens, bool external) {
    if (tokens.size() != 3) {
      error(std::string(external ? "EXTERN" : "ARENA") + " takes a name and a chunk size in floats");
      return;
    }
    std::string name(tokens[1]);
    unsigned chunkSize = 0;
    if (!isIdentifier(name)) {
      error("bad arena name '" + name + "'");
      return;
    }
    if (!parseUInt(tokens[2], chunkSize) || chunkSize == 0) {
      error("bad arena chunk size '" + std::string(tokens[2]) + "'");
      return;
    }
    if (arenaIndices_.count(name)) {
      error("duplicate arena '" + name + "'");
      return;
    }
    if (result_.program.manifest.size() >= MLVM::kNumArenas) {
      error("too many arenas (max " + std::to_string(MLVM::kNumArenas) + ")");
      return;
    }
    arenaIndices_[name] = static_cast<uint8_t>(result_.program.manifest.size());
    result_.program.manifest.push_back({name, chunkSize, external});
  }

  // MNEMONIC[.bank] operand, operand, ...
  // Operands are placed by the opcode's format (the instruction table row), never
  // by their position among other kinds.
  void parseInstruction(const std::vector<std::string_view>& tokens) {
    std::string_view mnemonic = tokens[0];
    std::string_view bankText;
    size_t dot = mnemonic.find('.');
    if (dot != std::string_view::npos) {
      bankText = mnemonic.substr(dot + 1);
      mnemonic = mnemonic.substr(0, dot);
    }

    const std::string name = toUpper(mnemonic);
    auto op = opcodeFromName(name);
    if (!op) {
      error("unknown instruction '" + std::string(mnemonic) + "'");
      return;
    }
    const FormatSpec spec = formatSpec(opcodeFormat(*op));

    unsigned bank = 0;
    if (!bankText.empty() && (!parseUInt(bankText, bank) || bank > 255)) {
      error("bad bank size '" + std::string(bankText) + "'");
      return;
    }
    if (bank % 4 != 0) {
      error("bank size must be 0 or a multiple of 4, got " + std::to_string(bank));
      return;
    }
    if (bank && !spec.hasBank) {
      error(name + " does not take a bank size");
      return;
    }

    std::vector<Operand> operands;
    bool operandsOk = true;
    for (size_t i = 1; i < tokens.size(); ++i) {
      auto operand = parseOperand(tokens[i]);
      if (!operand) { operandsOk = false; continue; }
      operands.push_back(std::move(*operand));
    }
    if (!operandsOk) return;

    if (operands.size() != spec.count) {
      error(name + " takes " + describe(spec) + " (" + std::to_string(spec.count) + " operands), got " +
            std::to_string(operands.size()));
      return;
    }

    Instruction inst{};
    inst[0] = *op;
    inst[3] = static_cast<uint8_t>(bank);
    uint8_t registerFields = 0;

    for (size_t i = 0; i < spec.count; ++i) {
      const OperandSlot& slot = spec.slots[i];
      const Operand& operand = operands[i];
      if (kindOf(operand) != slot.kind) {
        error("operand " + std::to_string(i + 1) + " of " + name + " must be " + std::string(kindName(slot.kind)) +
              " (" + name + " takes " + describe(spec) + ")");
        return;
      }
      switch (slot.kind) {
        case OperandKind::kReg:
          inst[slot.field] = std::get<RegisterRef>(operand).idx;
          registerFields |= static_cast<uint8_t>(1 << slot.field);
          break;
        case OperandKind::kArena: {
          const auto& a = std::get<ArenaRef>(operand);
          inst[slot.field] = a.arena;
          inst[slot.field + 1] = a.chunk;
          break;
        }
        case OperandKind::kFloat:
          setFloatImmediate(inst, std::get<FloatImm>(operand).value);
          break;
        case OperandKind::kByte:
          inst[slot.field] = std::get<ByteImm>(operand).value;
          break;
        case OperandKind::kLabel:
          fixups_.push_back({result_.program.instructions.size(), std::get<LabelRef>(operand).name, line_});
          break;
      }
    }

    // a bank of N voices occupies N registers from each register operand.
    if (bank) {
      for (uint8_t field = 1; field < 8; ++field) {
        if ((registerFields & (1 << field)) && (inst[field] + bank > MLVM::kNumRegisters)) {
          error("bank of " + std::to_string(bank) + " voices at r" + std::to_string(inst[field]) +
                " does not fit in the register file");
          return;
        }
      }
    }
    result_.program.instructions.push_back(inst);
  }

  std::optional<Operand> parseOperand(std::string_view tok) {
    // register: r0..r255
    if (tok.size() >= 2 && (tok[0] == 'r' || tok[0] == 'R') && isAllDigits(tok.substr(1))) {
      unsigned idx = 0;
      if (!parseUInt(tok.substr(1), idx) || idx >= MLVM::kNumRegisters) {
        error("register out of range '" + std::string(tok) + "'");
        return std::nullopt;
      }
      return Operand{RegisterRef{static_cast<uint8_t>(idx)}};
    }

    // arena ref: name/chunk or index/chunk
    size_t slash = tok.find('/');
    if (slash != std::string_view::npos) {
      std::string_view arenaText = tok.substr(0, slash);
      std::string_view chunkText = tok.substr(slash + 1);
      unsigned arena = 0, chunk = 0;
      if (isAllDigits(arenaText)) {
        if (!parseUInt(arenaText, arena) || arena >= MLVM::kNumArenas) {
          error("arena index out of range '" + std::string(tok) + "'");
          return std::nullopt;
        }
      } else {
        auto it = arenaIndices_.find(std::string(arenaText));
        if (it == arenaIndices_.end()) {
          error("undeclared arena '" + std::string(arenaText) + "'");
          return std::nullopt;
        }
        arena = it->second;
      }
      if (!parseUInt(chunkText, chunk) || chunk >= MLVM::kChunksPerArena) {
        error("bad arena chunk '" + std::string(tok) + "'");
        return std::nullopt;
      }
      return Operand{ArenaRef{static_cast<uint8_t>(arena), static_cast<uint8_t>(chunk)}};
    }

    // bare integer: one byte
    if (isAllDigits(tok)) {
      unsigned v = 0;
      if (!parseUInt(tok, v) || v > 255) {
        error("integer operand out of range '" + std::string(tok) + "'");
        return std::nullopt;
      }
      return Operand{ByteImm{static_cast<uint8_t>(v)}};
    }

    // float immediate
    float f = 0.f;
    if (parseFloatLiteral(tok, f)) {
      return Operand{FloatImm{f}};
    }

    // label
    if (isIdentifier(tok)) {
      return Operand{LabelRef{std::string(tok)}};
    }

    error("bad operand '" + std::string(tok) + "'");
    return std::nullopt;
  }

  void resolveFixups() {
    for (const auto& fixup : fixups_) {
      auto it = labels_.find(fixup.label);
      if (it == labels_.end()) {
        result_.errors.push_back({fixup.line, "undefined label '" + fixup.label + "'"});
        continue;
      }
      // offset is relative to the instruction after the branch
      long long offset = static_cast<long long>(it->second) - static_cast<long long>(fixup.instructionIdx + 1);
      if (offset < INT16_MIN || offset > INT16_MAX) {
        result_.errors.push_back({fixup.line, "branch to '" + fixup.label + "' is out of range"});
        continue;
      }
      if (fixup.instructionIdx < result_.program.instructions.size()) {
        setAddressOffset(result_.program.instructions[fixup.instructionIdx], static_cast<int16_t>(offset));
      }
    }
  }
};

} // anonymous namespace

AssemblyResult assemble(std::string_view source) {
  Parser p;
  return p.parse(source);
}

std::string disassemble(const Program& program) {
  std::ostringstream out;
  for (size_t i = 0; i < program.manifest.size(); ++i) {
    const auto& a = program.manifest[i];
    out << (a.external ? "EXTERN " : "ARENA ") << a.name << " " << a.chunkSizeInFloats << "   // arena " << i << "\n";
  }
  for (const auto& d : program.data) {
    const std::string name = d.arena < program.manifest.size() ? program.manifest[d.arena].name : std::to_string(d.arena);
    out << "DATA " << name << "/" << static_cast<unsigned>(d.chunk);
    for (float v : d.values) out << " " << v;
    out << "\n";
  }
  for (size_t i = 0; i < program.instructions.size(); ++i) {
    const Instruction& inst = program.instructions[i];
    std::string mnemonic(opcodeName(getOpcode(inst)));
    if (inst[3]) mnemonic += "." + std::to_string(inst[3]);
    out << i << ":\t" << mnemonic << "\t";
    for (size_t f = 1; f < inst.size(); ++f) {
      out << static_cast<unsigned>(inst[f]) << (f + 1 < inst.size() ? " " : "");
    }
    out << "\n";
  }
  return out.str();
}

} // namespace mlvm
