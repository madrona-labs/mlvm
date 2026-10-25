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

Operators in mlvm can process multiple independent copies of a computation in a single instruction, controlled by the bank size field. A bank size of 0 means a single horizontal calculation, where each float is one frame in a single time series — one signal block in, one signal block out. A bank size of N where N is 1 or more means N independent 4x blocks of the operation are calculated simultaneously, each with its own input signals and state. The 4x blocks are SIMD-optimized and treat each 16-float block of data as 4 frames, each with 4 independent lanes. Banks are the mechanism for polyphony: a 16-voice synthesizer runs a filter bank of 16 instances per instruction rather than 16 separate filter instructions, reducing dispatch overhead.

TODO: make the bank size description less confusing and add a figure showing bank order in memory!

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
| **Layout** (FILL, TRANSPOSE...) (*4) | op | dest | src1 | bank size | src2 | element\# | `[unused]` | `[unused]` |
| **Compare and branch** (CMP, JMP...) (*5) | op | `[unused]` | src1 | `[unused]` | src2 | `[unused]` | pc offset 8:15 | pc offset 0:7 |
| **Matrix** (MATMUL) (*6) | op | src2 arena | src2 chunk | matrix size(s) | src1 arena | src1 chunk | dest arena | dest chunk |


**(*1) Delays** — Delay state objects include internal state (playhead position, internal filters, etc.) as well as the buffer memory itself.

**(*2) Projections** — The type field selects the projection function (256 possible). Examples: linear, logarithmic, breakpoint function, smoothstep, tanh, … Projections may have parameters and are stateless by definition.

**(*3) Memory access** — Examples: LOAD\_I places a scalar float immediate into a register. LOAD / STORE read from / write to arenas. 

**(*4) Layout** — Examples: FILL broadcasts element N of src1 to all elements of dest. TRANSPOSE converts between horizontal and vertical signal layouts.

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

To run a program, the client writes input signals including audio and events into the register file. Then the program is run, instruction by instruction until the HALT instruction is reached. Then the client copies the mlvm output registers into its own memory.


### Registers

- 256 registers, each holding one signal block of **16 float32 values**  
- Total register file: 256 × 64 bytes \= **16KB** — fits in L1 on all targets  
- Registers are **transient**: they live only within a block execution  
- Addressed by 8-bit index (0–255)

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

### ControlBlock: one block of a control-rate signal

A **SignalBlock** is one block's worth of an audio-rate signal: 16 floats. A **ControlBlock** is one block's worth of a control-rate signal: one float4 of plain data.

```
struct ControlBlock {
  float start;    // value at frame 0, or before the step
  float end;      // value at the last frame, or after the step
  float offset;   // step frame, 0..15. unused for a ramp
  float kind;     // 0 = ramp, 1 = step
};
```

A **ramp** moves linearly from start to end, landing on end at the last frame so the next block continues without a repeated sample. A **step** holds start for frames before offset and end from offset on, which gives sample-accurate note onsets and gate edges at a rate of one step per row per block. The producer, not the consumer, decides which: a note-on steps every row at the event frame (except a gliding pitch, which ramps), a note-off steps the gate, the voice-number row always steps, and everything else ramps. Because every ControlBlock is a complete description of its block, nothing downstream remembers anything between blocks.

All four fields are floats so the struct is exactly one float4, loads in one instruction, and drops into a float arena without reinterpretation. Small integers are exact in float. A two-float (start, end) form with a NaN-boxed step tag was considered and rejected: it saves 8 bytes per voice-row at the cost of a lossy half-float "before" value, a bit-level encoding that breaks `/fp:fast` NaN tests, and a type that other madronalib users could no longer treat as plain floats.

ControlBlock is defined in madronalib, since it is useful to any synth built on madronalib, not only to mlvm. madronalib does not know about arenas. It exports plain data types with a documented layout:

```
// one row (e.g. pitch) for all voices of one channel: row[v] is voice v's ControlBlock.
using ControlRow = std::array<ControlBlock, kMaxVoices>;        // 64 floats, 256 bytes

// all rows for one channel. The 9 a-rate rows plus one reserved row make 10,
// a round number that keeps the arena arithmetic easy to follow. The a-rate
// output count (kNumVoiceOutputRows = 9) is unchanged.
constexpr size_t kNumControlRows = 10;
struct VoiceControls { ControlRow rows[kNumControlRows]; };    // 10 rows, 2560 bytes
```

EventsToSignals grows a `VoiceControls` output alongside its existing a-rate outputs. The a-rate path stays on during a transition period; later a flag can switch it off, which is where the CPU saving is realized.

### The event arena

Event data is k-rate host data, and the memory model already has a home for that: **params as arena**. One event arena holds all channels. Its chunk size is 64 floats, one ControlRow, and its chunk index is channel × 10 \+ row. 8 channels use 80 of the 256 chunks.

| | per voice-row | 1 channel | 8 channels |
| :---- | :---- | :---- | :---- |
| a-rate SignalBlock, 9 rows | 64B | 9KB | 72KB |
| ControlBlock, 10 rows | 16B | 2.5KB | 20KB |

The boundary between madronalib and mlvm is a copy, once per block in `MLVM::processBlock`, from each channel's `VoiceControls` into the event arena. For 8 channels that is 20KB per block, 60MB/s at a 3kHz block rate, about a microsecond of a 333µs block. A zero-copy arena that views host memory is possible later if a profile asks for it.

Time info (tempo, beat phase, samples since start) takes the same shape in its own chunk. Beat phase needs care because a ramp across the wrap from 0.99 to 0.01 goes the wrong way; the producer can emit an unwrapped end value. Audio inputs remain true a-rate signals in registers.

### LOAD\_K: k-rate to a-rate

One opcode in the memory access category converts a ControlRow chunk into a-rate registers:

```
LOAD_K.4  r16, events/pitch      // dest: field 1, bank size: field 3, arena/chunk: fields 4,5
```

For each voice in the bank, LOAD\_K expands the voice's ControlBlock: a ramp fills the frames linearly from start to end, a step holds start until the offset frame and end after it. The frame and lane arrangement follows the bank layout; LOAD\_K's definition does not depend on it. The producer decided ramp versus step, so LOAD\_K needs no per-row knowledge.

Banking is what keeps programs short. One LOAD\_K converts one signal for all voices, so a 16-voice synth using pitch, gate and mod needs three conversion instructions, not 48. A synth converts only the rows it uses, and the VM only does conversion work for rows that are read.

```
LOAD_K.4  r16, events/pitch
LOAD_K.4  r20, events/gate
SAW.4     r24, r16, saw-params/0, saw-states/0
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

1. madronalib (branch `control-blocks`, done): `ControlBlock`, `ControlRow`, `VoiceControls`; EventsToSignals fills a `VoiceControls` at the end of `makeSignalBlock()` alongside its a-rate outputs; AudioContext exposes it per channel  
2. mlvm (branch `event-inputs`, done): the `events` arena, the per-block copy in `processBlock`, LOAD\_K with scalar ramp and step, tests that drive the VM with hand-built ControlBlocks  
3. Example: a bank of oscillators whose pitch comes from LOAD\_K (needs SAW)  

After that: stateful operator state layouts (SAW, SVF), bank inner loops, time info chunk.


## Key decisions log

- **Register machine, not stack**: fewer dispatches, explicit operands  
- **16 floats per register block**: fits one cacheline (64 bytes), confirmed safe for 32KB L1, allows global feedback up to 3kHz at sr=48k
- **Fixed 64-bit instructions**: simple fetch, good I-cache density  
- **Variable chunk size arenas**: one 8-bit chunk index works for 16-byte filter state and 4KB delay chunks alike — chunk size declared in manifest, not in instruction  
- **Buffers are part of state**: state for a delay or audio playback object might include playhead position and filter states, then buffer memory  
- **Manifest for arena sizes only**: all other object counts inferred at compile time by the compiler, not by scanning instructions at load time  
- **Params as arena**: k-rate control values from host live in a params arena, written before each block, read by operators via params arena+chunk fields  
- **Bank size 0 \= one horizontal copy**: clean encoding, the only special case  
- **Events are k-rate ControlBlocks in one event arena**: one float4 (start, end, offset, kind) per voice per row; the producer decides ramp vs. step and steps are sample accurate; LOAD\_K converts to a-rate. 72KB of a-rate signals becomes 20KB of ControlBlocks. See "Event and control inputs."  
- **ControlBlock is four plain floats, not a NaN-boxed pair**: 10KB more per 8 channels buys a lossless, debuggable type that stays general-purpose for every madronalib user.  
- **madronalib owns ControlBlock, mlvm owns the arena mapping**: madronalib exports plain data with a documented layout and never learns about arenas. The boundary is one memcpy per block.  
- **Opcodes declared once**: an X-macro list generates the enum and the mnemonic table, so the assembler can never fall out of sync with the VM.


## Open questions / TBD

- Compare and branch semantics: exact conditions (ALL, ANY, threshold?), flag register count. CMP has sources but no dest, so the assembler's generic register placement currently puts src1 in field 1 rather than field 2. Resolve together with the semantics, either by giving CMP a dest (flag in a register) or by adding a per-opcode operand format column to the opcode list.  
- When to turn off a-rate event signal generation in EventsToSignals once mlvm no longer reads it  
- A note-on and note-off in the same block: only one step per row per block is representable, so the gate reaches zero in the next block. Fine for MIDI; revisit if a second step per block is ever needed.  
- Projection type encoding: list of 256 projection functions TBD  
- Whether JIT compilation is in scope (not planned for v0.2)  
- Exact binary file format (magic number, version, endianness)  
- Whether the assembler will be a separate tool or embedded in the VM library
- Matrix sizes: stored in arena data, instruction or both?
- What kind of small scalar storage to add for loop counters and so on.

