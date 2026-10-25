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
        // r1 is voice 0's gate and r2 its pitch: play MIDI notes and watch them follow.
        std::cout << "voice 0 gate (r1):" << vm->registers[1][0]
                  << " pitch (r2):" << vm->registers[2][0] << "\n";
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
  // The "events" arena receives control-rate voice data from the host every block,
  // one chunk per row (see ml::ControlSignals): chunk 0 is pitch, chunk 1 is the gate.
  // LOAD_G expands voice 0's gate to r1; LOAD_KS steps voice 0's pitch into r2 at the
  // gate's rise. Oscillators are still stubs, so the outputs stay silent for now.
  const char* testCode = R"(
    ARENA events 16
    ARENA kstate 32

    LOAD_G  r1, events/1, kstate/0        // voice 0 gate, 0/1 audio rate
    LOAD_KS r2, r1, events/0, kstate/1    // voice 0 pitch, stepping at note onsets
    LOAD_I  r3, 0.0f
    MUL     r8, r2, r3                    // output 0: silence
    MOVE    r9, r8                        // output 1
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
