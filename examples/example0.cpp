// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com

// Example0: an audible test that needs no MIDI. Sixteen sine voices on a pentatonic
// scale from 110 Hz, each with its own slow sine LFO on its gain. The LFO rates run
// from 0.2 Hz down to 0.1 Hz, so the voices drift in and out of phase and the chord
// sweeps. The voices are summed and centered in a stereo output.
//
// The per-voice constants are DATA lines in the program: the program's data section.
// Pitches are in octaves from A440. LOAD_K.16 turns a 16-float row into a bank.

#include <iostream>
#include <string>

#include "madronalib.h"
#include "mlvm.h"
#include "assembler.h"

using mlvm::MLVM;

constexpr int kInputChannels = 0;
constexpr int kOutputChannels = 2;

const std::string kProgram =
  "EXTERN audioout " + std::to_string(kFramesPerBlock) + "\n" + R"(
  ARENA  consts 16
  ARENA  kstate 16

  // voice pitches: major pentatonic from 110 Hz (semitones 0 2 4 7 9, three octaves)
  DATA   consts/0 \
    -2 -1.8333 -1.6667 -1.4167 -1.25 -1 -0.8333 -0.6667 \
    -0.4167 -0.25 0 0.1667 0.3333 0.5833 0.75 1

  // LFO pitches: 0.2 Hz down to 0.1 Hz, evenly spaced
  DATA   consts/1 \
    -11.103 -11.152 -11.203 -11.255 -11.310 -11.366 -11.425 -11.487 \
    -11.550 -11.618 -11.689 -11.762 -11.840 -11.923 -12.010 -12.103

  LOAD_K.16  r16, consts/0, kstate/0     // voice pitches -> bank r16..r31
  LOAD_K.16  r32, consts/1, kstate/1     // lfo pitches   -> bank r32..r47
  SINE.16    r48, r16, kstate/2          // oscillators
  SINE.16    r64, r32, kstate/3          // lfos, -1..1
  LOAD_I.16  r80, 1.0f
  ADD.16     r64, r64, r80               // 0..2
  LOAD_I.16  r80, 0.5f
  MUL.16     r64, r64, r80               // gain 0..1
  MUL.16     r48, r48, r64               // voice = osc * gain
  MIX.16     r8, r48                     // sum the voices
  LOAD_I     r9, 0.03f                   // about 0.5 / 16
  MUL        r8, r8, r9
  STORE      r8, audioout/0              // centered stereo
  STORE      r8, audioout/1
  HALT
)";

void processAudio(AudioContext* context, MLVM* vm)
{
  vm->processBlock(context);
}

// Logs from a separate thread so nothing prints on the audio thread.
struct Logger
{
  Logger(AudioTask* t, AudioContext* c, MLVM* v) : task(t), context(c), vm(v)
  {
    logThread = std::thread([&]() {
      while (!task->hasQuit())
      {
        float peak = 0.f;
        for (size_t i = 0; i < kFramesPerBlock; ++i) peak = std::max(peak, std::fabs(context->outputs[0][i]));
        // r64 is the gain bank; its first float is voice 0's gain at frame 0.
        std::cout << "output peak: " << peak << "  voice 0 gain: " << vm->registers[64][0] << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      }
    });
  }
  ~Logger() { logThread.join(); }

  AudioTask* task;
  AudioContext* context;
  MLVM* vm;
  std::thread logThread;
};

int main(int, char*[])
{
  // start the Timers. call this once in an application.
  bool deferToMainThread = false;
  SharedResourcePointer<ml::Timers> t;
  t->start(deferToMainThread);

  mlvm::AssemblyResult assembled = mlvm::assemble(kProgram);
  if (!assembled.ok()) {
    for (const auto& e : assembled.errors) std::cout << "line " << e.line << ": " << e.message << "\n";
    return 1;
  }
  std::cout << mlvm::disassemble(assembled.program);

  MLVM vm;
  if (!vm.loadProgram(assembled.program)) {
    std::cout << "couldn't load program\n";
    return 1;
  }

  AudioContext ctx(kInputChannels, kOutputChannels);
  AudioTask exampleTask(&ctx, processAudio, &vm);
  Logger log(&exampleTask, &ctx, &vm);
  ctx.updateTime(0, 120.0, true);

  return exampleTask.runConsoleApp();
}
