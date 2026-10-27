// madronalib: a C++ framework for DSP applications.
// Copyright (c) 2020-2022 Madrona Labs LLC. http://www.madronalabs.com
// Distributed under the MIT license: http://madrona-labs.mit-license.org/

// example of RtAudio wrapping low-level madronalib DSP code.

#include <iostream>
#include "madronalib.h"

// TODO figure out better private interface setup
#include "mlvm.h"
#include "assembler.h"

using mlvm::MLVM;

// TEMP
constexpr int kInputChannels = 0;
constexpr int kOutputChannels = 2;
constexpr int kSampleRate = 48000;
constexpr float kOutputGain = 0.1f;

struct VMExampleState {
  MLVM* vm{nullptr};
};

// processAudio() does all of the audio processing, in SignalBlock-sized chunks.
// It is called every time a new buffer of audio is needed.
void processAudio(AudioContext* context, VMExampleState* state)
{
  state->vm->processBlock(context);
}

struct ContextLogger
{
  ContextLogger(AudioTask* t, AudioContext* c, MLVM* v)
  {
    task = t;
    context = c;
    vm = v;
    logThread = std::thread ([&]() {
      while (!task->hasQuit())
      {
        auto timeInfo = context->getTimeInfo();
        std::cout << "bpm:" << timeInfo.bpm << "\n";
        std::cout << "phase:" << timeInfo.quarterNotesPhase_[0] << "\n";
        std::cout << "samples since start:" << timeInfo.samplesSinceStart << "\n";
        std::cout << "output 0-0:" << context->outputs[0][0] << "\n";
        // r1 is voice 0's gate; r8 is the mix of all voices' pitch * mod wheel * gate.
        std::cout << "voice 0 gate (r1):" << vm->registers[1][0]
                  << " mix (r8):" << vm->registers[8][kFramesPerBlock - 1] << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      }
    });
  }

  ~ContextLogger() { logThread.join(); }

  AudioTask* task;
  AudioContext* context;
  MLVM* vm;
  std::thread logThread;
};

int main( int argc, char *argv[] )
{
  EventsToSignals eventsToSignals;
  eventsToSignals.setSampleRate(kSampleRate);

  const auto& handleMsg = [&](MIDIMessage m)->void
  {
    std::cout << "handleMsg got " << m.size() << "bytes: ";
    for ( int i=0; i<m.size(); i++ )
      std::cout << (int)m[i] << " ";
    std::cout << "\n";
    eventsToSignals.addEvent(MIDIMessageToEvent(m));
  };

  MIDIInput midiInput;
  if (!midiInput.start(handleMsg)) {
    std::cout << "couldn't start MIDI input!\n";
  }

  // start the Timers. call this once in an application.
  bool deferToMainThread = false;
  SharedResourcePointer<ml::Timers> t;
  t->start(deferToMainThread);

  // setup the vm
  MLVM vm;

  // TODO compile a graph of modules, getting memory needs
  // auto testGraph = readJSONGraph;
  // vm.compile(testGraph, testProgram);

  // but right now we can use the assembler to write a
  // program that is like a single module.
  // Outputs are read from registers starting at kOutputRegisterBase.
  // The host provides its data as external arenas, bound by name every block; the
  // program reads only what it declares. events0 holds channel 0's voice rows (see
  // ml::ControlSignals: chunk 0 pitch, chunk 1 gate), ctl0 its 128 controller values.
  // LOAD_G expands voice 0's gate to r1; LOAD_KS steps voice 0's pitch into r2 at the
  // gate's rise; LOAD_K ramps the mod wheel into r3. Oscillators are still stubs, so
  // the outputs just pass the (absent) audio input through: silence.
  const std::string testCode =
    "EXTERN audioin " + std::to_string(kFramesPerBlock) + "\n"
    "EXTERN audioout " + std::to_string(kFramesPerBlock) + "\n" + R"(
    EXTERN events0 16
    EXTERN ctl0 1
    ARENA  kstate 32

    // a 16-voice bank: gates, pitches stepping at onsets, mod wheel broadcast to all voices
    LOAD_G.16   r16, events0/1, kstate/0         // gates   -> r16..r31 (vertical)
    LOAD_KS.16  r32, r16, events0/0, kstate/1    // pitches -> r32..r47
    LOAD_K.16   r48, ctl0/1, kstate/2            // mod wheel in every lane -> r48..r63
    MUL.16      r64, r32, r48                    // pitch * mod, per voice (stands in for a synth voice)
    MUL.16      r64, r64, r16                    // gated
    MIX.16      r8, r64                          // sum the voices
    UNBANK.4    r1, r16                          // voice 0's gate, horizontal, for the logger
    LOAD        r4, audioin/0
    STORE       r4, audioout/0
    STORE       r4, audioout/1
    HALT
  )";

  mlvm::AssemblyResult assembled = mlvm::assemble(testCode);
  if (!assembled.ok()) {
    for (const auto& e : assembled.errors) {
      std::cout << "line " << e.line << ": " << e.message << "\n";
    }
    return 1;
  }
  std::cout << mlvm::disassemble(assembled.program);

  if (!vm.loadProgram(assembled.program)) return 1;

  // fill a struct with the data the callback will need to create a context.
  VMExampleState state{&vm};
  
  AudioContext ctx(kInputChannels, kOutputChannels);
  AudioTask exampleTask(&ctx, processAudio, &state);
  ContextLogger log(&exampleTask, &ctx, &vm);
  
  // roll onward at 120 bpm
  ctx.updateTime(0, 120.0, true, kSampleRate);

  // run the audio task
  return exampleTask.runConsoleApp();
}
