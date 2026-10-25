"""Compile and run the native C++ protocol test against the real component sources.

This keeps the on-device decoder (components/hoymiles_dtu_base/protocol.cpp) under test
without a parallel Python re-implementation. Skipped when no C++ compiler exists.
"""

import ast
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).parents[2]
COMPONENT = REPO / "components" / "hoymiles_dtu_base"
NATIVE = REPO / "tests" / "native"


def _find_compiler() -> str | None:
    for candidate in ("c++", "g++", "clang++"):
        path = shutil.which(candidate)
        if path is not None:
            return path
    return None


def test_native_protocol(tmp_path):
    compiler = _find_compiler()
    if compiler is None:
        pytest.skip("no C++ compiler available")

    binary = tmp_path / "test_protocol"
    compile_cmd = [
        compiler,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-I{COMPONENT}",
        f"-I{NATIVE / 'stubs'}",
        str(NATIVE / "test_protocol.cpp"),
        str(COMPONENT / "protocol.cpp"),
        "-o",
        str(binary),
    ]
    compile_result = subprocess.run(
        compile_cmd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    assert compile_result.returncode == 0, compile_result.stdout

    run_result = subprocess.run(
        [str(binary)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    assert run_result.returncode == 0, run_result.stdout


def _model_channel_counts() -> dict[str, int]:
    """MODEL_CHANNEL_COUNTS from the component, evaluated without importing esphome."""
    source = (COMPONENT / "__init__.py").read_text()
    for node in ast.parse(source).body:
        if isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id == "MODEL_CHANNEL_COUNTS"
            for t in node.targets
        ):
            expression = ast.Expression(node.value)
            return eval(compile(expression, "MODEL_CHANNEL_COUNTS", "eval"), {})
    raise AssertionError("MODEL_CHANNEL_COUNTS not found")


def test_channel_counts_match_cpp(tmp_path):
    """The Python channel table (config warnings) must agree with protocol.cpp."""
    compiler = _find_compiler()
    if compiler is None:
        pytest.skip("no C++ compiler available")

    counts = _model_channel_counts()
    lines = "\n".join(
        f'  std::printf("{model} %u\\n", hm_model_channel_count({model.upper()}));'
        for model in counts
    )
    program = tmp_path / "channel_counts.cpp"
    program.write_text(
        '#include "protocol.h"\n#include <cstdio>\n#include <cstring>\n'
        # protocol.cpp needs this ESPHome function; stubbed like in test_protocol.cpp.
        "namespace esphome {\n"
        "void get_mac_address_raw(uint8_t *mac) { memset(mac, 0, 6); }\n"
        "}  // namespace esphome\n"
        "using namespace esphome::hoymiles_dtu;\n"
        f"int main() {{\n{lines}\n  return 0;\n}}\n"
    )
    binary = tmp_path / "channel_counts"
    compile_result = subprocess.run(
        [
            compiler,
            "-std=c++17",
            f"-I{COMPONENT}",
            f"-I{NATIVE / 'stubs'}",
            str(program),
            str(COMPONENT / "protocol.cpp"),
            "-o",
            str(binary),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    assert compile_result.returncode == 0, compile_result.stdout

    output = subprocess.run(
        [str(binary)], text=True, stdout=subprocess.PIPE, check=True
    ).stdout
    pairs = (line.split() for line in output.splitlines())
    cpp_counts = {model: int(count) for model, count in pairs}
    assert cpp_counts == counts
