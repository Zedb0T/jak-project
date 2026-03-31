"""
Quick test: verify jakopengoal_thin.dll loads in Blender's Python and
jak_server.exe can be launched via jak_global_init().

Run from Blender:
  blender --background --python test_thin_dll.py

Or from the Scripting workspace inside Blender.
"""

import ctypes as ct
import os
import sys
import time
import platform

# Path to the build output
BUILD_DIR = r"C:\Users\ZedBo\OneDrive\Documents\GitHub\jak-project\build_static\bin\Release"
GAME_DATA = r"C:\Users\ZedBo\OneDrive\Documents\GitHub\jak-project"

dll_name = "jakopengoal_thin.dll"
server_name = "jak_server.exe"

dll_path = os.path.join(BUILD_DIR, dll_name)
server_path = os.path.join(BUILD_DIR, server_name)

print(f"[test] DLL path: {dll_path}")
print(f"[test] Server path: {server_path}")
print(f"[test] DLL exists: {os.path.isfile(dll_path)}")
print(f"[test] Server exists: {os.path.isfile(server_path)}")
print(f"[test] DLL size: {os.path.getsize(dll_path) / 1024:.1f} KB")
print(f"[test] Server size: {os.path.getsize(server_path) / (1024*1024):.1f} MB")

# Step 1: Load the DLL
print("\n=== Step 1: Loading thin DLL ===")
try:
    jak = ct.CDLL(dll_path, winmode=0)
    print("[test] DLL loaded successfully!")
except Exception as e:
    print(f"[test] FAILED to load DLL: {e}")
    sys.exit(1)

# Declare function signatures
jak.jak_global_init.argtypes = [ct.c_char_p, ct.POINTER(ct.c_ubyte)]
jak.jak_global_init.restype = ct.c_int32

jak.jak_global_terminate.argtypes = []
jak.jak_global_terminate.restype = None

jak.jak_is_ready.argtypes = []
jak.jak_is_ready.restype = ct.c_bool

# Step 2: Initialize (launches jak_server.exe subprocess)
print("\n=== Step 2: Calling jak_global_init ===")
tex_buf = (ct.c_ubyte * (1024 * 512 * 4))()
result = jak.jak_global_init(GAME_DATA.encode('utf-8'), tex_buf)
print(f"[test] jak_global_init returned: {result}")

if result < 0:
    print(f"[test] FAILED! Error code: {result}")
    sys.exit(1)

# Step 3: Poll for ready
print("\n=== Step 3: Polling jak_is_ready ===")
for i in range(60):  # up to 60 seconds
    ready = jak.jak_is_ready()
    print(f"[test] jak_is_ready() = {ready} (attempt {i+1}/60)")
    if ready:
        break
    time.sleep(1.0)

if not ready:
    print("[test] Runtime did not become ready in 60 seconds")
    print("[test] Shutting down...")
    jak.jak_global_terminate()
    sys.exit(1)

print("\n=== SUCCESS: Runtime is ready! ===")

# Step 4: Clean shutdown
print("\n=== Step 4: Shutting down ===")
jak.jak_global_terminate()
print("[test] Clean shutdown complete!")
