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
constexpr int kVoices = 16;

void processAudio(AudioContext* context, MLVM* vm)
{
  vm->processBlock(context);
}

int main(int argc, char* argv[])
{
  // start the Timers. call this once in an application.
  bool deferToMainThread = false;
  SharedResourcePointer<ml::Timers> t;
  t->start(deferToMainThread);

  AudioContext ctx(kInputChannels, kOutputChannels);
  ctx.setInputPolyphony(kVoices);

  // MIDI goes into the context's event channel 0: its first EventsToSignals, which
  // the program reads as events0 and ctl0. The MIDI channel rides inside the Event
  // and is ignored for note allocation in the default MIDI protocol, so MPE
  // controllers play as plain polyphonic MIDI. The handler runs on the MIDI input's
  // thread, so events are posted to the context's queue rather than added directly;
  // process() moves them in on the audio thread before its next block.
  const auto& handleMIDI = [&](MIDIMessage m) -> void
  {
    ctx.postInputEvent(MIDIMessageToEvent(m));
  };

  // list the MIDI inputs and open one: port 0 by default, or the port number given
  // as the first argument.
  MIDIInput midiInput;
  const unsigned int portCount = midiInput.getPortCount();
  std::cout << portCount << " MIDI input port(s) via " << midiInput.getAPIDisplayName() << ":\n";
  for (unsigned int i = 0; i < portCount; ++i) {
    std::cout << "  " << i << ": " << midiInput.getPortName(i) << "\n";
  }
  if (portCount == 0) {
    std::cout << "no MIDI input ports found!\n";
    return 1;
  }
  const unsigned int port = (argc > 1) ? static_cast<unsigned int>(std::atoi(argv[1])) : 0;
  if (!midiInput.start(handleMIDI, port)) {
    std::cout << "couldn't start MIDI input on port " << port << "!\n";
    return 1;
  }
  std::cout << "listening on port " << port << ": " << midiInput.getPortName(port) << "\n";

  // the synth, as an mlvm program. Registers r16.. hold 16-voice banks.
  const std::string program =
    "EXTERN audioout " + std::to_string(kFramesPerBlock) + "\n" + R"(
    EXTERN events0 16
    EXTERN ctl0 1
    ARENA  kstate 32

    LOAD_G.16   r16, events0/1, kstate/0        // gates
    LOAD_KS.16  r32, r16, events0/0, kstate/1   // note number, stepping at note onsets
    LOAD_I.16   r96, -69.0f
    ADD.16      r32, r32, r96                   // semitones from A440
    LOAD_I.16   r96, 0.0833333f
    MUL.16      r32, r32, r96                   // octaves: SAW's pitch unit, 0 = A440
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

  AudioTask synthTask(&ctx, processAudio, &vm);
  ctx.updateTime(0, 120.0, true);

  // log once a second from the Timers thread, never from the audio thread.
  Timer logTimer;
  logTimer.start([&]() {
    float peak = 0.f;
    for (size_t i = 0; i < kFramesPerBlock; ++i) peak = std::max(peak, std::fabs(ctx.outputs[0][i]));
    std::cout << "voice 0 gate: " << vm.registers[16][0]
              << "  mod wheel: " << ctx.getInputControllerValues(0)[1]
              << "  output peak: " << peak << "\n";
  }, milliseconds(1000));

  return synthTask.runConsoleApp();
}
