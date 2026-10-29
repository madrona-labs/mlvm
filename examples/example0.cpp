// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com

// Example0: an audible test that needs no MIDI. Sixteen sine voices on a pentatonic
// scale from 110 Hz, each with its own slow sine LFO on its gain. The LFO rates run
// from 0.2 Hz down to 0.1 Hz, so the voices drift in and out of phase and the chord
// sweeps. The voices are summed and centered in a stereo output.
//
// The per-voice constants are tables in this file, in Hz for readability, converted
// to mlvm's pitch unit (octaves, 0 = A440) and written into the program as DATA lines:
// the program's data section. LOAD_K.16 turns a 16-float row into a vertical bank.

#include <array>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>

#include "madronalib.h"
#include "mlvm.h"
#include "assembler.h"

using mlvm::MLVM;

constexpr int kInputChannels = 0;
constexpr int kOutputChannels = 2;
constexpr int kSampleRate = 48000;
constexpr size_t kVoices = 16;

// ---- the constant tables

// voice pitches: major pentatonic from 110 Hz, semitones 0 2 4 7 9 and up three octaves
constexpr std::array<float, kVoices> kVoiceHz = {
  110.00f, 123.47f, 138.59f, 164.81f, 185.00f,
  220.00f, 246.94f, 277.18f, 329.63f, 369.99f,
  440.00f, 493.88f, 554.37f, 659.26f, 739.99f,
  880.00f
};

// LFO rates: evenly spaced from 0.2 Hz down to 0.1 Hz, one per voice
constexpr std::array<float, kVoices> kLfoHz = {
  0.2000f, 0.1933f, 0.1867f, 0.1800f, 0.1733f,
  0.1667f, 0.1600f, 0.1533f, 0.1467f, 0.1400f,
  0.1333f, 0.1267f, 0.1200f, 0.1133f, 0.1067f,
  0.1000f
};

// mlvm pitch is log2(frequency / 440)
float pitchFromHz(float hz) { return std::log2(hz / 440.f); }

// a DATA line: "DATA name/chunk v0 v1 ..." with the table converted to pitch
std::string pitchDataLine(const std::string& arena, int chunk, const std::array<float, kVoices>& hzTable)
{
  std::ostringstream line;
  line << "DATA " << arena << "/" << chunk;
  for (float hz : hzTable) line << " " << pitchFromHz(hz);
  return line.str();
}

// ---- the program

std::string makeProgram()
{
  return
    "EXTERN audioout " + std::to_string(kFramesPerBlock) + "\n"
    "ARENA  consts 16\n"
    "ARENA  kstate 16\n" +
    pitchDataLine("consts", 0, kVoiceHz) + "\n" +
    pitchDataLine("consts", 1, kLfoHz) + "\n" + R"(
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
}

// ---- host plumbing

struct ExampleState {
  MLVM* vm{nullptr};
};

void processAudio(AudioContext* context, ExampleState* state)
{
  state->vm->processBlock(context);
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

  mlvm::AssemblyResult assembled = mlvm::assemble(makeProgram());
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

  ExampleState state{&vm};
  AudioContext ctx(kInputChannels, kOutputChannels);
  AudioTask exampleTask(&ctx, processAudio, &state);
  Logger log(&exampleTask, &ctx, &vm);
  ctx.updateTime(0, 120.0, true, kSampleRate);

  return exampleTask.runConsoleApp();
}
