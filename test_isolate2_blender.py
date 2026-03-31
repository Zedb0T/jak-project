import ctypes as ct
import os

base = r"C:\Users\ZedBo\OneDrive\Documents\GitHub\jak-project\build_static\bin\Release"

# Can we load test_runtime AND jakopengoal?
print("Loading test_runtime.dll first...")
try:
    dll1 = ct.CDLL(os.path.join(base, "test_runtime.dll"), winmode=0)
    print(f"  OK: test_runtime.dll")
except Exception as e:
    print(f"  FAIL: {e}")

print("Loading jakopengoal.dll...")
try:
    dll2 = ct.CDLL(os.path.join(base, "jakopengoal.dll"), winmode=0)
    print(f"  OK: jakopengoal.dll")
except Exception as e:
    print(f"  FAIL: {e}")

print("DONE")
import bpy
bpy.ops.wm.quit_blender()
