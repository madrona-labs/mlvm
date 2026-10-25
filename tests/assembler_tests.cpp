// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com
//
// Tests for the assembler and the VM instruction loop. No framework: each
// check prints on failure and the process exit code is the failure count.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "assembler.h"
#include "mlvm.h"

namespace {

int failures = 0;

#define CHECK(cond) \
  do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_EQ(a, b) \
  do { auto va = (a); auto vb = (b); if (!(va == vb)) { ++failures; \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
      (long long)va, (long long)vb); } } while (0)

bool hasErrorOnLine(const mlvm::AssemblyResult& r, int line) {
  for (const auto& e : r.errors) if (e.line == line) return true;
  return false;
}

void printErrors(const mlvm::AssemblyResult& r) {
  for (const auto& e : r.errors) std::printf("  line %d: %s\n", e.line, e.message.c_str());
}

// ----------------------------------------------------------------

void testOpcodeTable() {
  using namespace mlvm;
  CHECK(opcodeName(NOOP) == "NOOP");
  CHECK(opcodeName(HALT) == "HALT");
  CHECK(opcodeFromName("ADD").has_value());
  CHECK_EQ(*opcodeFromName("ADD"), ADD);
  CHECK(!opcodeFromName("add").has_value());      // table is uppercase; assembler upcases
  CHECK(!opcodeFromName("BOGUS").has_value());
  // round trip every opcode
  for (size_t i = 0; i < NUM_OPCODES; ++i) {
    auto op = static_cast<Opcode>(i);
    auto back = opcodeFromName(opcodeName(op));
    CHECK(back.has_value() && *back == op);
  }
}

void testSketchProgram() {
  using namespace mlvm;
  // the architecture doc's example, in the agreed syntax
  const char* src = R"(
    // MANIFEST
    ARENA svf-params 5
    ARENA svf-states 2
    ARENA delay-params 10
    ARENA delay-states 4096   // delay memory indexable in 4096-float chunks

    // INSTRUCTIONS
    LOAD     r12, delay-params/1
    LOAD_I   r12, 42.333f
    filter1:  ; a label
    SVF.4    r2, r1, svf-params/3, svf-states/3
    saw.16   r4, r3, delay-params/0, delay-states/0
    CMP      r5, r6
    BNE      filter1
    HALT
  )";

  auto r = assemble(src);
  if (!r.ok()) printErrors(r);
  CHECK(r.ok());
  CHECK_EQ(r.program.manifest.size(), 4u);
  CHECK(r.program.manifest[3].name == "delay-states");
  CHECK_EQ(r.program.manifest[3].chunkSizeInFloats, 4096u);
  CHECK_EQ(r.program.instructions.size(), 7u);
  if (r.program.instructions.size() != 7) return;

  const auto& code = r.program.instructions;

  // LOAD r12, delay-params/1 -> register in field 1, arena 2 chunk 1 in fields 4,5
  CHECK((code[0] == Instruction{LOAD, 12, 0, 0, 2, 1, 0, 0}));

  // LOAD_I r12, 42.333f -> float in fields 4..7
  CHECK_EQ(code[1][0], LOAD_I);
  CHECK_EQ(code[1][1], 12);
  CHECK(getFloatImmediate(code[1]) == 42.333f);

  // SVF.4 r2, r1, svf-params/3, svf-states/3
  CHECK((code[2] == Instruction{SVF, 2, 1, 4, 0, 3, 1, 3}));

  // lower-case mnemonic, bank 16
  CHECK((code[3] == Instruction{SAW, 4, 3, 16, 2, 0, 3, 0}));

  // CMP r5, r6 -> fields 1 and 2 under the generic rule
  CHECK((code[4] == Instruction{CMP, 5, 6, 0, 0, 0, 0, 0}));

  // BNE filter1: label is at instruction 2; branch is instruction 5; offset = 2 - 6 = -4
  CHECK_EQ(code[5][0], BNE);
  CHECK_EQ(getAddressOffset(code[5]), -4);

  CHECK_EQ(code[6][0], HALT);

  // disassembly mentions every mnemonic
  std::string listing = disassemble(r.program);
  CHECK(listing.find("SVF.4") != std::string::npos);
  CHECK(listing.find("ARENA delay-states 4096") != std::string::npos);
}

void testErrors() {
  using namespace mlvm;
  const char* src =
    "ARENA params 4\n"          // 1
    "FROB r1, r2\n"              // 2: unknown mnemonic
    "MOVE r1, r256\n"            // 3: register out of range
    "LOAD r1, nothere/0\n"       // 4: undeclared arena
    "JMP nowhere\n"              // 5: undefined label
    "ADD r1, r2, r3, r4, r5\n"   // 6: too many registers
    "LOAD_I r1, 1.0, 2.0\n"      // 7: two immediates overlap
    "ARENA params 4\n"           // 8: duplicate arena
    "HALT\n";                    // 9

  auto r = assemble(src);
  CHECK(!r.ok());
  CHECK_EQ(r.errors.size(), 7u);
  for (int line : {2, 3, 4, 5, 6, 7, 8}) {
    if (!hasErrorOnLine(r, line)) { ++failures; std::printf("FAIL: expected an error on line %d\n", line); }
  }
  if (r.errors.size() != 7) printErrors(r);
}

void testVMArithmetic() {
  using namespace mlvm;
  auto r = assemble(R"(
    LOAD_I r1, 2.0f
    LOAD_I r2, 3.0f
    ADD    r0, r1, r2       // 5
    NEG    r3, r0           // -5
    LERP   r4, r1, r2, r1   // lerp(2, 3, 2) = 4
    HALT
    LOAD_I r0, 99.0f        // never reached
  )");
  if (!r.ok()) printErrors(r);
  CHECK(r.ok());

  MLVM vm;
  CHECK(vm.loadProgram(r.program));
  vm.run();
  for (size_t i = 0; i < ml::kFramesPerBlock; ++i) {
    CHECK(vm.registers[0][i] == 5.f);
    CHECK(vm.registers[3][i] == -5.f);
    CHECK(vm.registers[4][i] == 4.f);
  }
}

void testVMArenaAndBranch() {
  using namespace mlvm;
  // count r0 down from 3 to 0 using a loop; STORE the running value to an
  // arena each pass so we can check the arena was written.
  auto r = assemble(R"(
    ARENA scratch 16
    LOAD_I r0, 3.0f
    LOAD_I r1, 1.0f
    LOAD_I r2, 0.0f
  loop:
    SUB    r0, r0, r1
    STORE  r0, scratch/2
    CMP    r0, r2            // flag = any(r0 > 0)
    BNE    loop
    LOAD   r5, scratch/2     // read back the last stored value
    HALT
  )");
  if (!r.ok()) printErrors(r);
  CHECK(r.ok());

  MLVM vm;
  CHECK(vm.loadProgram(r.program));
  CHECK_EQ(vm.arenas[0].floatVec.size(), 256u * 16u);
  vm.run();
  CHECK(vm.registers[0][0] == 0.f);
  CHECK(vm.registers[5][0] == 0.f);
  CHECK(vm.arenas[0].floatVec[2 * 16] == 0.f);
  CHECK(!vm.flag);
}

void testVMOutOfRangeArenaIsSafe() {
  using namespace mlvm;
  // chunk 255 of a 4-float arena cannot hold a 16-float block: must not crash or write.
  auto r = assemble(R"(
    ARENA tiny 4
    LOAD_I r0, 1.0f
    STORE  r0, tiny/255
    LOAD   r1, tiny/255
    LOAD   r2, 7/0           // arena 7 was never declared: zero-size
    HALT
  )");
  CHECK(r.ok());
  MLVM vm;
  CHECK(vm.loadProgram(r.program));
  vm.run();
  CHECK(vm.registers[1][0] == 0.f);
  CHECK(vm.registers[2][0] == 0.f);
}

void testLoadK() {
  using namespace mlvm;
  auto r = assemble(R"(
    ARENA events 64          // one ml::ControlRow per chunk: 16 voices x 4 floats
    LOAD_K   r1, events/0    // voice 0 of row 0
    LOAD_K.3 r4, events/3    // voices 0, 1, 2 of row 3 into r4, r5, r6
    HALT
  )");
  if (!r.ok()) printErrors(r);
  CHECK(r.ok());
  if (r.program.instructions.size() != 3) { ++failures; return; }
  const auto& code = r.program.instructions;
  CHECK((code[0] == Instruction{LOAD_K, 1, 0, 0, 0, 0, 0, 0}));
  CHECK((code[1] == Instruction{LOAD_K, 4, 0, 3, 0, 3, 0, 0}));

  MLVM vm;
  CHECK(vm.loadProgram(r.program));
  CHECK_EQ(vm.eventArenaIdx, 0);
  CHECK_EQ(vm.arenas[0].floatVec.size(), 256u * 64u);

  // write ControlBlocks straight into the arena, as processBlock would.
  auto writeBlock = [&](size_t row, size_t voice, ml::ControlBlock c) {
    std::memcpy(vm.arenas[0].floatVec.data() + row * 64 + voice * 4, &c, sizeof(c));
  };
  writeBlock(0, 0, ml::ControlBlock::ramp(0.f, 1.f));
  writeBlock(3, 0, ml::ControlBlock::ramp(2.f, 2.f));     // flat
  writeBlock(3, 1, ml::ControlBlock::ramp(-1.f, 0.f));
  writeBlock(3, 2, ml::ControlBlock::step(-1.f, 5.f, 6));  // step at frame 6
  vm.run();

  const size_t last = ml::kFramesPerBlock - 1;
  CHECK(vm.registers[1][last] == 1.f);
  CHECK(std::fabs(vm.registers[1][0] - 1.f / ml::kFramesPerBlock) < 1e-6f);
  for (size_t i = 1; i < ml::kFramesPerBlock; ++i) CHECK(vm.registers[1][i] > vm.registers[1][i - 1]);
  for (size_t i = 0; i < ml::kFramesPerBlock; ++i) CHECK(vm.registers[4][i] == 2.f);
  CHECK(vm.registers[5][last] == 0.f);
  CHECK(std::fabs(vm.registers[5][0] - (-1.f + 1.f / ml::kFramesPerBlock)) < 1e-6f);
  for (size_t i = 0; i < 6; ++i) CHECK(vm.registers[6][i] == -1.f);
  for (size_t i = 6; i < ml::kFramesPerBlock; ++i) CHECK(vm.registers[6][i] == 5.f);

  // an events arena with the wrong chunk size is refused
  auto bad = assemble("ARENA events 32\nHALT\n");
  CHECK(bad.ok());
  MLVM vm2;
  CHECK(!vm2.loadProgram(bad.program));

  // a program without an events arena has no event arena index
  auto none = assemble("ARENA scratch 32\nHALT\n");
  MLVM vm3;
  CHECK(vm3.loadProgram(none.program));
  CHECK_EQ(vm3.eventArenaIdx, -1);
}

} // namespace

int main() {
  testOpcodeTable();
  testSketchProgram();
  testErrors();
  testVMArithmetic();
  testVMArenaAndBranch();
  testVMOutOfRangeArenaIsSafe();
  testLoadK();

  if (failures == 0) {
    std::printf("mlvm tests: all passed\n");
  } else {
    std::printf("mlvm tests: %d failure(s)\n", failures);
  }
  return failures;
}
