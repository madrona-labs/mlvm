// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com
//
// Tests for the assembler and the VM instruction loop. No framework: each
// check prints on failure and the process exit code is the failure count.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

const size_t kFrames = ml::kFramesPerBlock;
const std::string kFramesStr = std::to_string(ml::kFramesPerBlock);

bool allFramesEqual(const ml::SignalBlock& b, float value) {
  for (size_t i = 0; i < kFrames; ++i) if (b[i] != value) return false;
  return true;
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
  CHECK(!r.program.manifest[3].external);
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
    "EXTERN audioin\n"           // 9: EXTERN needs a chunk size
    "HALT\n";                    // 10

  auto r = assemble(src);
  CHECK(!r.ok());
  CHECK_EQ(r.errors.size(), 8u);
  for (int line : {2, 3, 4, 5, 6, 7, 8, 9}) {
    if (!hasErrorOnLine(r, line)) { ++failures; std::printf("FAIL: expected an error on line %d\n", line); }
  }
  if (r.errors.size() != 8) printErrors(r);
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
  CHECK(allFramesEqual(vm.registers[0], 5.f));
  CHECK(allFramesEqual(vm.registers[3], -5.f));
  CHECK(allFramesEqual(vm.registers[4], 4.f));
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
  CHECK_EQ(vm.arenas[0].size(), 256u * 16u);
  vm.run();
  CHECK(vm.registers[0][0] == 0.f);
  CHECK(vm.registers[5][0] == 0.f);
  CHECK(vm.arenas[0].data()[2 * 16] == 0.f);
  CHECK(!vm.flag);
}

void testVMOutOfRangeArenaIsSafe() {
  using namespace mlvm;
  // chunk 255 of a 4-float arena cannot hold a whole block: must not crash or write.
  auto r = assemble(R"(
    ARENA tiny 4
    LOAD_I r0, 1.0f
    STORE  r0, tiny/255
    LOAD   r1, tiny/255
    LOAD   r2, 7/0           // arena 7 was never declared: no memory
    HALT
  )");
  CHECK(r.ok());
  MLVM vm;
  CHECK(vm.loadProgram(r.program));
  vm.run();
  CHECK(vm.registers[1][0] == 0.f);
  CHECK(vm.registers[2][0] == 0.f);
}

void testExternArenas() {
  using namespace mlvm;

  // parse and disassemble
  std::string src =
    "EXTERN audioin " + kFramesStr + "\n"
    "EXTERN audioout " + kFramesStr + "\n"
    "LOAD   r1, audioin/0\n"
    "NEG    r2, r1\n"
    "STORE  r1, audioout/0\n"
    "STORE  r2, audioout/1\n"
    "STORE  r2, audioin/1      // read-only: must be ignored\n"
    "HALT\n";
  auto r = assemble(src);
  if (!r.ok()) printErrors(r);
  CHECK(r.ok());
  CHECK_EQ(r.program.manifest.size(), 2u);
  CHECK(r.program.manifest[0].external);
  CHECK(r.program.manifest[1].external);
  CHECK(disassemble(r.program).find("EXTERN audioin " + kFramesStr) != std::string::npos);

  MLVM vm;
  CHECK(vm.loadProgram(r.program));
  CHECK_EQ(vm.externals.size(), 2u);
  CHECK(vm.arenas[0].external);
  CHECK(!vm.arenas[0].writable);

  // two channels in, two out, as the host would hand us
  std::vector<float> in(2 * kFrames, 0.f), out(2 * kFrames, 0.f);
  for (size_t i = 0; i < kFrames; ++i) { in[i] = 0.5f; in[kFrames + i] = 0.25f; }
  CHECK(vm.bindArena(0, in.data(), in.size(), false));
  CHECK(vm.bindArena(1, out.data(), out.size(), true));
  vm.run();
  for (size_t i = 0; i < kFrames; ++i) {
    CHECK(out[i] == 0.5f);               // channel 0: pass through
    CHECK(out[kFrames + i] == -0.5f);    // channel 1: negated
    CHECK(in[kFrames + i] == 0.25f);     // read-only input untouched by the STORE
  }
  vm.unbindArenas();
  CHECK(vm.arenas[0].data() == nullptr);

  // unbound externals are inert: registers keep their values, nothing is written
  vm.registers[1] = ml::SignalBlock(7.f);
  vm.run();
  CHECK(allFramesEqual(vm.registers[1], 7.f));
  CHECK(out[0] == 0.5f);

  // binding a non-external arena is refused
  auto owned = assemble("ARENA scratch 16\nHALT\n");
  MLVM vm2;
  CHECK(vm2.loadProgram(owned.program));
  float dummy[16] = {};
  CHECK(!vm2.bindArena(0, dummy, 16, false));

  // unknown external names and wrong chunk sizes are refused at load
  MLVM vm3;
  CHECK(!vm3.loadProgram(assemble("EXTERN bogus 16\nHALT\n").program));
  CHECK(!vm3.loadProgram(assemble("EXTERN events0 32\nHALT\n").program));
  CHECK(!vm3.loadProgram(assemble("EXTERN ctl0 16\nHALT\n").program));
  CHECK(!vm3.loadProgram(assemble("EXTERN audioin 7\nHALT\n").program));
  CHECK(vm3.loadProgram(assemble("EXTERN events7 16\nEXTERN ctl3 1\nHALT\n").program));
  CHECK_EQ(vm3.externals.size(), 2u);
  CHECK(vm3.externals[0].kind == MLVM::HostInput::events);
  CHECK_EQ(vm3.externals[0].channel, 7);
  CHECK(vm3.externals[1].kind == MLVM::HostInput::controllers);
  CHECK_EQ(vm3.externals[1].channel, 3);
}

void testControllers() {
  using namespace mlvm;
  auto r = assemble(R"(
    EXTERN ctl0 1
    ARENA  kstate 16
    LOAD_K r1, ctl0/74, kstate/0      // one controller, ramped from the previous value
    LOAD_K r2, ctl0/127, kstate/1     // the last controller is addressable
    HALT
  )");
  if (!r.ok()) printErrors(r);
  CHECK(r.ok());

  MLVM vm;
  CHECK(vm.loadProgram(r.program));
  std::vector<float> ctl(MLVM::kNumControllers, 0.f);
  ctl[74] = 1.f;
  ctl[127] = 0.5f;
  CHECK(vm.bindArena(0, ctl.data(), ctl.size(), false));

  // from zero state the value ramps over kControlRampFrames frames, then holds.
  vm.run();
  const size_t ramp = std::min(MLVM::kControlRampFrames, kFrames);
  CHECK(std::fabs(vm.registers[1][0] - 1.f / ramp) < 1e-6f);
  for (size_t i = 1; i < ramp; ++i) CHECK(vm.registers[1][i] > vm.registers[1][i - 1]);
  CHECK(vm.registers[1][ramp - 1] == 1.f);
  for (size_t i = ramp; i < kFrames; ++i) CHECK(vm.registers[1][i] == 1.f);
  CHECK(vm.registers[2][ramp - 1] == 0.5f);
  CHECK(vm.registers[2][kFrames - 1] == 0.5f);

  // unchanged value: flat
  vm.run();
  CHECK(allFramesEqual(vm.registers[1], 1.f));
  vm.unbindArenas();
}

void testControlRate() {
  using namespace mlvm;

  // rows follow ml::ControlSignals: 0 pitch, 1 gate, 9 bend. kstate chunks: 0 gate
  // level, 1 sample-and-hold (32 floats), 2 ramp state.
  auto r = assemble(R"(
    EXTERN events0 16
    ARENA  kstate 32
    LOAD_G  r1, events0/1, kstate/0        // gate -> 0/1 block
    LOAD_KS r2, r1, events0/0, kstate/1    // pitch, stepping at the gate's rise
    LOAD_K  r3, events0/9, kstate/2        // bend, ramped
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

  // a VoiceControls-shaped buffer: 10 rows of 16 voices
  std::vector<float> events(MLVM::kEventRows * MLVM::kEventVoices, 0.f);
  CHECK(vm.bindArena(0, events.data(), events.size(), false));
  auto set = [&](size_t row, size_t voice, float value) { events[row * 16 + voice] = value; };

  const size_t ramp = std::min(MLVM::kControlRampFrames, kFrames);

  // run 1: gate rises at frame 4; pitch 1.0; bend 0.5. All state starts at zero.
  set(1, 0, ml::encodeGateTransition(true, 4));
  set(0, 0, 1.f);
  set(9, 0, 0.5f);
  vm.run();
  for (size_t i = 0; i < 4; ++i) CHECK(vm.registers[1][i] == 0.f);
  for (size_t i = 4; i < kFrames; ++i) CHECK(vm.registers[1][i] == 1.f);
  for (size_t i = 0; i < 4; ++i) CHECK(vm.registers[2][i] == 0.f);     // held previous value
  for (size_t i = 4; i < kFrames; ++i) CHECK(vm.registers[2][i] == 1.f);  // stepped at the rise
  CHECK(vm.registers[3][ramp - 1] == 0.5f);                             // ramp lands on the value
  CHECK(vm.registers[3][kFrames - 1] == 0.5f);                          // and holds
  CHECK(std::fabs(vm.registers[3][0] - 0.5f / ramp) < 1e-6f);

  // run 2: no transition, same values. Gate stays high, pitch flat, bend flat.
  set(1, 0, ml::kNoGateTransition);
  vm.run();
  CHECK(allFramesEqual(vm.registers[1], 1.f));
  CHECK(allFramesEqual(vm.registers[2], 1.f));
  CHECK(allFramesEqual(vm.registers[3], 0.5f));

  // run 3: gate falls at frame 2; pitch moves to 2.0 with no rise, so it ramps.
  set(1, 0, ml::encodeGateTransition(false, 2));
  set(0, 0, 2.f);
  vm.run();
  for (size_t i = 0; i < 2; ++i) CHECK(vm.registers[1][i] == 1.f);
  for (size_t i = 2; i < kFrames; ++i) CHECK(vm.registers[1][i] == 0.f);
  CHECK(vm.registers[2][ramp - 1] == 2.f);
  CHECK(vm.registers[2][0] > 1.f);
  CHECK(vm.registers[2][0] < 2.f);

  // run 4: gate rises at frame 0; pitch 3.0. The rise is detected against the stored
  // gate level, so the sample-and-hold steps at frame 0.
  set(1, 0, ml::encodeGateTransition(true, 0));
  set(0, 0, 3.f);
  vm.run();
  CHECK(allFramesEqual(vm.registers[1], 1.f));
  CHECK(allFramesEqual(vm.registers[2], 3.f));
  vm.unbindArenas();

}

// ---- banks

void testBankErrors() {
  using namespace mlvm;
  auto r = assemble(
    "ADD.3 r0, r1, r2\n"        // 1: not a multiple of 4
    "ADD.5 r0, r1, r2\n"        // 2: not a multiple of 4
    "ADD.8 r250, r1, r2\n"      // 3: bank runs past r255
    "ADD.8 r0, r252, r2\n"      // 4: source bank runs past r255
    "ADD.4 r0, r1, r2\n"        // 5: fine
    "ADD.252 r0, r1, r2\n"      // 6: fine, exactly fits
    "HALT\n");
  CHECK(!r.ok());
  CHECK_EQ(r.errors.size(), 4u);
  for (int line : {1, 2, 3, 4}) {
    if (!hasErrorOnLine(r, line)) { ++failures; std::printf("FAIL: expected an error on line %d\n", line); }
  }
  if (r.errors.size() != 4) printErrors(r);
}

void testBankLayout() {
  using namespace mlvm;
  // 8 horizontal voices in r0..r7, distinct ramps; BANK.8 into r16..r23 must match
  // madronalib's horizontalToVertical byte for byte; UNBANK.8 into r32 round-trips.
  auto r = assemble("BANK.8 r16, r0\nUNBANK.8 r32, r16\nHALT\n");
  if (!r.ok()) printErrors(r);
  CHECK(r.ok());
  MLVM vm;
  CHECK(vm.loadProgram(r.program));

  ml::SignalBlockArray<8> h;
  for (size_t v = 0; v < 8; ++v) {
    for (size_t i = 0; i < kFrames; ++i) vm.registers[v][i] = static_cast<float>(v * 1000 + i);
    h.setRow(v, vm.registers[v]);
  }
  vm.run();

  ml::SignalBlockArrayBase<float4, 2> expected = ml::horizontalToVertical<2>(h);
  CHECK(std::memcmp(vm.registers[16].data(), expected.data(), 8 * kFrames * sizeof(float)) == 0);

  // voice v, frame i sits at lane v%4 of float4 frame i of group v/4
  CHECK(vm.registers[16][0 * 4 + 1] == 1000.f);          // voice 1, frame 0
  CHECK(vm.registers[16][5 * 4 + 2] == 2005.f);          // voice 2, frame 5
  CHECK(vm.registers[20][3 * 4 + 0] == 4003.f);          // voice 4, frame 3 (group 1)

  for (size_t v = 0; v < 8; ++v) CHECK(vm.registers[32 + v] == vm.registers[v]);
}

void testElementwiseBank() {
  using namespace mlvm;
  auto r = assemble("ADD.8 r0, r16, r24\nNEG.4 r40, r16\nHALT\n");
  CHECK(r.ok());
  MLVM vm;
  CHECK(vm.loadProgram(r.program));
  for (size_t k = 0; k < 8; ++k) {
    vm.registers[16 + k] = ml::SignalBlock(static_cast<float>(k));
    vm.registers[24 + k] = ml::SignalBlock(100.f);
  }
  vm.run();
  for (size_t k = 0; k < 8; ++k) CHECK(allFramesEqual(vm.registers[k], 100.f + k));
  for (size_t k = 0; k < 4; ++k) CHECK(allFramesEqual(vm.registers[40 + k], -static_cast<float>(k)));
  CHECK(allFramesEqual(vm.registers[44], 0.f));   // untouched beyond the bank
}

void testVerticalControl() {
  using namespace mlvm;
  // a 16-voice bank of pitches ramps per voice; UNBANK lets us check each voice.
  auto r = assemble(R"(
    EXTERN events0 16
    ARENA  kstate 32
    LOAD_K.16  r16, events0/0, kstate/0      // 16 pitches -> vertical bank r16..r31
    UNBANK.16  r32, r16                      // -> horizontal voices r32..r47
    LOAD_G.8   r48, events0/1, kstate/1      // 8 gates -> vertical bank r48..r55
    UNBANK.8   r56, r48
    LOAD_KS.4  r64, r48, events0/2, kstate/2 // 4 velocities stepping at their gates
    UNBANK.4   r68, r64
    HALT
  )");
  if (!r.ok()) printErrors(r);
  CHECK(r.ok());
  MLVM vm;
  CHECK(vm.loadProgram(r.program));

  std::vector<float> events(MLVM::kEventRows * MLVM::kEventVoices, 0.f);
  CHECK(vm.bindArena(0, events.data(), events.size(), false));
  for (size_t v = 0; v < 16; ++v) events[0 * 16 + v] = static_cast<float>(v + 1);       // pitch
  for (size_t v = 0; v < 8; ++v) events[1 * 16 + v] = ml::encodeGateTransition(true, v * 2); // gate rises at 0,2,4..
  for (size_t v = 0; v < 4; ++v) events[2 * 16 + v] = 0.5f + 0.1f * v;                   // velocity

  const size_t ramp = std::min(MLVM::kControlRampFrames, kFrames);
  vm.run();

  // pitch: each voice ramps from 0 to v+1 over `ramp` frames, then holds
  for (size_t v = 0; v < 16; ++v) {
    const ml::SignalBlock& p = vm.registers[32 + v];
    const float target = static_cast<float>(v + 1);
    CHECK(std::fabs(p[0] - target / ramp) < 1e-5f);
    CHECK(p[ramp - 1] == target);
    CHECK(p[kFrames - 1] == target);
  }
  // gates: voice v rises at frame 2v
  for (size_t v = 0; v < 8; ++v) {
    const ml::SignalBlock& g = vm.registers[56 + v];
    for (size_t i = 0; i < kFrames; ++i) CHECK(g[i] == (i >= 2 * v ? 1.f : 0.f));
  }
  // velocities: held at 0 (zero state) until each voice's gate rise, then the value
  for (size_t v = 0; v < 4; ++v) {
    const ml::SignalBlock& vel = vm.registers[68 + v];
    const float value = 0.5f + 0.1f * v;
    for (size_t i = 0; i < kFrames; ++i) CHECK(vel[i] == (i >= 2 * v ? value : 0.f));
  }

  // second block: no gate transitions, pitches unchanged -> flat; velocities flat
  for (size_t v = 0; v < 8; ++v) events[1 * 16 + v] = ml::kNoGateTransition;
  vm.run();
  for (size_t v = 0; v < 16; ++v) CHECK(allFramesEqual(vm.registers[32 + v], static_cast<float>(v + 1)));
  for (size_t v = 0; v < 8; ++v) CHECK(allFramesEqual(vm.registers[56 + v], 1.f));
  for (size_t v = 0; v < 4; ++v) CHECK(allFramesEqual(vm.registers[68 + v], 0.5f + 0.1f * v));

  // a 32-voice request on a 16-voice row is refused (registers untouched)
  auto big = assemble("EXTERN events0 16\nARENA kstate 32\nLOAD_K.32 r16, events0/0, kstate/0\nHALT\n");
  CHECK(big.ok());
  MLVM vm2;
  CHECK(vm2.loadProgram(big.program));
  CHECK(vm2.bindArena(0, events.data(), events.size(), false));
  vm2.registers[16] = ml::SignalBlock(9.f);
  vm2.run();
  CHECK(allFramesEqual(vm2.registers[16], 9.f));
}

void testBroadcastAndMix() {
  using namespace mlvm;
  auto r = assemble(R"(
    EXTERN ctl0 1
    ARENA  kstate 16
    LOAD_K.16  r16, ctl0/1, kstate/0     // one controller broadcast to 16 voices
    SPREAD.16  r32, r1                   // horizontal r1 to all 16 voices
    MUL.16     r48, r16, r32
    MIX.16     r8, r48                   // sum of 16 voices
    HALT
  )");
  if (!r.ok()) printErrors(r);
  CHECK(r.ok());
  MLVM vm;
  CHECK(vm.loadProgram(r.program));
  std::vector<float> ctl(MLVM::kNumControllers, 0.f);
  ctl[1] = 0.5f;
  CHECK(vm.bindArena(0, ctl.data(), ctl.size(), false));
  for (size_t i = 0; i < kFrames; ++i) vm.registers[1][i] = static_cast<float>(i);

  const size_t ramp = std::min(MLVM::kControlRampFrames, kFrames);
  vm.run();
  // after the ramp every voice holds 0.5 * i, so the mix is 16 * 0.5 * i = 8i
  for (size_t i = ramp; i < kFrames; ++i) CHECK(std::fabs(vm.registers[8][i] - 8.f * i) < 1e-3f);
  // during the ramp the controller is at 0.5 * (i+1)/ramp in every lane
  CHECK(std::fabs(vm.registers[8][0] - 16.f * (0.5f / ramp) * 0.f) < 1e-5f);
  CHECK(std::fabs(vm.registers[8][1] - 16.f * (0.5f * 2.f / ramp) * 1.f) < 1e-4f);
  // SPREAD put frame i of r1 in all four lanes of every group
  CHECK(vm.registers[32][3 * 4 + 0] == 3.f);
  CHECK(vm.registers[32][3 * 4 + 3] == 3.f);
  CHECK(vm.registers[44][7 * 4 + 2] == 7.f);
}

} // namespace

int main() {
  testOpcodeTable();
  testSketchProgram();
  testErrors();
  testVMArithmetic();
  testVMArenaAndBranch();
  testVMOutOfRangeArenaIsSafe();
  testExternArenas();
  testControllers();
  testControlRate();
  testBankErrors();
  testBankLayout();
  testElementwiseBank();
  testVerticalControl();
  testBroadcastAndMix();

  if (failures == 0) {
    std::printf("mlvm tests: all passed\n");
  } else {
    std::printf("mlvm tests: %d failure(s)\n", failures);
  }
  return failures;
}
