#!/usr/bin/env python3
"""Run clang-tidy over the hoymiles_dtu_* components with the repo's .clang-tidy.

    scripts/clang-tidy.py               # build the fixtures, then report findings
    scripts/clang-tidy.py --fix         # apply clang-tidy's fix-its
    scripts/clang-tidy.py --no-compile  # reuse the fixtures' existing builds

clang-tidy needs the compiler flags of a real build. The ESP-IDF fixtures (one per
radio) are compiled with ESPHome, whose native IDF build writes a
compile_commands.json. That database compiles ESPHome's copies of the sources with
xtensa GCC, so each entry for a hoymiles_dtu_* source is rewritten:

  * the translation unit is the repo file instead of the build copy, and an include
    overlay makes `esphome/components/hoymiles_dtu_*/...` resolve to the repo too,
    so findings and fixes land in the repo (headers included);
  * GCC-only flags are translated for clang the way ESPHome's own script/clang-tidy
    does: compile as 32-bit x86 pretending to be Xtensa, with the GCC toolchain's
    include directories passed explicitly.

The rewritten database goes to .esphome/clang-tidy/ (git-ignored). Exits non-zero
when clang-tidy reports anything.
"""

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FIXTURE_DIR = REPO / "tests" / "components" / "hoymiles_dtu"
# ESP-IDF fixtures: between them they compile every hoymiles_dtu_* source.
FIXTURES = ("test.esp32-idf.yaml", "test.hms-cmt-idf.yaml")
OUT_DIR = REPO / ".esphome" / "clang-tidy"
HEADER_FILTER = ".*/components/hoymiles_dtu_.*"
# ESPHome pins clang-tidy 22 (requirements_dev.txt); its .clang-tidy is written for
# that release, and other majors add or drop checks.
CLANG_TIDY_MAJOR = 22

# GCC flags clang does not understand (from ESPHome's script/clang-tidy), plus the
# precompiled-header warning: CMake's .gch is GCC-only and is dropped below.
OMIT_FLAGS = {
    "-free",
    "-fipa-pta",
    "-fstrict-volatile-bitfields",
    "-mlongcalls",
    "-mtext-section-literals",
    "-mdisable-hardware-atomics",
    "-mfix-esp32-psram-cache-issue",
    "-mfix-esp32-psram-cache-strategy=memw",
    "-fno-tree-switch-conversion",
    "-freorder-blocks",
    "-fno-jump-tables",
    "-fno-shrink-wrap",
    "-mno-target-align",
    "-fdiagnostics-color=always",
    "-Winvalid-pch",
}
# clang has no usable Xtensa core config, so analyse as 32-bit x86 that claims to be
# Xtensa (same as ESPHome's script/clang-tidy).
CLANG_FLAGS = [
    "-m32",
    "-U__i386__",
    "-U__x86_64__",
    "-D__XTENSA__",
    "-D__XTENSA_EL__",
    "-D_LIBC",
    "-nostdinc",
    "-nostdinc++",
    "-DCLANG_TIDY",
    "-D_GLIBCXX_HAVE_TLS",
    "-Ddeprecated(x)=",
]


def compile_fixtures():
    for fixture in FIXTURES:
        print(f"Compiling {fixture} ...", flush=True)
        subprocess.run(
            ["esphome", "compile", str(FIXTURE_DIR / fixture)],
            cwd=REPO,
            check=True,
            stdout=subprocess.DEVNULL,
        )


def expand_response_files(flags):
    """Inline GCC @file arguments (the IDF toolchain keeps -mlongcalls in one)."""
    out = []
    for flag in flags:
        if flag.startswith("@") and Path(flag[1:]).is_file():
            out += expand_response_files(shlex.split(Path(flag[1:]).read_text()))
        else:
            out.append(flag)
    return out


def gcc_include_dirs(gxx, machine_flags):
    """The toolchain's built-in include path, which -nostdinc hides from clang."""
    lines = subprocess.run(
        [gxx, *machine_flags, "-E", "-x", "c++", "-", "-v"],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        check=True,
    ).stderr.splitlines()
    start = lines.index("#include <...> search starts here:") + 1
    end = lines.index("End of search list.")
    return [os.path.normpath(line.strip()) for line in lines[start:end]]


def clang_arguments(entry, repo_file, overlay):
    args = entry.get("arguments") or shlex.split(entry["command"])
    gxx, flags = args[0], expand_response_files(args[1:])
    out = ["clang++", *CLANG_FLAGS, f"-I{overlay}"]
    skip_next = False
    for i, flag in enumerate(flags):
        if skip_next:
            skip_next = False
        elif flag == "-o" or (flag == "-include" and "cmake_pch" in flags[i + 1]):
            skip_next = True
        elif flag in OMIT_FLAGS or flag.startswith(("-Werror", "-mtune=esp")):
            pass
        elif flag == entry["file"]:
            out.append(str(repo_file))
        else:
            out.append(flag)
    machine = [f for f in flags if f.startswith("-m") and f not in OMIT_FLAGS]
    for directory in gcc_include_dirs(gxx, machine):
        out += ["-isystem", directory]
    return out


def write_database():
    overlay = OUT_DIR / "overlay"
    components = overlay / "esphome" / "components"
    components.mkdir(parents=True, exist_ok=True)
    for component in sorted((REPO / "components").glob("hoymiles_dtu_*")):
        link = components / component.name
        if not link.is_symlink():
            link.symlink_to(component)

    entries = {}
    databases = (FIXTURE_DIR / ".esphome" / "build").glob(
        "*/build/compile_commands.json"
    )
    for database in sorted(databases):
        sources = database.parents[1] / "src" / "esphome" / "components"
        for entry in json.loads(database.read_text()):
            source = Path(entry["file"])
            if source.parent.parent != sources:
                continue
            if not source.parent.name.startswith("hoymiles_dtu_"):
                continue
            repo_file = REPO / "components" / source.parent.name / source.name
            if repo_file in entries or not repo_file.is_file():
                continue
            entries[repo_file] = {
                "directory": entry["directory"],
                "file": str(repo_file),
                "arguments": clang_arguments(entry, repo_file, overlay),
            }

    expected = set((REPO / "components").glob("hoymiles_dtu_*/*.cpp"))
    if missing := sorted(expected - set(entries)):
        names = ", ".join(str(path.relative_to(REPO)) for path in missing)
        sys.exit(f"No compile command for {names}: compile the fixtures first.")
    (OUT_DIR / "compile_commands.json").write_text(
        json.dumps(list(entries.values()), indent=1)
    )
    return sorted(entries)


def find_clang_tidy(requested):
    candidates = [requested] if requested else []
    candidates += [f"clang-tidy-{CLANG_TIDY_MAJOR}", "clang-tidy"]
    for name in candidates:
        if path := shutil.which(name):
            break
    else:
        sys.exit(f"clang-tidy not found (tried {', '.join(candidates)}).")
    version = subprocess.run(
        [path, "--version"], capture_output=True, text=True, check=True
    ).stdout
    match = re.search(r"version (\d+)\.", version)
    major = int(match.group(1)) if match else None
    if major != CLANG_TIDY_MAJOR:
        print(
            f"warning: {path} is clang-tidy {major}, ESPHome uses "
            f"{CLANG_TIDY_MAJOR}: results may differ.",
            file=sys.stderr,
        )
    print(f"Using {path} (clang-tidy {major})", flush=True)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fix", action="store_true", help="apply fix-its")
    parser.add_argument(
        "--no-compile", action="store_true", help="reuse existing fixture builds"
    )
    parser.add_argument(
        "--clang-tidy",
        metavar="PATH",
        help=f"clang-tidy binary (default: clang-tidy-{CLANG_TIDY_MAJOR}, then "
        "clang-tidy)",
    )
    args = parser.parse_args()
    clang_tidy = find_clang_tidy(args.clang_tidy)

    if not args.no_compile:
        compile_fixtures()
    sources = write_database()

    failed = False
    # One file at a time, so a header shared by several sources is fixed only once.
    for source in sources:
        print(f"clang-tidy {source.relative_to(REPO)}", flush=True)
        cmd = [clang_tidy, "-p", str(OUT_DIR), f"--header-filter={HEADER_FILTER}"]
        if args.fix:
            cmd.append("--fix")
        result = subprocess.run(
            [*cmd, "--quiet", str(source)], capture_output=True, text=True
        )
        # clang-tidy prints the number of suppressed warnings in system and library
        # headers on stderr; only show what concerns our files.
        if result.stdout.strip():
            print(result.stdout, end="")
        failed |= result.returncode != 0
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
