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

void testControlRate() {
  using namespace mlvm;
  using ml::kFramesPerBlock;
  const size_t last = kFramesPerBlock - 1;

  // rows follow ml::ControlSignals: 0 pitch, 1 gate, 9 bend. kstate chunks: 0 gate
  // level, 1 sample-and-hold (32 floats), 2 ramp state.
  auto r = assemble(R"(
    ARENA events 16
    ARENA kstate 32
    LOAD_G  r1, events/1, kstate/0        // gate -> 0/1 block
    LOAD_KS r2, r1, events/0, kstate/1    // pitch, stepping at the gate's rise
    LOAD_K  r3, events/9, kstate/2        // bend, ramped
    HALT
  )");
  if (!r.ok()) printErrors(r);
  CHECK(r.ok());
  if (r.program.instructions.size() != 4) { ++failures; return; }
  const auto& code = r.program.instructions;
  CHECK((code[0] == Instruction{LOAD_G, 1, 0, 0, 0, 1, 1, 0}));
  CHECK((code[1] == Instruction{LOAD_KS, 2, 1, 0, 0, 0, 1, 1}));
  CHECK((code[2] == Instruction{LOAD_K, 3, 0, 0, 0, 9, 1, 2}));

  MLVM vm;
  CHECK(vm.loadProgram(r.program));
  CHECK_EQ(vm.eventArenaIdx, 0);
  CHECK_EQ(vm.arenas[0].floatVec.size(), 256u * 16u);

  auto set = [&](size_t row, size_t voice, float value) { vm.arenas[0].floatVec[row * 16 + voice] = value; };
  auto allEqual = [&](size_t reg, float value) {
    for (size_t i = 0; i < kFramesPerBlock; ++i) if (vm.registers[reg][i] != value) return false;
    return true;
  };

  // run 1: gate rises at frame 4; pitch 1.0; bend 0.5. All state starts at zero.
  set(1, 0, ml::encodeGateTransition(true, 4));
  set(0, 0, 1.f);
  set(9, 0, 0.5f);
  vm.run();
  for (size_t i = 0; i < 4; ++i) CHECK(vm.registers[1][i] == 0.f);
  for (size_t i = 4; i < kFramesPerBlock; ++i) CHECK(vm.registers[1][i] == 1.f);
  for (size_t i = 0; i < 4; ++i) CHECK(vm.registers[2][i] == 0.f);     // held previous value
  for (size_t i = 4; i < kFramesPerBlock; ++i) CHECK(vm.registers[2][i] == 1.f);  // stepped at the rise
  CHECK(vm.registers[3][last] == 0.5f);                                 // ramp lands on the value
  CHECK(std::fabs(vm.registers[3][0] - 0.5f / kFramesPerBlock) < 1e-6f);

  // run 2: no transition, same values. Gate stays high, pitch flat, bend flat.
  set(1, 0, ml::kNoGateTransition);
  vm.run();
  CHECK(allEqual(1, 1.f));
  CHECK(allEqual(2, 1.f));
  CHECK(allEqual(3, 0.5f));

  // run 3: gate falls at frame 2; pitch moves to 2.0 with no rise, so it ramps.
  set(1, 0, ml::encodeGateTransition(false, 2));
  set(0, 0, 2.f);
  vm.run();
  for (size_t i = 0; i < 2; ++i) CHECK(vm.registers[1][i] == 1.f);
  for (size_t i = 2; i < kFramesPerBlock; ++i) CHECK(vm.registers[1][i] == 0.f);
  CHECK(vm.registers[2][last] == 2.f);
  CHECK(vm.registers[2][0] > 1.f);
  CHECK(vm.registers[2][0] < 2.f);

  // run 4: gate rises at frame 0; pitch 3.0. The rise is detected against the stored
  // gate level, so the sample-and-hold steps at frame 0.
  set(1, 0, ml::encodeGateTransition(true, 0));
  set(0, 0, 3.f);
  vm.run();
  CHECK(allEqual(1, 1.f));
  CHECK(allEqual(2, 3.f));

  // bank of two voices: LOAD_G.2 fills r1 and r2 from voices 0 and 1
  auto rBank = assemble("ARENA events 16\nARENA kstate 16\nLOAD_G.2 r1, events/1, kstate/0\nHALT\n");
  CHECK(rBank.ok());
  MLVM vmBank;
  CHECK(vmBank.loadProgram(rBank.program));
  vmBank.arenas[0].floatVec[1 * 16 + 0] = ml::encodeGateTransition(true, 0);
  vmBank.arenas[0].floatVec[1 * 16 + 1] = ml::encodeGateTransition(true, 10);
  vmBank.run();
  for (size_t i = 0; i < kFramesPerBlock; ++i) CHECK(vmBank.registers[1][i] == 1.f);
  for (size_t i = 0; i < 10; ++i) CHECK(vmBank.registers[2][i] == 0.f);
  for (size_t i = 10; i < kFramesPerBlock; ++i) CHECK(vmBank.registers[2][i] == 1.f);

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
  testControlRate();

  if (failures == 0) {
    std::printf("mlvm tests: all passed\n");
  } else {
    std::printf("mlvm tests: %d failure(s)\n", failures);
  }
  return failures;
}
