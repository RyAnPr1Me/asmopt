"""Core optimizer logic for asmopt."""

from __future__ import annotations

from dataclasses import dataclass, field
import re
from typing import Dict, List, Optional, Sequence, Tuple


_COMMENT_MARKERS = (";", "#")


@dataclass
class OptimizationStats:
    original_lines: int = 0
    optimized_lines: int = 0
    replacements: int = 0
    removals: int = 0

    def to_dict(self) -> Dict[str, int]:
        return {
            "original_lines": self.original_lines,
            "optimized_lines": self.optimized_lines,
            "replacements": self.replacements,
            "removals": self.removals,
        }


@dataclass
class IRLine:
    line_no: int
    kind: str
    text: str
    mnemonic: Optional[str] = None
    operands: List[str] = field(default_factory=list)


@dataclass
class CFGBlock:
    name: str
    instructions: List[IRLine]


class Optimizer:
    def __init__(
        self,
        architecture: str = "x86-64",
        target_cpu: str = "generic",
        format: Optional[str] = None,
    ) -> None:
        self.architecture = architecture
        self.target_cpu = target_cpu
        self.format = format
        self.optimization_level = 2
        self.amd_optimizations = True
        self.enabled_optimizations = {"peephole"}
        self.disabled_optimizations = set()
        self.options: Dict[str, object] = {}
        self.no_optimize = False
        self._original_text: Optional[str] = None
        self._original_lines: List[str] = []
        self._optimized_lines: List[str] = []
        self._ir: List[IRLine] = []
        self._cfg_blocks: List[CFGBlock] = []
        self._cfg_edges: List[Tuple[str, str]] = []
        self._stats = OptimizationStats()
        self._trailing_newline = False

    def set_optimization_level(self, level: int) -> None:
        self.optimization_level = max(0, min(level, 4))

    def enable_optimization(self, name: str) -> None:
        if name == "all":
            self.enabled_optimizations.add("peephole")
            return
        self.enabled_optimizations.add(name)

    def disable_optimization(self, name: str) -> None:
        if name == "all":
            self.enabled_optimizations.clear()
            return
        self.enabled_optimizations.discard(name)
        self.disabled_optimizations.add(name)

    def enable_amd_optimizations(self, enabled: bool) -> None:
        self.amd_optimizations = enabled

    def load_file(self, path: str) -> None:
        with open(path, "r", encoding="utf-8") as handle:
            text = handle.read()
        self.load_string(text)

    def load_string(self, assembly: str) -> None:
        self._original_text = assembly
        self._trailing_newline = assembly.endswith("\n")
        self._original_lines = assembly.splitlines()
        self._optimized_lines = []
        self._ir = []
        self._cfg_blocks = []
        self._cfg_edges = []
        self._stats = OptimizationStats()

    def optimize(self) -> str:
        if self._original_text is None:
            raise ValueError("No assembly loaded")
        syntax = self._detect_syntax(self._original_lines)
        self._ir = self._build_ir(self._original_lines)
        self._cfg_blocks, self._cfg_edges = self._build_cfg(self._ir)
        if self.no_optimize or self.optimization_level == 0:
            self._optimized_lines = list(self._original_lines)
            self._stats = self._build_stats()
            return self.get_assembly()
        if "peephole" not in self.enabled_optimizations:
            self._optimized_lines = list(self._original_lines)
            self._stats = self._build_stats()
            return self.get_assembly()

        optimized: List[str] = []
        replacements = 0
        removals = 0
        for line in self._original_lines:
            result, replaced, removed = self._peephole_line(line, syntax)
            if removed:
                removals += 1
                if result is not None:
                    optimized.append(result)
                continue
            if result is not None:
                optimized.append(result)
                if replaced:
                    replacements += 1
        self._optimized_lines = optimized
        self._stats = OptimizationStats(
            original_lines=len(self._original_lines),
            optimized_lines=len(self._optimized_lines),
            replacements=replacements,
            removals=removals,
        )
        return self.get_assembly()

    def get_assembly(self) -> str:
        if not self._optimized_lines and self._original_lines:
            self._optimized_lines = list(self._original_lines)
        text = "\n".join(self._optimized_lines)
        if self._trailing_newline:
            text += "\n"
        return text

    def get_report(self) -> str:
        stats = self.get_statistics()
        return (
            "Optimization Report\n"
            f"Original lines: {stats['original_lines']}\n"
            f"Optimized lines: {stats['optimized_lines']}\n"
            f"Replacements: {stats['replacements']}\n"
            f"Removals: {stats['removals']}\n"
        )

    def get_statistics(self) -> Dict[str, int]:
        if self._stats.original_lines == 0 and self._original_lines:
            self._stats = self._build_stats()
        return self._stats.to_dict()

    def save(self, path: str) -> None:
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(self.get_assembly())

    def get_ir(self) -> List[IRLine]:
        if not self._ir and self._original_lines:
            self._ir = self._build_ir(self._original_lines)
        return list(self._ir)

    def get_cfg(self) -> Tuple[List[CFGBlock], List[Tuple[str, str]]]:
        if not self._cfg_blocks and self._original_lines:
            self._cfg_blocks, self._cfg_edges = self._build_cfg(self.get_ir())
        return list(self._cfg_blocks), list(self._cfg_edges)

    def dump_ir_text(self) -> str:
        lines = ["IR:"]
        for entry in self.get_ir():
            if entry.kind == "instruction":
                operands = ", ".join(entry.operands)
                lines.append(f"{entry.line_no:04d}: instr {entry.mnemonic} {operands}".rstrip())
            else:
                lines.append(f"{entry.line_no:04d}: {entry.kind} {entry.text}".rstrip())
        return "\n".join(lines) + "\n"

    def dump_cfg_text(self) -> str:
        blocks, edges = self.get_cfg()
        lines = ["CFG:"]
        edge_map = {}
        for source, target in edges:
            edge_map.setdefault(source, []).append(target)
        for block in blocks:
            lines.append(f"{block.name}:")
            for instr in block.instructions:
                operands = ", ".join(instr.operands)
                lines.append(f"  {instr.mnemonic} {operands}".rstrip())
            if block.name in edge_map:
                targets = ", ".join(edge_map[block.name])
                lines.append(f"  -> {targets}")
        return "\n".join(lines) + "\n"

    def dump_cfg_dot(self) -> str:
        blocks, edges = self.get_cfg()
        lines = ["digraph cfg {", "  node [shape=box];"]
        for block in blocks:
            label_lines = [block.name + ":"]
            for instr in block.instructions:
                operands = ", ".join(instr.operands)
                label_lines.append(f"{instr.mnemonic} {operands}".rstrip())
            label = "\\l".join(label_lines) + "\\l"
            lines.append(f"  {block.name} [label=\"{label}\"]; ")
        for source, target in edges:
            lines.append(f"  {source} -> {target};")
        lines.append("}")
        return "\n".join(lines) + "\n"

    def _build_stats(self) -> OptimizationStats:
        return OptimizationStats(
            original_lines=len(self._original_lines),
            optimized_lines=len(self._optimized_lines),
            replacements=self._stats.replacements,
            removals=self._stats.removals,
        )

    def _detect_syntax(self, lines: Sequence[str]) -> str:
        if self.format in {"intel", "att"}:
            return self.format
        for line in lines:
            if "%" in line:
                return "att"
        return "intel"

    def _split_comment(self, line: str) -> Tuple[str, str]:
        comment_index = None
        for marker in _COMMENT_MARKERS:
            index = line.find(marker)
            if index != -1 and (comment_index is None or index < comment_index):
                comment_index = index
        if comment_index is None:
            return line, ""
        return line[:comment_index], line[comment_index:]

    def _is_directive_or_label(self, code: str) -> bool:
        stripped = code.strip()
        if not stripped:
            return True
        if stripped.startswith("."):
            return True
        if stripped.endswith(":"):
            return True
        return False

    def _parse_instruction(self, code: str) -> Optional[Tuple[str, str, str, str]]:
        match = re.match(r"(\s*)([A-Za-z][A-Za-z0-9.]*)(\s+)?(.*)?", code)
        if not match:
            return None
        indent, mnemonic, spacing, operands = match.groups()
        return indent, mnemonic, spacing or "", operands or ""

    def _parse_operands(self, operands: str) -> Optional[Tuple[str, str, str, str]]:
        if "," not in operands:
            return None
        before, after = operands.split(",", 1)
        pre_space = before[len(before.rstrip()) :]
        post_space = after[: len(after) - len(after.lstrip())]
        return before.strip(), after.strip(), pre_space, post_space

    def _normalize_register(self, operand: str, syntax: str) -> Optional[str]:
        op = operand.strip()
        if syntax == "att":
            if not op.startswith("%"):
                return None
            op = op[1:]
        if op.startswith("$"):
            return None
        if any(ch in op for ch in "[]()"):
            return None
        if op.startswith("*"):
            return None
        if not re.match(r"^[A-Za-z][A-Za-z0-9]*$", op):
            return None
        return op.lower()

    def _is_immediate_zero(self, operand: str, syntax: str) -> bool:
        op = operand.strip().lower()
        if syntax == "att":
            if not op.startswith("$"):
                return False
            op = op[1:]
        if op in {"0", "0x0"}:
            return True
        return False

    def _peephole_line(self, line: str, syntax: str) -> Tuple[Optional[str], bool, bool]:
        code, comment = self._split_comment(line)
        if self._is_directive_or_label(code):
            return line, False, False
        parsed = self._parse_instruction(code)
        if not parsed:
            return line, False, False
        indent, mnemonic, spacing, operands = parsed
        operands_info = self._parse_operands(operands)
        if not operands_info:
            return line, False, False
        op1, op2, pre_space, post_space = operands_info
        mnemonic_lower = mnemonic.lower()
        suffix = ""
        if mnemonic_lower == "mov":
            suffix = ""
        elif len(mnemonic_lower) == 4 and mnemonic_lower.startswith("mov") and mnemonic_lower[3] in "bwlq":
            suffix = mnemonic_lower[3]
        else:
            return line, False, False

        if syntax == "intel":
            dest = op1
            src = op2
        else:
            src = op1
            dest = op2
        dest_reg = self._normalize_register(dest, syntax)
        src_reg = self._normalize_register(src, syntax)
        if dest_reg and src_reg and dest_reg == src_reg:
            if comment:
                return f"{indent}{comment.lstrip()}", False, True
            return None, False, True

        if dest_reg and self._is_immediate_zero(src, syntax):
            xor_mnemonic = f"xor{suffix}" if suffix else "xor"
            new_operands = f"{dest}{pre_space},{post_space}{dest}"
            comment_suffix = f" {comment.lstrip()}" if comment else ""
            new_line = f"{indent}{xor_mnemonic}{spacing}{new_operands}{comment_suffix}".rstrip()
            return new_line, True, False

        return line, False, False

    def _build_ir(self, lines: Sequence[str]) -> List[IRLine]:
        ir: List[IRLine] = []
        for index, line in enumerate(lines, start=1):
            code, _comment = self._split_comment(line)
            stripped = code.strip()
            if not stripped:
                ir.append(IRLine(index, "blank", ""))
                continue
            if stripped.startswith("."):
                ir.append(IRLine(index, "directive", stripped))
                continue
            if stripped.endswith(":"):
                ir.append(IRLine(index, "label", stripped.rstrip(":")))
                continue
            parsed = self._parse_instruction(code)
            if not parsed:
                ir.append(IRLine(index, "text", stripped))
                continue
            _, mnemonic, _, operands = parsed
            operands_list = [part.strip() for part in operands.split(",") if part.strip()] if operands else []
            ir.append(IRLine(index, "instruction", stripped, mnemonic, operands_list))
        return ir

    def _build_cfg(self, ir: Sequence[IRLine]) -> Tuple[List[CFGBlock], List[Tuple[str, str]]]:
        blocks: List[CFGBlock] = []
        current_instrs: List[IRLine] = []
        current_label: Optional[str] = None
        label_to_block: Dict[str, str] = {}

        def finalize_block() -> None:
            nonlocal current_instrs, current_label
            if not current_instrs and current_label is None:
                return
            name = current_label or f"block{len(blocks)}"
            block = CFGBlock(name=name, instructions=current_instrs)
            blocks.append(block)
            if current_label:
                label_to_block[current_label] = name
            current_instrs = []
            current_label = None

        for entry in ir:
            if entry.kind == "label":
                finalize_block()
                current_label = entry.text
                continue
            if entry.kind != "instruction":
                continue
            current_instrs.append(entry)
            if self._is_terminator(entry):
                finalize_block()

        finalize_block()

        if not blocks:
            blocks = [CFGBlock(name="block0", instructions=[])]

        edges: List[Tuple[str, str]] = []
        for idx, block in enumerate(blocks):
            if not block.instructions:
                continue
            last = block.instructions[-1]
            if self._is_jump(last):
                target = self._jump_target(last)
                if target and target in label_to_block:
                    edges.append((block.name, label_to_block[target]))
                if self._is_conditional_jump(last) and idx + 1 < len(blocks):
                    edges.append((block.name, blocks[idx + 1].name))
            elif self._is_return(last):
                continue
            else:
                if idx + 1 < len(blocks):
                    edges.append((block.name, blocks[idx + 1].name))
        return blocks, edges

    def _is_jump(self, instr: IRLine) -> bool:
        if not instr.mnemonic:
            return False
        return instr.mnemonic.lower().startswith("j")

    def _is_conditional_jump(self, instr: IRLine) -> bool:
        if not instr.mnemonic:
            return False
        return instr.mnemonic.lower().startswith("j") and instr.mnemonic.lower() != "jmp"

    def _is_return(self, instr: IRLine) -> bool:
        if not instr.mnemonic:
            return False
        return instr.mnemonic.lower().startswith("ret")

    def _is_terminator(self, instr: IRLine) -> bool:
        return self._is_jump(instr) or self._is_return(instr)

    def _jump_target(self, instr: IRLine) -> Optional[str]:
        if not instr.operands:
            return None
        target = instr.operands[0]
        target = target.lstrip("*")
        return target if re.match(r"^[A-Za-z_\.][A-Za-z0-9_\.]*$", target) else None
