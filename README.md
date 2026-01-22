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
# Basic optimization for x86-64
python -m asmopt input.s -o output.s

# Optimize for AMD Zen 3 with maximum optimization
python -m asmopt -O3 --mtune zen3 --report report.txt input.s -o output.s

# Convert AT&T syntax to Intel syntax while optimizing
python -m asmopt -f intel input_att.s -o output_intel.s
```

## Python API

```python
import asmopt

opt = asmopt.Optimizer(architecture="x86-64", target_cpu="zen3")
opt.set_optimization_level(2)
opt.enable_optimization("peephole")
opt.enable_amd_optimizations(True)
opt.load_file("input.s")
opt.optimize()
print(opt.get_assembly())
print(opt.get_report())
print(opt.get_statistics())
opt.save("output.s")
```

## Requirements

- Python 3.9+

## Project Status

This project is in the specification phase. See [SPECIFICATION.md](SPECIFICATION.md) for the complete design document focused on AMD x86/x86-64 optimization.
