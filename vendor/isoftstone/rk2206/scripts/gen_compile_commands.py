#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate compile_commands.json for clangd / CLion / Cursor jump-to-definition.

This lite-os GN (2021) cannot pass --export-compile-commands through hb.
After `hb build`, run this script to dump ninja's compilation database and
supplement vendor sample sources that are commented out of BUILD.gn.
"""

from __future__ import print_function

import json
import os
import re
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
VENDOR_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, os.pardir))
# vendor/isoftstone/rk2206/scripts -> repo root is 4 levels up
ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, os.pardir, os.pardir, os.pardir, os.pardir))

COMPILER = os.environ.get(
    "OHOS_ARM_GCC",
    "/home/tools/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-gcc",
)
CXX_COMPILER = COMPILER.replace("arm-none-eabi-gcc", "arm-none-eabi-g++")
NINJA = os.environ.get("NINJA", "/home/tools/ninja/ninja")

GCC_ISYSTEM = [
    "/home/tools/gcc-arm-none-eabi-10.3-2021.10/lib/gcc/arm-none-eabi/10.3.1/include",
    "/home/tools/gcc-arm-none-eabi-10.3-2021.10/lib/gcc/arm-none-eabi/10.3.1/include-fixed",
    "/home/tools/gcc-arm-none-eabi-10.3-2021.10/arm-none-eabi/include",
]

SOURCE_EXTS = (".c", ".cc", ".cpp", ".cxx", ".S", ".s")


def load_ohos_config():
    path = os.path.join(ROOT, "ohos_config.json")
    if not os.path.isfile(path):
        raise SystemExit(
            "ohos_config.json not found. Run `hb set` in the repo root first."
        )
    with open(path, "r") as fh:
        return json.load(fh)


def out_dir(cfg):
    return os.path.join(ROOT, "out", cfg["board"], cfg["product"])


def rewrite_command(command):
    """Drop ccache, pin the cross compiler, add GCC builtin include paths."""
    parts = command.split()
    if parts and os.path.basename(parts[0]) == "ccache":
        parts = parts[1:]
    if not parts:
        return command
    compiler = parts[0]
    base = os.path.basename(compiler)
    if base in ("arm-none-eabi-gcc", "gcc"):
        parts[0] = COMPILER
    elif base in ("arm-none-eabi-g++", "g++"):
        parts[0] = CXX_COMPILER
    extra = []
    for inc in GCC_ISYSTEM:
        extra.extend(["-isystem", inc])
    # Insert after compiler so later -I still override system headers.
    parts[1:1] = extra
    return " ".join(parts)


def abs_source(directory, file_path):
    if os.path.isabs(file_path):
        return os.path.normpath(file_path)
    return os.path.normpath(os.path.join(directory, file_path))


def load_ninja_compdb(build_dir):
    ninja_file = os.path.join(build_dir, "build.ninja")
    if not os.path.isfile(ninja_file):
        raise SystemExit(
            "No build.ninja in %s. Run `hb build` first, then re-run this script."
            % build_dir
        )
    ninja_bin = NINJA if os.path.isfile(NINJA) else "ninja"
    proc = subprocess.run(
        [ninja_bin, "-C", build_dir, "-t", "compdb", "cc", "cxx", "asm"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        check=False,
    )
    if proc.returncode != 0:
        raise SystemExit("ninja -t compdb failed:\n%s" % proc.stderr)
    entries = json.loads(proc.stdout)
    normalized = []
    seen = set()
    for entry in entries:
        directory = entry.get("directory", build_dir)
        src = abs_source(directory, entry["file"])
        if src in seen:
            continue
        seen.add(src)
        item = {
            "directory": directory,
            "file": src,
            "command": rewrite_command(entry["command"]),
        }
        if "output" in entry:
            item["output"] = entry["output"]
        normalized.append(item)
    return normalized, seen


def parse_gn_list(text, key):
    """Return string items of `key = [ ... ]` in a BUILD.gn file."""
    match = re.search(r"%s\s*=\s*\[" % re.escape(key), text)
    if not match:
        return []
    rest = text[match.end():]
    end = rest.find("]")
    if end < 0:
        return []
    block = rest[:end]
    return re.findall(r'"([^"]+)"', block)


def gn_include_flags(sample_dir, build_gn):
    flags = []
    if os.path.isfile(build_gn):
        with open(build_gn, "r") as fh:
            text = fh.read()
        for item in parse_gn_list(text, "include_dirs"):
            item = item.strip().rstrip("/")
            if item.startswith("//"):
                path = os.path.join(ROOT, item[2:])
            else:
                path = os.path.join(sample_dir, item)
            flags.append("-I" + os.path.normpath(path))
    include_dir = os.path.join(sample_dir, "include")
    src_dir = os.path.join(sample_dir, "src")
    if os.path.isdir(include_dir) and ("-I" + include_dir) not in flags:
        flags.append("-I" + include_dir)
    if os.path.isdir(src_dir) and ("-I" + src_dir) not in flags:
        flags.append("-I" + src_dir)
    return flags


def pick_template(entries):
    for entry in entries:
        if entry["file"].endswith(".c") and "vendor/isoftstone/rk2206/samples" in entry["file"]:
            return entry
    for entry in entries:
        if entry["file"].endswith(".c"):
            return entry
    raise SystemExit("compile_commands.json has no C entries to clone flags from.")


def strip_sample_includes(command):
    """Drop -I paths that point at a specific sample so we can replace them."""
    return re.sub(
        r"\s-I(?:\S*)/vendor/isoftstone/rk2206/samples/[^/\s]+/(?:include|src)\S*",
        "",
        command,
    )


def list_sample_sources(samples_root):
    files = []
    for dirpath, dirnames, filenames in os.walk(samples_root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "out")]
        for name in filenames:
            if name.endswith(SOURCE_EXTS):
                files.append(os.path.join(dirpath, name))
    return files


def supplement_samples(entries, seen, build_dir):
    samples_root = os.path.join(VENDOR_DIR, "samples")
    if not os.path.isdir(samples_root):
        return 0
    template = pick_template(entries)
    added = 0
    for src in list_sample_sources(samples_root):
        src = os.path.normpath(src)
        if src in seen:
            continue
        rel = os.path.relpath(src, samples_root)
        sample_name = rel.split(os.sep, 1)[0]
        sample_dir = os.path.join(samples_root, sample_name)
        extra_i = gn_include_flags(sample_dir, os.path.join(sample_dir, "BUILD.gn"))
        command = strip_sample_includes(template["command"])
        # Replace the original source path with this file; keep -c/-o shape.
        command = command.replace(template["file"], src)
        if extra_i:
            # Place sample includes first so local headers win.
            compiler, _, rest = command.partition(" ")
            command = compiler + " " + " ".join(extra_i) + " " + rest
        entries.append(
            {
                "directory": build_dir,
                "file": src,
                "command": command,
                "output": os.path.join(
                    build_dir, "obj", "ide_index", rel + ".o"
                ),
            }
        )
        seen.add(src)
        added += 1
    return added


def main():
    cfg = load_ohos_config()
    build_dir = out_dir(cfg)
    print("[OHOS] out dir: %s" % build_dir)
    entries, seen = load_ninja_compdb(build_dir)
    print("[OHOS] ninja compile commands: %d" % len(entries))
    added = supplement_samples(entries, seen, build_dir)
    print("[OHOS] extra sample sources: %d" % added)

    root_db = os.path.join(ROOT, "compile_commands.json")
    out_db = os.path.join(build_dir, "compile_commands.json")
    payload = json.dumps(entries, indent=2, ensure_ascii=False)
    for dest in (root_db, out_db):
        with open(dest, "w") as fh:
            fh.write(payload)
            fh.write("\n")
        print("[OHOS] wrote %s (%d entries)" % (dest, len(entries)))
    print("[OHOS] Reload the window, then F12 / Go to Definition.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
