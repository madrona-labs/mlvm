// mlvm
// Copyright (c) 2025 Madrona Labs LLC. http://www.madronalabs.com

// Simple Synth: a 16-voice sawtooth through a lowpass SVF, played from MIDI.
// Each voice's filter sits at the note's pitch and the mod wheel (CC 1) opens it
// upward by up to four octaves. Voices are summed and centered in a stereo output.
//
// Everything the host provides arrives as external arenas bound by name each block:
// events0 (channel 0's voice rows), ctl0 (its 128 controllers) and audioout.

#include <iostream>
#include <string>

#include "madronalib.h"
#include "mlvm.h"
#include "assembler.h"

using mlvm::MLVM;

constexpr int kInputChannels = 0;
constexpr int kOutputChannels = 2;
constexpr int kSampleRate = 48000;
constexpr int kVoices = 16;

struct SynthState {
  MLVM* vm{nullptr};
};

void processAudio(AudioContext* context, SynthState* state)
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
        std::cout << "voice 0 gate: " << vm->registers[16][0]
                  << "  mod wheel: " << context->getInputControllerValues(0)[1]
                  << "  output peak: " << peak << "\n";
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

int main(int argc, char* argv[])
{
  // start the Timers. call this once in an application.
  bool deferToMainThread = false;
  SharedResourcePointer<ml::Timers> t;
  t->start(deferToMainThread);

  AudioContext ctx(kInputChannels, kOutputChannels);
  ctx.setInputPolyphony(kVoices);

  // MIDI goes straight into the context's event channel 0.
  const auto& handleMIDI = [&](MIDIMessage m) -> void
  {
    ctx.addInputEvent(MIDIMessageToEvent(m));
  };
  MIDIInput midiInput;
  if (!midiInput.start(handleMIDI)) {
    std::cout << "couldn't start MIDI input!\n";
    return 1;
  }

  // the synth, as an mlvm program. Registers r16.. hold 16-voice banks.
  const std::string program =
    "EXTERN audioout " + std::to_string(kFramesPerBlock) + "\n" + R"(
    EXTERN events0 16
    EXTERN ctl0 1
    ARENA  kstate 32

    LOAD_G.16   r16, events0/1, kstate/0        // gates
    LOAD_KS.16  r32, r16, events0/0, kstate/1   // pitch, stepping at note onsets
    LOAD_KS.16  r48, r16, events0/2, kstate/2   // velocity
    LOAD_K.16   r64, ctl0/1, kstate/3           // mod wheel, broadcast to every voice

    SAW.16      r80, r32, kstate/4              // sawtooth at the note's pitch

    LOAD_I.16   r96, 4.0f
    MUL.16      r96, r64, r96                   // mod * 4 octaves
    ADD.16      r96, r32, r96                   // cutoff = pitch + mod * 4
    LOAD_I.16   r112, 1.0f                      // k = 1/Q
    SVF.16      r128, r80, r96, r112, kstate/5  // lowpass at the cutoff

    MUL.16      r144, r16, r48                  // gate * velocity
    LOAD_I.16   r160, 0.01f                     // 10 ms attack and release
    ONEPOLE.16  r144, r144, r160, kstate/6      // envelope
    MUL.16      r128, r128, r144                // voice out

    MIX.16      r8, r128                        // sum the voices
    LOAD_I      r9, 0.1f
    MUL         r8, r8, r9
    STORE       r8, audioout/0                  // centered stereo
    STORE       r8, audioout/1
    HALT
  )";

  mlvm::AssemblyResult assembled = mlvm::assemble(program);
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

  SynthState state{&vm};
  AudioTask synthTask(&ctx, processAudio, &state);
  Logger log(&synthTask, &ctx, &vm);
  ctx.updateTime(0, 120.0, true, kSampleRate);

  return synthTask.runConsoleApp();
}
