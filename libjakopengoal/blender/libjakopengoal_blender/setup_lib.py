"""
setup_lib.py - Copy required DLLs into the addon's lib/ folder.

Run this once after building the project:
    python setup_lib.py [path_to_build_bin_release]

If no path is given, defaults to ../../../build/bin/Release relative to this script.
"""

import os
import sys
import shutil

# DLLs required by jakopengoal.dll (from the jak-project build)
REQUIRED_DLLS = [
    "jakopengoal.dll",
    "common.dll",
    "fmt.dll",
    "mman.dll",
    "lzokay.dll",
    "replxx.dll",
    "stb_image.dll",
    "tiny_gltf.dll",
    "draco.dll",
    "tree-sitter.dll",
    "sqlite3.dll",
    "libcurl.dll",
    "libtinyfiledialogs.dll",
    "discord-rpc.dll",
    "imgui.dll",
    "SDL3.dll",
]


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    lib_dir = os.path.join(script_dir, "lib")

    if len(sys.argv) > 1:
        src_dir = sys.argv[1]
    else:
        # Default: relative to jak-project root
        src_dir = os.path.join(script_dir, "..", "..", "..", "build", "bin", "Release")
        src_dir = os.path.normpath(src_dir)

    if not os.path.isdir(src_dir):
        print(f"ERROR: Source directory not found: {src_dir}")
        print(f"Build the project first, then run: python {__file__} <path_to_build_bin_release>")
        sys.exit(1)

    os.makedirs(lib_dir, exist_ok=True)

    copied = 0
    missing = []
    for dll in REQUIRED_DLLS:
        src = os.path.join(src_dir, dll)
        dst = os.path.join(lib_dir, dll)
        if os.path.isfile(src):
            shutil.copy2(src, dst)
            size_mb = os.path.getsize(dst) / (1024 * 1024)
            print(f"  Copied {dll} ({size_mb:.1f} MB)")
            copied += 1
        else:
            missing.append(dll)
            print(f"  WARNING: {dll} not found in {src_dir}")

    print(f"\nCopied {copied}/{len(REQUIRED_DLLS)} DLLs to {lib_dir}")
    if missing:
        print(f"Missing: {', '.join(missing)}")
    else:
        print("All DLLs copied successfully!")


if __name__ == "__main__":
    main()
