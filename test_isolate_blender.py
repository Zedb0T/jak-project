import ctypes as ct
import os

base = r"C:\Users\ZedBo\OneDrive\Documents\GitHub\jak-project\build_static\bin\Release"
dlls = ["test_bare.dll", "test_fmt.dll", "test_common.dll", "test_sdl3.dll", "test_runtime.dll"]

for name in dlls:
    path = os.path.join(base, name)
    try:
        dll = ct.CDLL(path, winmode=0)
        result = dll.test_ping()
        print(f"OK:   {name} (test_ping={result})")
    except Exception as e:
        print(f"FAIL: {name} -> {e}")

print("DONE")
import bpy
bpy.ops.wm.quit_blender()
