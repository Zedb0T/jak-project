import ctypes as ct
import sys
import os

dll_path = r"C:\Users\ZedBo\OneDrive\Documents\GitHub\jak-project\build_static\bin\Release\jakopengoal.dll"

kernel32 = ct.WinDLL("kernel32", use_last_error=True)

# Try normal LoadLibraryW
kernel32.LoadLibraryW.argtypes = [ct.c_wchar_p]
kernel32.LoadLibraryW.restype = ct.c_void_p

print("=== Attempting LoadLibraryW ===")
handle = kernel32.LoadLibraryW(dll_path)
if handle:
    print(f"OK: handle=0x{handle:x}")
else:
    err = ct.get_last_error()
    print(f"FAIL: error={err}")

# Try ct.CDLL
print("\n=== Attempting ct.CDLL(winmode=0) ===")
try:
    dll = ct.CDLL(dll_path, winmode=0)
    print("OK: ct.CDLL loaded!")
except Exception as e:
    print(f"FAIL: {e}")

# Try ct.CDLL default
print("\n=== Attempting ct.CDLL(default) ===")
try:
    dll = ct.CDLL(dll_path)
    print("OK: ct.CDLL loaded!")
except Exception as e:
    print(f"FAIL: {e}")

print("\n=== DONE ===")
import bpy
bpy.ops.wm.quit_blender()
