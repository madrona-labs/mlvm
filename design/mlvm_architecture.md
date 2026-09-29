# mlvm Architecture
Last revised: Sep 29, 2026

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

An instruction is 64 bytes long, consisting of a one-byte opcode followed by seven more one-byte fields. 

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

## Assembler design (planned)

- Human-readable text format  
- Named symbols for arenas and chunks (resolved to raw addresses at assemble time)  
- Manifest section at top declaring buffers and arenas with chunk sizes  
- One instruction per line, mnemonic \+ operands  
- Output: binary program file (manifest \+ instruction stream)

Example assembly language program (sketch):

````
// MANIFEST

ARENA svf-params 5
ARENA svf-states 2
ARENA delay-params 10
ARENA delay-states 4096 // delay memory indexable in 4096-float chunks

// INSTRUCTIONS

LOAD	r12, r13
LOAD_I    r12, 42.333f
filter1: // a label
SVF    r2, r1, 4, svf-params/3, svf-states/3
DELAY  r4, r3, 1, delay-params/0, delay-states/0
CMP r5, r6
BNE filter1
HALT
````


## Next steps

The immediate next task is a **draft assembler and VM** with one or two instructions from each category implemented. Suggested order:

1. Define the binary format structs (manifest, instruction word)  
2. Implement the assembler: parse manifest, parse instructions, emit binary  
3. Implement the VM: load program, allocate arenas, run instruction loop (switch dispatch)  
4. Implement one or two examples from each category:  
   - System: NOOP, HALT  
   - One-input math: NEG  
   - Two-input math: ADD  
   - Three-input math: LERP  
   - Generator: SAW (stateful, needs params \+ state)  
   - Filter: SVF (stateful, needs params \+ state)  
   - Memory access: LOAD_I (float immediate into register), LOAD, STORE  
   - Layout: FILL (broadcast element N to whole register)  
   - Compare and branch: CMP \+ JMP (basic loop)

Bank support can be scaffolded (bank size field read, loop over copies) but inner implementations can be scalar first.


## Key decisions log

- **Register machine, not stack**: fewer dispatches, explicit operands  
- **16 floats per register block**: fits one cacheline (64 bytes), confirmed safe for 32KB L1, allows global feedback up to 3kHz at sr=48k
- **Fixed 64-bit instructions**: simple fetch, good I-cache density  
- **Variable chunk size arenas**: one 8-bit chunk index works for 16-byte filter state and 4KB delay chunks alike — chunk size declared in manifest, not in instruction  
- **Buffers are part of state**: state for a delay or audio playback object might include playhead position and filter states, then buffer memory  
- **Manifest for arena sizes only**: all other object counts inferred at compile time by the compiler, not by scanning instructions at load time  
- **Params as arena**: k-rate control values from host live in a params arena, written before each block, read by operators via params arena+chunk fields  
- **Bank size 0 \= one horizontal copy**: clean encoding, the only special case


## Open questions / TBD

- Compare and branch semantics: exact conditions (ALL, ANY, threshold?), flag register count  
- Projection type encoding: list of 256 projection functions TBD  
- Whether JIT compilation is in scope (not planned for v0.2)  
- Exact binary file format (magic number, version, endianness)  
- Whether the assembler will be a separate tool or embedded in the VM library
- Matrix sizes: stored in arena data, instruction or both?
- What kind of small scalar storage to add for loop counters and so on.

