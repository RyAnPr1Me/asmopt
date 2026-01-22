# asmopt

Assembly Optimizer - A tool for optimizing x86/x86-64 assembly language code for AMD processors.

## Overview

`asmopt` is a program designed to optimize assembly language code using techniques similar to those employed by compiler backends, but operating directly on bare assembly code. The initial release focuses exclusively on **x86 and x86-64 assembly for AMD CPUs** (Zen through Zen 4), enabling deep optimization quality that matches or exceeds manually optimized assembly.

## Documentation

For a comprehensive specification of the assembly optimizer, including:
- Architecture and component design
- AMD-specific optimization techniques
- Supported optimization passes
- Input/output formats
- Implementation details
- API specifications

Please refer to [SPECIFICATION.md](SPECIFICATION.md)

## Features (Planned)

- **AMD CPU optimization**: Zen, Zen+, Zen 2, Zen 3, and Zen 4 microarchitectures
- **x86/x86-64 support**: Both 32-bit and 64-bit x86 assembly
- **Dual syntax support**: Intel and AT&T assembly syntax
- **Comprehensive optimizations**: Peephole optimization, dead code elimination, constant folding/propagation, instruction scheduling, register allocation, loop optimization, and more
- **AMD-specific optimizations**: Leverages AMD microarchitecture knowledge for optimal code generation
- **Detailed reporting**: Optional optimization reports showing what was changed and why
- **Safe transformations**: All optimizations preserve program semantics
- **Quality guarantee**: Output matches or exceeds manually optimized assembly

## Target Hardware

**Initial Release (v1.0)**:
- AMD Ryzen processors (Zen, Zen+, Zen 2, Zen 3, Zen 4)
- AMD EPYC server processors
- AMD Threadripper workstation processors

**Future Releases**:
- Intel processors
- ARM/AArch64 platforms
- RISC-V platforms

## Quick Start

```bash
# Configure and build
cmake -S . -B build
cmake --build build

# Basic optimization for x86-64
./build/asmopt input.s -o output.s

# Optimize for AMD Zen 3 with maximum optimization
./build/asmopt -O3 --mtune zen3 --report report.txt input.s -o output.s

# Convert AT&T syntax to Intel syntax while optimizing
./build/asmopt -f intel input_att.s -o output_intel.s
```

## C API

```c
#include "asmopt.h"

asmopt_context* opt = asmopt_create("x86-64");
asmopt_set_target_cpu(opt, "zen3");
asmopt_set_optimization_level(opt, 2);
asmopt_enable_optimization(opt, "peephole");
asmopt_set_amd_optimizations(opt, 1);
asmopt_parse_file(opt, "input.s");
asmopt_optimize(opt);
char* output = asmopt_generate_assembly(opt);
char* report = asmopt_generate_report(opt);
free(output);
free(report);
asmopt_destroy(opt);
```

## Requirements

- CMake 3.16+
- C11 compiler

## Project Status

This project is in the specification phase. See [SPECIFICATION.md](SPECIFICATION.md) for the complete design document focused on AMD x86/x86-64 optimization.
