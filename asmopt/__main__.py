"""Command line interface for asmopt."""

from __future__ import annotations

import argparse
import sys
from typing import Iterable, Optional

from .core import Optimizer


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="asmopt", description="Assembly optimizer")
    parser.add_argument("input", nargs="?", help="Input assembly file")
    parser.add_argument("-i", "--input", dest="input_file", help="Input assembly file")
    parser.add_argument("-o", "--output", dest="output_file", help="Output assembly file")
    parser.add_argument("-f", "--format", dest="format", choices=["intel", "att"], help="Assembly syntax")
    parser.add_argument("-O0", dest="opt_level", action="store_const", const=0)
    parser.add_argument("-O1", dest="opt_level", action="store_const", const=1)
    parser.add_argument("-O2", dest="opt_level", action="store_const", const=2)
    parser.add_argument("-O3", dest="opt_level", action="store_const", const=3)
    parser.add_argument("-O4", dest="opt_level", action="store_const", const=4)
    parser.add_argument("--enable", action="append", default=[], dest="enable")
    parser.add_argument("--disable", action="append", default=[], dest="disable")
    parser.add_argument("--no-optimize", action="store_true")
    parser.add_argument("--preserve-all", action="store_true")
    parser.add_argument("--report", dest="report")
    parser.add_argument("--stats", action="store_true")
    parser.add_argument("--cfg", dest="cfg")
    parser.add_argument("--dump-ir", action="store_true")
    parser.add_argument("--dump-cfg", action="store_true")
    parser.add_argument("-v", "--verbose", action="count", default=0)
    parser.add_argument("-q", "--quiet", action="store_true")
    parser.add_argument("-m", "--march", dest="march")
    parser.add_argument("--mtune", dest="mtune")
    parser.add_argument(
        "--amd-optimize",
        dest="amd_optimize",
        action=argparse.BooleanOptionalAction,
        default=True,
    )
    return parser


def _read_input(path: Optional[str]) -> str:
    if not path or path == "-":
        return sys.stdin.read()
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def _write_output(path: Optional[str], data: str) -> None:
    if not path or path == "-":
        sys.stdout.write(data)
        return
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(data)


def _apply_options(opt: Optimizer, args: argparse.Namespace) -> None:
    if args.opt_level is not None:
        opt.set_optimization_level(args.opt_level)
    for name in args.enable:
        opt.enable_optimization(name)
    for name in args.disable:
        opt.disable_optimization(name)
    opt.enable_amd_optimizations(args.amd_optimize)
    opt.no_optimize = args.no_optimize
    opt.options["preserve_all"] = args.preserve_all
    opt.options["verbose"] = args.verbose
    opt.options["quiet"] = args.quiet
    opt.options["march"] = args.march
    opt.options["mtune"] = args.mtune


def _emit_report(opt: Optimizer, path: Optional[str]) -> None:
    report = opt.get_report()
    if not path or path == "-":
        sys.stderr.write(report)
        return
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(report)


def _emit_stats(opt: Optimizer) -> None:
    stats = opt.get_statistics()
    sys.stderr.write(
        "Statistics:\n"
        f"  original_lines: {stats['original_lines']}\n"
        f"  optimized_lines: {stats['optimized_lines']}\n"
        f"  replacements: {stats['replacements']}\n"
        f"  removals: {stats['removals']}\n"
    )


def _emit_cfg(opt: Optimizer, path: str) -> None:
    dot = opt.dump_cfg_dot()
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(dot)


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    input_path = args.input_file or args.input
    if not input_path and sys.stdin.isatty():
        parser.error("input file required")

    opt = Optimizer(
        architecture=args.march or "x86-64",
        target_cpu=args.mtune or "generic",
        format=args.format,
    )
    _apply_options(opt, args)
    opt.load_string(_read_input(input_path))
    opt.optimize()

    if args.dump_ir:
        sys.stderr.write(opt.dump_ir_text())
    if args.dump_cfg:
        sys.stderr.write(opt.dump_cfg_text())
    if args.cfg:
        _emit_cfg(opt, args.cfg)
    if args.report:
        _emit_report(opt, args.report)
    if args.stats:
        _emit_stats(opt)

    _write_output(args.output_file, opt.get_assembly())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
