"""Quick test to debug DLL loading outside of Blender."""
import ctypes as ct
import os
import sys

lib_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "libjakopengoal_blender", "lib")
print(f"lib_dir: {lib_dir}")
print(f"exists: {os.path.isdir(lib_dir)}")
print()

# Add to search path
os.add_dll_directory(lib_dir)
os.environ["PATH"] = lib_dir + os.pathsep + os.environ.get("PATH", "")

# Try loading each dependency individually
dlls = [
    "mman.dll",
    "fmt.dll",
    "lzokay.dll",
    "stb_image.dll",
    "sqlite3.dll",
    "replxx.dll",
    "tree-sitter.dll",
    "draco.dll",
    "tiny_gltf.dll",
    "libcurl.dll",
    "libtinyfiledialogs.dll",
    "discord-rpc.dll",
    "imgui.dll",
    "SDL3.dll",
    "common.dll",
    "jakopengoal.dll",
]

for dll in dlls:
    path = os.path.join(lib_dir, dll)
    try:
        handle = ct.CDLL(path, winmode=0)
        print(f"  OK: {dll}")
    except Exception as e:
        print(f"  FAIL: {dll} -> {e}")
