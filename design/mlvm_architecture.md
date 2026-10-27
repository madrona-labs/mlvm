# mlvm Architecture
Last revised: Oct 25, 2026

## What is mlvm?

mlvm is a register-based virtual machine designed for audio signal processing. It is the technical foundation for Auk, a modular audio environment being developed by Madrona Labs. A host application (plugin, DAW, etc) makes an instance of mlvm and sends it a bytecode program to run, then audio and control signal inputs to produce audio outputs. 

## Motivation

Keeping audio processing within a virtual machine has several benefits over running arbitrary compiled C++ audio code: 

- programs can be loaded, replaced, and updated at runtime without recompiling or restarting the host  
- the VM provides a safe sandbox where a misbehaving program cannot crash the host application  
- bytecode is portable across platforms and CPU architectures without recompilation  
- a fixed, signal-based interface between host and VM makes it straightforward to visualize and debug a running program

The implementation language is C++. The codebase lives in the madronalib ecosystem. The designer is Randy Jones (Madrona Labs).

## Design philosophy

- **Data-oriented**: operators are stateless functions handed memory, not objects that own state  
- **Allocate-once, run-hot**: all memory layout is fixed at patch compile time, no runtime allocation  
- **Compiler does the hard work**: cache-friendliness, arena layout, bank scheduling are compiler concerns not ISA concerns  
- **L1 for registers, L2 for arenas**: register file is sized to fit in L1 cache on all target platforms  
- **Simple decoder**: one byte opcode at top, one byte bank size, switch dispatch, operands are always at the same position

## Target platforms

- Apple Silicon (M1+): 128KB L1 data cache per performance core  
- Intel/AMD desktop (13th gen, Ryzen 7000): 32–48KB L1 data cache  
- Raspberry Pi 4 (Cortex-A72): 32KB L1 data cache — **design floor**  
- Raspberry Pi 5 (Cortex-A76): 64KB L1 data cache

## Core data types and operation

mlvm is a data-oriented design. Its core is a register file consisting of blocks of 16 float32 values. All processing operates on these blocks — a single instruction consumes and produces whole blocks, never individual samples. The register file holds 256 such blocks (16KB total) and is designed to live entirely in L1 cache. The instruction stream is a separate concern and fits comfortably in the L1 instruction cache given that even complex DSP programs are unlikely to exceed a few hundred instructions. Each instruction is 64 bits, divided into eight one-byte fields.

### Instructions

An instruction is 64 bits long, consisting of a one-byte opcode followed by seven more one-byte fields. 

The table below shows proposed instructions categorized by use and by how their fields are packed into the 64-bit instruction word. mlvm instructions span a wide range of abstraction, from basic math primitives to complete stateful operations like filters and delay lines. Built-in high-level operators give programmers access to well-optimized, field-tested DSP building blocks like filters and oscillators with minimal code, while the lower-level math primitives allow custom DSP algorithms to be built from scratch. 

Fields in the instruction are arranged such that if a field is present, it will be in the same place for all opcodes that have it. In other words, bank size is always field 3 (bits 32:39) if present, and so on. This makes decoding simple.

Two values are stored using more than one field. Operations that use a program counter offset store a 16-bit offset in fields 6 and 7. Operations that have a 32 bit float immediate value store it in fields 4–7 to keep it aligned to a 4 byte boundary.

#### Field 0: opcode

One of 256 possible opcodes.

#### Field 1: dest index

Index of the destination register.

#### Field 2: src1 index

Index of the src1 register.

#### Field 3: bank size

Operators in mlvm can process multiple independent voices in a single instruction, controlled by the bank size field. Banks are the mechanism for polyphony: a 16-voice synthesizer runs one filter instruction with bank size 16 rather than 16 filter instructions, and the inner loop is SIMD over four voices at a time.

The bank size is a **voice count, 0 or a multiple of 4**. Bank size 0 means a single horizontal calculation: a register is one time series, one float per frame, one signal block in, one signal block out. A bank of N voices at register r occupies registers r through r+N−1 as N/4 madronalib `SignalBlock4`s in the **vertical** layout: one float4 per frame holding four voices, so voice v of the bank is lane v mod 4 of group v div 4. This is exactly the layout madronalib's `GenBank` and `FilterBank` compute in, so an mlvm bank hands madronalib processors their data with no rearrangement.

```
bank of 8 voices at r16, 64-frame block:

 register r16..r19  (group 0, one SignalBlock4)
   frame 0:  v0[0] v1[0] v2[0] v3[0]
   frame 1:  v0[1] v1[1] v2[1] v3[1]
   ...
   frame 63: v0[63] v1[63] v2[63] v3[63]
 register r20..r23  (group 1)
   frame 0:  v4[0] v5[0] v6[0] v7[0]
   ...
```

A bank uses the same memory as N horizontal voices, just arranged differently. **Elementwise operators are layout-blind**: ADD.16 applies to 16 registers in turn, which is correct for both layouts because the same index goes in and out. Only time-dependent operators (ramps, gates, filters, generators) and layout operators need to know the arrangement. Bank sizes that are not 0 or a multiple of 4 are assembler errors, as is a bank that does not fit in the register file.

Meeting a horizontal signal with a bank, say a global LFO or a channel-wide controller against 16 voices, is a layout operation: `SPREAD.16` broadcasts a horizontal register into a bank, and the control-rate operators broadcast a one-float row (a controller) into every lane directly, so channel-wide controllers never need SPREAD. `MIX.16` sums a bank into one horizontal signal, the last step of every polyphonic synth. `BANK` and `UNBANK` convert between N horizontal voice registers and a vertical bank.

#### Field 4: [src2 index, params arena, immed. 24-31]

Index of the src2 register, or index of the arena for ops with arena-stored parameters, or bits 24-31 of immediate.

#### Field 5: [src3 index, params chunk, element #, immed. 16-23]

Index of the src3 register, or chunk of the arena for ops with arena-stored parameters, or element # for layout operations, or bits 16-23 of immdiate. 

#### Field 6: [state arena, projection type, pc offset 8-15, immed. 8-15]

Index of the arena for ops with arena-stored states, or projection type, or high bits of 16-bit jump offset, or bits 8-15 of immediate.

#### Field 7: [state chunk, pc offset 0-7, immed. 0-7]

Chunk of the arena for ops with arena-stored states, or low bits of 16-bit jump offset, or bits 0-7 of immediate.


| Category | 56:63 | 48:55 | 40:47 | 32:39 | 24:31 | 16:23 | 8:15 | 0:7 |
| :---- | :---- | :---- | :---- | :---- | :---- | :---- | :---- | :---- |
|  | *field 0* | *field 1* | *field 2* | *field 3* | *field 4* | *field 5* | *field 6* | *field 7* |
| **System** (NOOP, HALT) | op | `[unused]` | `[unused]` | `[unused]` | `[unused]` | `[unused]` | `[unused]` | `[unused]` |
| **One-input math** (SIN, LOG, EXP, NEG, ABS...) | op | dest | src1 | bank size | `[unused]` | `[unused]` | `[unused]` | `[unused]` |
| **Two-input math** (ADD, ADD\_WRAP, MUL, MIN...) | op | dest | src1 | bank size | src2 | `[unused]` | `[unused]` | `[unused]` |
| **Three-input math** (LERP, CLAMP, SELECT...) | op | dest | src1 | bank size | src2 | src3 | `[unused]` | `[unused]` |
| **Filters/Gens** (SAW, SINE, SVF, ADSR, DELAY (*1)...) | op | dest | src1 | bank size | params arena | params chunk | state arena | state chunk |
| **Projections** (PROJ) (*2) | op | dest | src1 | bank size | params arena | params chunk | proj. type | `[unused]` |
| **Memory access** (LOAD\_I, MOV...) (*3) | op | dest | src1 | `[unused]` | immed. 24:31 | immed. 16:23 | immed. 8:15 | immed. 0:7 |
| **Arena access** (LOAD, STORE, ...) (*3) | op | dest | src1 | `[unused]` | src1 arena | src1 chunk | dest arena | dest chunk |
| **Layout** (FILL, SPREAD, MIX, BANK, UNBANK...) (*4) | op | dest | src1 | bank size | src2 | element\# | `[unused]` | `[unused]` |
| **Compare and branch** (CMP, JMP...) (*5) | op | `[unused]` | src1 | `[unused]` | src2 | `[unused]` | pc offset 8:15 | pc offset 0:7 |
| **Matrix** (MATMUL) (*6) | op | src2 arena | src2 chunk | matrix size(s) | src1 arena | src1 chunk | dest arena | dest chunk |


**(*1) Delays** — Delay state objects include internal state (playhead position, internal filters, etc.) as well as the buffer memory itself.

**(*2) Projections** — The type field selects the projection function (256 possible). Examples: linear, logarithmic, breakpoint function, smoothstep, tanh, … Projections may have parameters and are stateless by definition.

**(*3) Memory access** — Examples: LOAD\_I places a scalar float immediate into a register. LOAD / STORE read from / write to arenas. 

**(*4) Layout** — FILL broadcasts element N of src1 to all elements of dest. SPREAD broadcasts a horizontal signal into a bank. MIX sums a bank into a horizontal signal. BANK and UNBANK convert between N horizontal voice registers and a vertical bank; their source and destination must not overlap.

**(*5) Compare and branch** — CMP tests a condition across frames in a signal (ALL frames, ANY frame, etc.) and sets a flag register. JMP reads that flag. Target is a signed PC offset giving a range of ±32K instructions. Details TBD.

**(*6) Matrix** — The only instruction that takes arena indices as primary operands rather than register indices. Operates directly on arena memory. Matrix size details TBD.




### Program and manifest

The addresses and sizes of states within the arena are declared in the manifest, a section at the start of every mlvm program. The manifest is the only place where state sizes appear explicitly. In other words, the compiler is responsible for state layout. The manifest also serves as a symbol table: states are given human-readable names, and the assembler resolves these names to raw arena and chunk addresses in the instruction stream at assemble time.

#### Program sections

[ MANIFEST ]
- Arena declarations (name, chunk size)
- Symbol table: named arena+chunk addresses resolved by assembler

[ INSTRUCTIONS ]
- Fixed-width 64-bit instructions
- Hot loop, no dynamic allocation

The manifest is the only place buffer sizes appear explicitly — delay times may be computed parameters so they cannot be inferred by scanning the instruction stream. The compiler is responsible for all arena layout. 

The loader reads the manifest, allocates memory, and begins execution — it does not scan instructions.

To run a program, the host binds its data (audio input and output buffers, voice controls, controller values) to the program's **external arenas** by name, then runs the program instruction by instruction until HALT. Nothing is copied: the program loads what it needs from the external arenas into registers, computes, and stores results into the audio output arena. The register file is purely the program's working space. See "External arenas" below.


### Registers

- 256 registers, each holding one signal block of **16 float32 values**  
- Total register file: 256 × 64 bytes \= **16KB** — fits in L1 on all targets  
- Registers are **transient**: they live only within a block execution  
- Addressed by 8-bit index (0–255)

#### Block size (note, Oct 2026)

The 16-frame block above is the goal, not the current state. madronalib's `kFramesPerBlock` is 64 today, and current shipping plugins run at block size 64. The hope is that madronalib and mlvm will make efficient plugins possible at block size 16. However, **everything must work at block sizes up to 256**, in case future low-end platforms need it. A longer block makes registers longer: at 64 frames a register is 256 bytes and the register file is 64KB, at 256 frames it is 256KB, which no longer fits the L1 budget argued above. The time/space tradeoffs (fewer registers, a smaller register file, or accepting L2 for the register file at large block sizes) are to be explored later. Nothing in the ISA depends on the block size; the control-rate encodings in particular were chosen to be block-size agnostic.

### Arenas

Memory outside the register file is organized into arenas — flat byte buffers addressed by an 8-bit arena selector and an 8-bit chunk index, where each arena has its own chunk size. A small-state arena might use 16-byte chunks for filter coefficients and oscillator phase; a delay arena might use 4KB chunks to address large buffers. This gives 256 addressable chunks per arena, with total capacity scaling with chunk size. Arenas are the home for all persistent state: filter coefficients and history, oscillator phase, delay lines, and so on. Arenas are expected to live in L2 or beyond; the compiler is responsible for arranging objects within arenas to be as cache-friendly as possible, grouping hot small states together and pushing large cold states toward the back. There is no dynamic allocation at runtime — all arena layout, including chunk sizes, is fixed at patch compile time.

Many DSP primitives—oscillators, filters, delays—require a state to be maintained between processing blocks. Rather than objects that own state, mlvm has functions that take signal data and parameters as input, read and write a state in the arena, and produce signal data as output. State is just memory at an address that the function has been handed. This is similar in spirit to functional programming with explicit effects: operators are pure functions over bulk signal data, with any side effects — filter history, oscillator phase, delay buffers — isolated as states in the arena.

- Flat byte buffers addressed by **8-bit arena selector** \+ **8-bit chunk index**  
- Each arena has its own **chunk size declared in the manifest** (e.g. 16 bytes, 4KB, ...)  
- 256 addressable chunks per arena; total capacity \= 256 × chunk\_size  
- Arenas hold all **persistent state**: filter history, oscillator phase, delay buffers, params  
- Expected to live in **L2 or beyond**; compiler arranges layout for cache friendliness  
- Variable chunk size is key to the design: small arenas use 16-byte chunks, delay arenas use large chunks — same 8-bit index either way, chunk size declared once in manifest
- an assembler program can use *variables*—named arena locations—for easier reading by humans


### Memory model summary

Mlvm uses a “functional-with-effects" model. Rather than objects that own state, mlvm has functions — operators — that do one or more of the following: take signal data as input, read and write a state in the arena, and produce signal data as output. No operator owns its state; state is just memory at an address the operator has been handed. Operators are pure functions over bulk signal data, with side effects — filter history, oscillator phase, delay buffers — in the arena. The program uses two kinds of data: registers (transient signal blocks), and arena state (persistent state across block boundaries)

Including the program itself, that’s three kinds of memory, all fixed at compile time:

| Kind | Location | Size | Purpose |
| :---- | :---- | :---- | :---- |
| Register file | L1 | 16KB (256 × 64B) | Transient signal blocks during block execution |
| Arenas | L2+ | 256 × chunk\_size per arena | Persistent state, params, buffers, signal I/O |
| Program binary | — | varies | Manifest \+ instruction stream |

## Event and control inputs

### The problem

The host's EventsToSignals object turns one channel of events (typically one MIDI channel) into 9 a-rate signals per voice: pitch, gate, voice number, z, x, y, mod, elapsed time and bend. Auk's use case is a DSP module handling everything in a performance at once, so it takes up to 8 event channels of up to 16 voices each. As a-rate signals that is:

| | signal blocks | bytes |
| :---- | :---- | :---- |
| 1 channel, 16 voices | 144 | 9KB |
| 8 channels | 1152 | 72KB |
| register file | 256 | 16KB |

It does not fit, and even if it did, the host would generate 1152 a-rate blocks every 16 frames, nearly all of them flat lines, whether or not the program reads them. All-a-rate scores well on source clarity but very badly on runtime memory and CPU.

### Why k-rate is enough

A 16-frame block at 48kHz is a 3kHz control rate. Quantizing events to block boundaries adds at most 0.33ms of timing jitter, below MIDI's own jitter. With a linear ramp across each block the steps disappear. So event data can be k-rate with no audible loss, and the sample-accurate placement EventsToSignals does today becomes unnecessary for mlvm.

### Control values: one float per voice per row

A **SignalBlock** is one block's worth of an audio-rate signal: 16 floats. A control-rate signal is **one float per block**. Which kind of float it is comes from the row it sits in, so no tag bits are needed anywhere.

- **Value rows** (pitch, velocity, voice, z, x, y, mod, elapsed time, bend) hold the value at the end of the block. A consumer that wants audio rate ramps to it from the previous value it saw, which it keeps as its own state. Interpolation state therefore lives in the consumer, where every other stateful operator already keeps state, and the producer stays stateless.
- **The gate row** holds at most one transition per block: `0` means no transition, `+(k+1)` rises to high at frame k, `-(k+1)` falls to low at frame k. The offset is a small integer stored exactly in the float, so the encoding does not depend on the block size and will survive a move to 32 or 64 frames. A consumer keeps the gate level as its state.

Gate and velocity are separate rows here. The audio-rate `kGate` row carries gate × velocity for compatibility; splitting them in the k-rate path is what lets the gate be sample accurate in one float. With velocity in the tenth slot there are exactly 10 rows and no reserved row.

**Two edges in one block.** A retrigger falls then rises; a very short note rises then falls. The producer encodes the first edge, and tracks the level the consumer now holds. The next block's a-rate gate is constant at the other level, so the producer sees a transition at frame 0 and encodes it. A stolen voice's onset is delayed by less than one block, the cv change lands with the deferred rise, and nothing is lost. This is the sr/(2 × block size) event rate limit, accepted by design.

The alternatives: v1 used a (start, end) pair per value so the producer could describe ramps with no consumer state, then a four-float (start, end, offset, kind) block for sample-accurate steps. A 16-bit frame mask in the gate float was considered: exact, any number of edges, but it depends on 16 frames fitting a float mantissa and loses resolution if the block grows. A NaN-boxed step tag in a two-float pair was rejected for being lossy and for breaking the plain-float contract for every madronalib user.

madronalib owns the types and knows nothing about arenas:

```
enum ControlSignals { kCtlPitch, kCtlGate, kCtlVelocity, kCtlVoice, kCtlZ, kCtlX, kCtlY,
                      kCtlMod, kCtlElapsedTime, kCtlBend, kNumControlRows };   // 10
using ControlRow = std::array<float, 16>;              // one row for all voices
struct VoiceControls { ControlRow rows[kNumControlRows]; };   // one channel, 640 bytes
```

EventsToSignals fills a `VoiceControls` at the end of `makeSignalBlock()` alongside its a-rate outputs, derived from them. The a-rate path stays on during a transition period; later a flag can switch it off, which is where the CPU saving is realized, and the derivation becomes direct.

### Controllers

Each event channel also has 128 continuous controllers. Their values are one float each with no voice dimension, 4KB for 8 channels, and they change only when a controller event arrives. madronalib keeps a `ControllerValues` table (`std::array<float, 128>`) per channel, written on events and never touched per block. Smoothing is the consumer's job, so a program chooses how a controller is interpolated. Channel pressure is not in the table; it is added into the z voice row as before.

### External arenas

Host data is k-rate (or audio) data the program may or may not read. Copying it into the VM every block would cost bandwidth whether or not it is used, and the amounts add up: 5KB of voice controls, 4KB of controllers, 4KB of audio I/O per block. Instead the manifest declares **external arenas**, and the host binds a pointer to each by name before every block. The data stays where the host wrote it, a program pays exactly for the reads it makes, and the host pays nothing for data no program uses.

```
EXTERN audioin  64     // one SignalBlock per chunk, chunk = channel (read-only)
EXTERN audioout 64     // same layout, the only writable external arena
EXTERN events0  16     // channel 0's VoiceControls: chunk = row (ControlSignals), 16 voices per chunk
EXTERN ctl0     1      // channel 0's ControllerValues: chunk = controller number
```

The loader resolves the names once, checks the chunk sizes against the host layouts, and the VM binds pointers by index each block with no lookups. An external arena the host does not provide (an audio input on a context with no inputs, an event channel beyond those configured) is simply unbound: reads from it are no-ops and leave registers untouched. Stores to read-only external arenas are ignored. Only `audioout` is writable.

| host data | per block, 8 channels | copied per block before | now |
| :---- | :---- | :---- | :---- |
| voice controls, 10 rows × 16 voices | 5KB | yes | bound |
| controllers, 128 per channel | 4KB | would have been | bound |
| audio in and out, 8 \+ 8 channels at 64 frames | 4KB | yes | bound |

The per-channel arena shape (`events3/row` rather than one arena indexed by channel × 10 \+ row) falls out of the host's memory: each channel's data is contiguous but the channels are not contiguous with each other. It also reads better. madronalib still knows nothing about arenas; its structs are the arena layouts, pinned by static asserts in mlvm.

Time info (tempo, beat phase, samples since start) takes the same shape in its own chunk. Beat phase needs care because a ramp across the wrap from 0.99 to 0.01 goes the wrong way; the producer can emit an unwrapped end value. Audio inputs remain true a-rate signals in registers.

### Control-rate operators

Three stateful operators convert control rows to audio rate. They use the Filters/Gens field layout: dest in field 1, bank size in field 3, the control row's arena and chunk in fields 4,5, and a state chunk in fields 6,7. State is one float per voice (two for LOAD\_KS), declared by the program like any other state.

```
LOAD_G   r1, events0/gate,  kstate/0     // gate codes -> 0/1 block. state: gate level
LOAD_KS  r2, r1, events0/pitch, kstate/1 // sample and hold: hold the previous value until the
                                         // gate in src1 rises, then step. state: value + gate level
LOAD_K   r3, events0/bend,  kstate/2     // ramp from the previous value. state: value
LOAD_K   r4, ctl0/1,  kstate/3           // a controller is a one-float row: same operator
```

LOAD\_KS is the one that makes notes sample accurate: velocity, pitch, elapsed time and voice number all change exactly at the onset frame, because the producer guarantees the cv for a block pairs with the gate rise in that block. Between onsets LOAD\_KS ramps like LOAD\_K, so a gliding or bent pitch stays smooth; for rows that only change at onsets the two behaviours are identical. A program that does not care about onset accuracy can use LOAD\_K alone and accept a short ramp at note-on.

**Ramps are 16 frames long regardless of the block size.** LOAD\_K and LOAD\_KS ramp over `kControlRampFrames` frames (16, clamped to the block if it is shorter) and then hold, rather than ramping over the whole block. A program therefore sounds the same at block size 16, 64 or 256. This sets the pattern for every operator that depends on time: express it in frames or seconds, never in blocks. The choice of interpolation (linear now; a glide operator with a variable time, or other curves, later) belongs to the VM, which keeps madronalib general-purpose.

Banking is what keeps programs short. One instruction converts one row for all voices, so a 16-voice synth using gate, pitch and velocity needs three instructions, not 48. A synth converts only the rows it uses, and the VM only does conversion work for rows that are read. With a bank size these operators write vertical banks directly: a `ControlRow` holds its 16 voices contiguously, so voices 0 to 3 are one float4 load, the lane order a vertical group wants, and LOAD\_K for a group is one float4 ramp. A one-float row (a controller) is broadcast to every lane.

A stateful operator does not always need LOAD\_K in front of it. madronalib's processors accept per-block parameters, one float4 per group of voices, and interpolate inside, and a `ControlRow` already has that shape. So SAW can take its pitch straight from `events0/pitch` through its params fields when nothing else needs that value at audio rate. LOAD\_K is for values the program does arithmetic on.

```
LOAD_G.4   r16, events/gate,  kstate/0
LOAD_KS.4  r20, r16, events/pitch, kstate/1
SAW.4      r24, r20, saw-params/0, saw-states/0
```

Stateful operators already carry params arena and chunk fields, so an operator designed for it can read a k-rate parameter directly from the event arena with no conversion when a-rate modulation of that input is not needed. This is optional and does not change the main path.

### Alternatives considered

- **K-rate operand type on every op.** Zero conversion lines, but every operand fetch on the hot path grows a type test, a signal used by three ops is converted three times, and 8-bit register indices would have to give up address space for the flag. Permanent decoder complexity to save a few lines of program.
- **K-rate values packed into frames of input registers.** 16 voices of one row is exactly one block, so the 8-channel table is 72 registers. A neat fit, but it spends 28% of the L1 register file on data read once per block. By the memory model that data belongs in L2, i.e. an arena. This is the event arena design with the table in the wrong tier.
- **Raw events into the VM.** Sample accurate, but every stateful op grows event handling and the everything-is-a-signal model is lost.

## Assembler design (planned)

- Human-readable text format  
- Named symbols for arenas and chunks (resolved to raw addresses at assemble time)  
- Manifest section at top declaring buffers and arenas with chunk sizes  
- One instruction per line, mnemonic \+ operands. Bank size is a mnemonic suffix: `SVF.4` is bank size 4, `SVF` is 0.  
- Operand kind is determined by its text, and kind determines its field: registers `rN` fill fields 1, 2, 4, 5 in order; arena refs `name/chunk` fill 4,5 then 6,7; a float immediate (`42.333f`) fills 4–7; a label fills 6,7 as a pc offset.  
- Opcodes are declared once, in an X-macro list in `mlvm.h`, which generates both the enum and the mnemonic table.  
- Output: binary program file (manifest \+ instruction stream)

Example assembly language program (sketch):

````
// MANIFEST

ARENA svf-params 5
ARENA svf-states 2
ARENA delay-params 10
ARENA delay-states 4096 // delay memory indexable in 4096-float chunks

// INSTRUCTIONS

LOAD      r12, delay-params/1
LOAD_I    r12, 42.333f
LOAD_K.4  r16, events/pitch
filter1: // a label
SVF.4     r2, r1, svf-params/3, svf-states/3
DELAY     r4, r3, delay-params/0, delay-states/0
CMP r5, r6
BNE filter1
HALT
````


## Next steps

Done (Oct 2026): opcode list, assembler, VM instruction loop with tests. Implemented: NOOP, HALT, MOVE, LOAD\_I, LOAD, STORE, CMP, BNE, JMP, NEG, ABS, SIN, ADD, SUB, MUL, DIV, LERP, FILL. SAW and SVF are stubs that pass src1 through until arena state layouts are defined. Bank size is decoded but inner implementations are scalar.

The next task is **event inputs**, per "Event and control inputs" above. It spans madronalib and mlvm, with the a-rate path left on throughout:

1. madronalib (branch `control-blocks`, done): `ControlSignals`, `ControlRow`, `VoiceControls`, `ControllerValues`, gate transition helpers; EventsToSignals fills them alongside its a-rate outputs; AudioContext exposes them per channel  
2. mlvm (branch `event-inputs`, done): `EXTERN` arenas bound from the context each block (audio I/O, `events<c>`, `ctl<c>`), LOAD\_K, LOAD\_KS and LOAD\_G with per-voice state and 16-frame ramps, tests that drive the VM across several blocks through bound buffers  
3. A glide operator with a variable time for controllers and other continuous values  
4. Time info (tempo, beat phase, samples since start) as an external arena  
5. Example: a bank of oscillators whose pitch comes from LOAD\_KS and whose envelope is gated by LOAD\_G (needs SAW and an envelope)  

After that: stateful operator state layouts (SAW, SVF), bank inner loops, time info chunk.


## Key decisions log

- **Register machine, not stack**: fewer dispatches, explicit operands  
- **16 floats per register block**: fits one cacheline (64 bytes), confirmed safe for 32KB L1, allows global feedback up to 3kHz at sr=48k. This is the target; shipping plugins use 64 today and sizes up to 256 must work. See "Block size" under Registers.
- **Fixed 64-bit instructions**: simple fetch, good I-cache density  
- **Variable chunk size arenas**: one 8-bit chunk index works for 16-byte filter state and 4KB delay chunks alike — chunk size declared in manifest, not in instruction  
- **Buffers are part of state**: state for a delay or audio playback object might include playhead position and filter states, then buffer memory  
- **Manifest for arena sizes only**: all other object counts inferred at compile time by the compiler, not by scanning instructions at load time  
- **Params as arena**: k-rate control values from host live in a params arena, written before each block, read by operators via params arena+chunk fields  
- **Bank size 0 \= one horizontal copy**: clean encoding, the only special case  
- **Events are one float per voice per row in one event arena**: value rows hold the block-end value, the gate row holds a signed transition offset. 72KB of a-rate signals becomes 5KB. See "Event and control inputs."  
- **Interpolation state lives in the consumer**: LOAD\_K, LOAD\_KS and LOAD\_G keep per-voice state like every other stateful operator, so the producer is stateless and a value is a single float. (v1 put [start, end] pairs and then four-float ramp/step blocks in the data instead; both worked, both were bigger than needed.)  
- **Gate is a signed transition offset, not a frame mask**: `0` none, `±(k+1)` rise/fall at frame k. Block-size agnostic. A 16-bit frame mask was exact and allowed many edges per block but only because 16 frames fit a float mantissa.  
- **Gate and velocity are separate k-rate rows**: the one place sample-accurate timing and a full-resolution value coincided, split so each is one float. The a-rate gate × velocity row is unchanged.  
- **One transition per block**: the producer encodes the first edge and tracks the consumer's level so the next block corrects at frame 0. Event rate limit is sr/(2 × block size).  
- **Host data is bound, not copied**: `EXTERN` arenas for audio I/O, voice controls and controllers, one per channel, bound by name each block. The copies were cheap (about 1% of a block on the design-floor platform) but a bound pointer costs nothing for data a program does not read, lets the producer update controllers only on events, and makes host input one mechanism. Registers are now purely program scratch.  
- **128 controllers per channel, raw values, VM interpolates**: smoothing moves from the producer to the consumer so a program chooses it. Channel pressure stays in the z voice row.  
- **16-frame control ramps**: time in frames, never in blocks, so programs are block-size independent.  
- **Bank size is a voice count, a multiple of 4, in madronalib's vertical layout**: N voices occupy N registers as N/4 SignalBlock4s. Elementwise operators are layout-blind. SPREAD, MIX, BANK and UNBANK move between layouts; control-rate operators write banks directly and broadcast one-float rows.  
- **One event channel per bank**: multitimbral programs run one bank per channel. MPE per-note controls arrive as voice rows already.  
- **madronalib owns ControlBlock, mlvm owns the arena mapping**: madronalib exports plain data with a documented layout and never learns about arenas. The boundary is one memcpy per block.  
- **Opcodes declared once**: an X-macro list generates the enum and the mnemonic table, so the assembler can never fall out of sync with the VM.


## Open questions / TBD

- Compare and branch semantics: exact conditions (ALL, ANY, threshold?), flag register count. CMP has sources but no dest, so the assembler's generic register placement currently puts src1 in field 1 rather than field 2. Resolve together with the semantics, either by giving CMP a dest (flag in a register) or by adding a per-opcode operand format column to the opcode list.  
- When to turn off a-rate event signal generation in EventsToSignals once mlvm no longer reads it  
- madronalib's per-block `Params` interpolation inside processors spans the block; it should become 16 frames to match LOAD\_K and keep sounds block-size independent  
- Fused broadcast variants of ADD/MUL (bank × horizontal without SPREAD) only if profiles show SPREAD's extra registers matter  
- Time info (tempo, beat phase, samples since start) as control rows in their own chunk; beat phase needs an unwrapped end value so a ramp across the wrap goes the right way  
- Projection type encoding: list of 256 projection functions TBD  
- Whether JIT compilation is in scope (not planned for v0.2)  
- Exact binary file format (magic number, version, endianness)  
- Whether the assembler will be a separate tool or embedded in the VM library
- Matrix sizes: stored in arena data, instruction or both?
- What kind of small scalar storage to add for loop counters and so on.

