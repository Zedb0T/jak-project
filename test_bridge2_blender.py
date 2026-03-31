import ctypes as ct, os
base = r"C:\Users\ZedBo\OneDrive\Documents\GitHub\jak-project\build_static\bin\Release"
for name in ["test_bridge2.dll"]:
    try:
        dll = ct.CDLL(os.path.join(base, name), winmode=0)
        print(f"OK:   {name} (ping={dll.test_ping()})")
    except Exception as e:
        print(f"FAIL: {name} -> {e}")
print("DONE")
import bpy; bpy.ops.wm.quit_blender()
