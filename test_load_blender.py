import ctypes as ct
import sys
import os

# Test 1: Can we load ANY custom DLL?
print("=== Test: Can Blender load a custom DLL at all? ===")

# Build a minimal test inline
print(f"Python: {sys.version}")
print(f"Executable: {sys.executable}")

# Test 2: Try loading our static DLL with more error info
dll_path = r"C:\Users\ZedBo\OneDrive\Documents\GitHub\jak-project\build_static\bin\Release\jakopengoal.dll"

kernel32 = ct.WinDLL("kernel32", use_last_error=True)

# Check: are we running with reduced DLL search?
print("\n=== Checking DLL search settings ===")

# Try LoadLibraryExW with different flags
LoadLibraryExW = kernel32.LoadLibraryExW
LoadLibraryExW.argtypes = [ct.c_wchar_p, ct.c_void_p, ct.c_ulong]
LoadLibraryExW.restype = ct.c_void_p

# LOAD_LIBRARY_AS_DATAFILE = 0x2 (loads DLL without running DllMain)
print("\n=== Loading as DATAFILE (no DllMain) ===")
handle = LoadLibraryExW(dll_path, None, 0x2)
if handle:
    print(f"OK: Loaded as datafile (handle=0x{handle:x})")
    kernel32.FreeLibrary(handle)
else:
    err = ct.get_last_error()
    print(f"FAIL: Cannot even load as datafile (error={err})")

# LOAD_WITH_ALTERED_SEARCH_PATH = 0x8
print("\n=== Loading with ALTERED_SEARCH_PATH ===")
handle = LoadLibraryExW(dll_path, None, 0x8)
if handle:
    print(f"OK: Loaded with altered search path (handle=0x{handle:x})")
    kernel32.FreeLibrary(handle)
else:
    err = ct.get_last_error()
    print(f"FAIL: error={err}")

# Normal load
print("\n=== Normal LoadLibraryW ===")
kernel32.LoadLibraryW.argtypes = [ct.c_wchar_p]
kernel32.LoadLibraryW.restype = ct.c_void_p
handle = kernel32.LoadLibraryW(dll_path)
if handle:
    print(f"OK: Loaded normally (handle=0x{handle:x})")
else:
    err = ct.get_last_error()
    print(f"FAIL: error={err}")
    
    # Try to get more info via FormatMessage
    FormatMessageW = kernel32.FormatMessageW
    FormatMessageW.argtypes = [ct.c_ulong, ct.c_void_p, ct.c_ulong, ct.c_ulong, ct.c_wchar_p, ct.c_ulong, ct.c_void_p]
    FormatMessageW.restype = ct.c_ulong
    buf = ct.create_unicode_buffer(1024)
    n = FormatMessageW(0x1000, None, err, 0, buf, 1024, None)
    if n > 0:
        print(f"  Message: {buf.value.strip()}")

import bpy
bpy.ops.wm.quit_blender()
