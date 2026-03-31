import ctypes as ct
import os

base = r"C:\Users\ZedBo\OneDrive\Documents\GitHub\jak-project\build_static\bin\Release"

for name in ["test_full.dll", "jakopengoal.dll"]:
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
