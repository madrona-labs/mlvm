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
  ContextLogger(AudioTask* t, AudioContext* c)
  {
    task = t;
    context = c;
    logThread = std::thread ([&]() {
      while (!task->hasQuit())
      {
        auto timeInfo = context->getTimeInfo();
        std::cout << "bpm:" << timeInfo.bpm << "\n";
        std::cout << "phase:" << timeInfo.quarterNotesPhase_[0] << "\n";
        std::cout << "samples since start:" << timeInfo.samplesSinceStart << "\n";
        std::cout << "output 0-0:" << context->outputs[0][0] << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      }
    });
  }
  
  ~ContextLogger() { logThread.join(); }
  
  AudioTask* task;
  AudioContext* context;
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
  const char* testCode = R"(
    ARENA scratch 16

    LOAD_I r1, 0.0f           // silence
    LOAD_I r2, 0.5f
    MUL    r3, r1, r2
    STORE  r3, scratch/0
    LOAD   r8, scratch/0      // output 0
    MOVE   r9, r8             // output 1
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
  ContextLogger log(&exampleTask, &ctx);
  
  // roll onward at 120 bpm
  ctx.updateTime(0, 120.0, true, kSampleRate);

  // run the audio task
  return exampleTask.runConsoleApp();
}
