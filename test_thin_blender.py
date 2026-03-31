import ctypes as ct, os
path = r"C:\Users\ZedBo\OneDrive\Documents\GitHub\jak-project\build_static\bin\Release\jakopengoal_thin.dll"
try:
    dll = ct.CDLL(path, winmode=0)
    dll.jak_is_ready.argtypes = []
    dll.jak_is_ready.restype = ct.c_bool
    ready = dll.jak_is_ready()
    print(f"OK: jakopengoal_thin.dll loaded! jak_is_ready={ready}")
except Exception as e:
    print(f"FAIL: {e}")
print("DONE")
import bpy; bpy.ops.wm.quit_blender()
