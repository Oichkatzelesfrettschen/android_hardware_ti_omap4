#!/usr/bin/env python3
"""Compile the production DOMX ownership functions with host boundary doubles."""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess


def extract_function(source, name):
    pattern = rf"^(?:static )?(?:RPC_OMX_ERRORTYPE|OMX_ERRORTYPE|MEMPLUGIN_ERRORTYPE) {name}\([^;]+?^\{{.*?^\}}"
    matches = re.findall(pattern, source, re.MULTILINE | re.DOTALL)
    if len(matches) != 1:
        raise ValueError(f"expected one definition of {name}, found {len(matches)}")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="cc")
    arguments = parser.parse_args()
    test_directory = Path(__file__).resolve().parent
    repository = test_directory.parents[2]
    source_path = repository / "domx/domx/omx_proxy_common/src/omx_proxy_common.c"
    source = source_path.read_text()
    header = (repository / "domx/domx/omx_proxy_common.h").read_text()
    macros = header[
        header.index("#define PROXY_assert") : header.index(
            "typedef OMX_ERRORTYPE(*PROXY_EMPTYBUFFER_DONE)"
        )
    ]
    functions = "\n\n".join(
        extract_function(source, name)
        for name in (
            "RPC_RegisterBuffer",
            "RPC_UnRegisterBuffer",
            "PROXY_AllocateBuffer",
            "PROXY_UseBuffer",
            "PROXY_FreeBuffer",
        )
    )
    arguments.output.mkdir(parents=True, exist_ok=True)
    generated_path = arguments.output / "ownership_production.inc"
    generated_path.write_text(macros + "\n" + functions + "\n")
    plugin_source_path = repository / "domx/domx/plugins/memplugin_ion.c"
    plugin_source = plugin_source_path.read_text()
    plugin_header = (repository / "domx/domx/plugins/inc/memplugin_ion.h").read_text()
    plugin_macros = "\n".join(
        re.search(
            r"^#define " + name + r".*?^\}while\(0\)", content, re.MULTILINE | re.DOTALL
        )[0]
        for name, content in (
            ("MEMPLUGIN_ION_PARAMS_INIT", plugin_header),
            ("MEMPLUGIN_ION_PARAMS_COPY", plugin_source),
        )
    )
    (arguments.output / "plugin_production.inc").write_text(
        plugin_macros
        + "\n"
        + "\n".join(
            extract_function(plugin_source, name)
            for name in ("MemPlugin_ION_Alloc", "MemPlugin_ION_Free")
        )
    )
    for path in (source_path, plugin_source_path):
        print(
            f"source={path} sha256={hashlib.sha256(path.read_bytes()).hexdigest()}",
            flush=True,
        )
    failed = False
    for test_name in ("test", "ion_test"):
        executable = arguments.output / test_name
        command = [
            arguments.compiler,
            "-m32",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-g",
            "-O1",
            "-fsanitize=undefined",
            "-fno-sanitize-recover=all",
            "-I",
            str(arguments.output),
            str(test_directory / f"{test_name}.c"),
            "-o",
            str(executable),
        ]
        print("compile=" + " ".join(command), flush=True)
        subprocess.run(command, check=True)
        failed |= subprocess.run([str(executable)], check=False).returncode != 0
    raise SystemExit(int(failed))


if __name__ == "__main__":
    main()
