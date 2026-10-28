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
    } else {
      parseInstruction(tokens);
    }
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
  void parseInstruction(const std::vector<std::string_view>& tokens) {
    std::string_view mnemonic = tokens[0];
    std::string_view bankText;
    size_t dot = mnemonic.find('.');
    if (dot != std::string_view::npos) {
      bankText = mnemonic.substr(dot + 1);
      mnemonic = mnemonic.substr(0, dot);
    }

    auto op = opcodeFromName(toUpper(mnemonic));
    if (!op) {
      error("unknown instruction '" + std::string(mnemonic) + "'");
      return;
    }

    unsigned bank = 0;
    if (!bankText.empty() && (!parseUInt(bankText, bank) || bank > 255)) {
      error("bad bank size '" + std::string(bankText) + "'");
      return;
    }
    if (bank % 4 != 0) {
      error("bank size must be 0 or a multiple of 4, got " + std::to_string(bank));
      return;
    }

    Instruction inst{};
    inst[0] = *op;
    inst[3] = static_cast<uint8_t>(bank);

    std::vector<Operand> operands;
    bool operandsOk = true;
    for (size_t i = 1; i < tokens.size(); ++i) {
      auto operand = parseOperand(tokens[i]);
      if (!operand) { operandsOk = false; continue; }
      operands.push_back(std::move(*operand));
    }
    if (!operandsOk) return;

    if (!encode(inst, operands, opcodeHasState(*op))) return;

    // a bank of N voices occupies N registers from each register operand.
    if (bank) {
      for (uint8_t field : {uint8_t(1), uint8_t(2), uint8_t(4), uint8_t(5)}) {
        if ((registerFields_ & (1 << field)) && (inst[field] + bank > MLVM::kNumRegisters)) {
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

  // bitmask of the fields that hold register indices in the instruction most recently encoded.
  uint8_t registerFields_{0};

  // Place operands into fields by kind. Returns false and reports an error if
  // two operands claim the same field. For a stateful opcode the last arena
  // reference is its state and always goes to fields 6,7.
  bool encode(Instruction& inst, const std::vector<Operand>& operands, bool hasState) {
    static constexpr uint8_t kRegisterSlots[] = {1, 2, 4, 5};
    static constexpr uint8_t kArenaSlots[][2] = {{4, 5}, {6, 7}};
    size_t nextRegister = 0;
    uint8_t used = 0;   // bitmask of fields written
    registerFields_ = 0;

    size_t nArenaRefs = 0;
    for (const auto& operand : operands) if (std::get_if<ArenaRef>(&operand)) ++nArenaRefs;
    size_t arenaRefsSeen = 0;
    if (hasState && nArenaRefs == 0) {
      error("this instruction keeps state and needs a state chunk, e.g. kstate/0");
      return false;
    }

    auto claim = [&](uint8_t field) {
      if (used & (1 << field)) {
        error("too many operands: field " + std::to_string(field) + " is already in use");
        return false;
      }
      used |= (1 << field);
      return true;
    };

    for (const auto& operand : operands) {
      if (auto* r = std::get_if<RegisterRef>(&operand)) {
        if (nextRegister >= 4) { error("too many register operands"); return false; }
        uint8_t field = kRegisterSlots[nextRegister++];
        if (!claim(field)) return false;
        inst[field] = r->idx;
        registerFields_ |= (1 << field);
      } else if (auto* a = std::get_if<ArenaRef>(&operand)) {
        // the state ref of a stateful op goes to fields 6,7. Any other arena ref takes
        // the first field pair that is still free (fields 4,5 unless registers hold
        // them, as in SVF: in, cutoff, k).
        ++arenaRefsSeen;
        const bool isState = hasState && (arenaRefsSeen == nArenaRefs);
        int pair = -1;
        if (isState) {
          pair = 1;
        } else {
          const int lastPair = hasState ? 0 : 1;   // pair 1 is reserved for the state
          for (int p = 0; p <= lastPair && pair < 0; ++p) {
            const auto& slots = kArenaSlots[p];
            if (!(used & (1 << slots[0])) && !(used & (1 << slots[1]))) pair = p;
          }
        }
        if (pair < 0) { error("too many operands: no field pair is free for an arena reference"); return false; }
        const auto& slots = kArenaSlots[pair];
        if (!claim(slots[0]) || !claim(slots[1])) return false;
        inst[slots[0]] = a->arena;
        inst[slots[1]] = a->chunk;
      } else if (auto* f = std::get_if<FloatImm>(&operand)) {
        for (uint8_t field = 4; field <= 7; ++field) if (!claim(field)) return false;
        setFloatImmediate(inst, f->value);
      } else if (auto* b = std::get_if<ByteImm>(&operand)) {
        if (!claim(5)) return false;
        inst[5] = b->value;
      } else if (auto* l = std::get_if<LabelRef>(&operand)) {
        if (!claim(6) || !claim(7)) return false;
        fixups_.push_back({result_.program.instructions.size(), l->name, line_});
      }
    }
    return true;
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
