"""
gamepad.py - SDL3-based gamepad reader with GameControllerDB support.

Uses SDL3.dll (already in addon lib/) for broad controller compatibility.
Downloads gamecontrollerdb.txt from GitHub on first use for extended mappings.
"""

import ctypes
import os
import platform

# ---------------------------------------------------------------------------
#  SDL3 constants
# ---------------------------------------------------------------------------

SDL_INIT_GAMEPAD = 0x00002000

# SDL_GamepadAxis
SDL_GAMEPAD_AXIS_LEFTX  = 0
SDL_GAMEPAD_AXIS_LEFTY  = 1
SDL_GAMEPAD_AXIS_RIGHTX = 2
SDL_GAMEPAD_AXIS_RIGHTY = 3

# SDL_GamepadButton (SDL3 naming)
SDL_GAMEPAD_BUTTON_SOUTH          = 0   # Xbox A / PS Cross
SDL_GAMEPAD_BUTTON_EAST           = 1   # Xbox B / PS Circle
SDL_GAMEPAD_BUTTON_WEST           = 2   # Xbox X / PS Square
SDL_GAMEPAD_BUTTON_NORTH          = 3   # Xbox Y / PS Triangle
SDL_GAMEPAD_BUTTON_BACK           = 4
SDL_GAMEPAD_BUTTON_GUIDE          = 5
SDL_GAMEPAD_BUTTON_START          = 6
SDL_GAMEPAD_BUTTON_LEFT_STICK     = 7
SDL_GAMEPAD_BUTTON_RIGHT_STICK    = 8
SDL_GAMEPAD_BUTTON_LEFT_SHOULDER  = 9
SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER = 10
SDL_GAMEPAD_BUTTON_DPAD_UP        = 11
SDL_GAMEPAD_BUTTON_DPAD_DOWN      = 12
SDL_GAMEPAD_BUTTON_DPAD_LEFT      = 13
SDL_GAMEPAD_BUTTON_DPAD_RIGHT     = 14

# ---------------------------------------------------------------------------
#  Module state
# ---------------------------------------------------------------------------

_sdl3 = None
_gamepad = None            # SDL_Gamepad* handle
_gamepad_connected = False
_initialized = False
_poll_count = 0

STICK_DEADZONE = 7849      # ~24% of 32767

# Current gamepad values (read by jak.py)
gamepad_state = {
    "stick_x": 0.0,
    "stick_y": 0.0,
    "r_stick_x": 0.0,
    "r_stick_y": 0.0,
    "button_x": False,        # PS X / Xbox A  (jump)
    "button_square": False,   # PS Square / Xbox X  (attack)
    "button_circle": False,   # PS Circle / Xbox B  (roll)
    "button_triangle": False, # PS Triangle / Xbox Y
    "button_l1": False,       # Left shoulder
    "button_r1": False,       # Right shoulder
}

# ---------------------------------------------------------------------------
#  Deadzone
# ---------------------------------------------------------------------------

def _apply_deadzone(val, deadzone=STICK_DEADZONE):
    """Apply deadzone and normalize a signed 16-bit axis to [-1, 1]."""
    if abs(val) < deadzone:
        return 0.0
    max_val = 32767.0
    if val > 0:
        return (val - deadzone) / (max_val - deadzone)
    else:
        return (val + deadzone) / (max_val - deadzone)

# ---------------------------------------------------------------------------
#  GameControllerDB download
# ---------------------------------------------------------------------------

def _ensure_controller_db(addon_dir):
    """Download gamecontrollerdb.txt from GitHub if not cached locally."""
    db_path = os.path.join(addon_dir, "gamecontrollerdb.txt")
    if os.path.isfile(db_path):
        return db_path

    url = "https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/master/gamecontrollerdb.txt"
    print(f"[jak-gamepad] Downloading gamecontrollerdb.txt ...")
    try:
        import urllib.request
        urllib.request.urlretrieve(url, db_path)
        size_kb = os.path.getsize(db_path) / 1024
        print(f"[jak-gamepad] Downloaded gamecontrollerdb.txt ({size_kb:.0f} KB)")
        return db_path
    except Exception as e:
        print(f"[jak-gamepad] Download failed: {e}")
        print("[jak-gamepad] Will use SDL3 built-in mappings only")
        return None

# ---------------------------------------------------------------------------
#  SDL3 DLL search
# ---------------------------------------------------------------------------

def _find_sdl3_dll(lib_dir):
    """Search for SDL3.dll in common locations."""
    search_paths = []

    # 1. Addon lib/ directory
    addon_lib = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib")
    search_paths.append(os.path.join(addon_lib, "SDL3.dll"))

    # 2. Same directory as the thin DLL (lib_dir)
    if lib_dir:
        search_paths.append(os.path.join(lib_dir, "SDL3.dll"))

    # 3. build/bin/Release (non-static build)
    if lib_dir:
        project_root = lib_dir
        # Walk up from build_static/bin/Release to project root
        for _ in range(4):
            project_root = os.path.dirname(project_root)
        search_paths.append(os.path.join(project_root, "build", "bin", "Release", "SDL3.dll"))
        search_paths.append(os.path.join(project_root, "build", "bin", "Debug", "SDL3.dll"))

    for path in search_paths:
        if os.path.isfile(path):
            print(f"[jak-gamepad] Found SDL3.dll at: {path}")
            return path

    print(f"[jak-gamepad] SDL3.dll not found. Searched:")
    for p in search_paths:
        print(f"  {p}")
    return None

# ---------------------------------------------------------------------------
#  Public API
# ---------------------------------------------------------------------------

def init(lib_dir=None):
    """Initialize SDL3 gamepad subsystem and load controller mappings.

    Args:
        lib_dir: Directory containing the thin DLL (used to find SDL3.dll)

    Returns True if initialization succeeded.
    """
    global _sdl3, _initialized

    if _initialized:
        return _sdl3 is not None

    _initialized = True

    if platform.system() != "Windows":
        print("[jak-gamepad] Not Windows, gamepad disabled")
        return False

    # Find SDL3.dll
    sdl_path = _find_sdl3_dll(lib_dir)
    if sdl_path is None:
        return False

    # Load SDL3.dll
    try:
        _sdl3 = ctypes.CDLL(sdl_path)
        print(f"[jak-gamepad] Loaded SDL3.dll OK")
    except Exception as e:
        print(f"[jak-gamepad] Failed to load SDL3.dll: {e}")
        _sdl3 = None
        return False

    # Declare function signatures
    try:
        # Init / Quit
        _sdl3.SDL_Init.argtypes = [ctypes.c_uint32]
        _sdl3.SDL_Init.restype = ctypes.c_bool  # SDL3 returns bool (true=success)

        _sdl3.SDL_QuitSubSystem.argtypes = [ctypes.c_uint32]
        _sdl3.SDL_QuitSubSystem.restype = None

        # Mappings
        _sdl3.SDL_AddGamepadMappingsFromFile.argtypes = [ctypes.c_char_p]
        _sdl3.SDL_AddGamepadMappingsFromFile.restype = ctypes.c_int

        # Enumeration
        _sdl3.SDL_HasGamepad.argtypes = []
        _sdl3.SDL_HasGamepad.restype = ctypes.c_bool

        _sdl3.SDL_GetGamepads.argtypes = [ctypes.POINTER(ctypes.c_int)]
        _sdl3.SDL_GetGamepads.restype = ctypes.POINTER(ctypes.c_uint32)

        # Open / Close
        _sdl3.SDL_OpenGamepad.argtypes = [ctypes.c_uint32]
        _sdl3.SDL_OpenGamepad.restype = ctypes.c_void_p

        _sdl3.SDL_CloseGamepad.argtypes = [ctypes.c_void_p]
        _sdl3.SDL_CloseGamepad.restype = None

        # Read state
        _sdl3.SDL_GetGamepadAxis.argtypes = [ctypes.c_void_p, ctypes.c_int]
        _sdl3.SDL_GetGamepadAxis.restype = ctypes.c_int16

        _sdl3.SDL_GetGamepadButton.argtypes = [ctypes.c_void_p, ctypes.c_int]
        _sdl3.SDL_GetGamepadButton.restype = ctypes.c_bool

        # Events
        _sdl3.SDL_PumpEvents.argtypes = []
        _sdl3.SDL_PumpEvents.restype = None

        # Memory
        _sdl3.SDL_free.argtypes = [ctypes.c_void_p]
        _sdl3.SDL_free.restype = None

        # Error
        _sdl3.SDL_GetError.argtypes = []
        _sdl3.SDL_GetError.restype = ctypes.c_char_p

    except Exception as e:
        print(f"[jak-gamepad] Failed to set up SDL3 function signatures: {e}")
        _sdl3 = None
        return False

    # Initialize gamepad subsystem
    if not _sdl3.SDL_Init(SDL_INIT_GAMEPAD):
        err = _sdl3.SDL_GetError()
        err_str = err.decode('utf-8', errors='replace') if err else "unknown"
        print(f"[jak-gamepad] SDL_Init(SDL_INIT_GAMEPAD) failed: {err_str}")
        _sdl3 = None
        return False

    print("[jak-gamepad] SDL3 gamepad subsystem initialized")

    # Load gamecontrollerdb.txt mappings
    addon_dir = os.path.dirname(os.path.abspath(__file__))
    db_path = _ensure_controller_db(addon_dir)
    if db_path and os.path.isfile(db_path):
        n = _sdl3.SDL_AddGamepadMappingsFromFile(db_path.encode('utf-8'))
        if n >= 0:
            print(f"[jak-gamepad] Loaded {n} controller mappings from gamecontrollerdb.txt")
        else:
            err = _sdl3.SDL_GetError()
            err_str = err.decode('utf-8', errors='replace') if err else "unknown"
            print(f"[jak-gamepad] Failed to load mappings: {err_str}")

    # Try to open the first available gamepad
    _try_open_gamepad()

    return True


def _try_open_gamepad():
    """Try to detect and open the first available gamepad."""
    global _gamepad, _gamepad_connected

    if _sdl3 is None:
        return

    # Close any existing gamepad
    if _gamepad:
        _sdl3.SDL_CloseGamepad(_gamepad)
        _gamepad = None
        _gamepad_connected = False

    count = ctypes.c_int(0)
    ids = _sdl3.SDL_GetGamepads(ctypes.byref(count))

    if count.value > 0 and ids:
        instance_id = ids[0]
        _gamepad = _sdl3.SDL_OpenGamepad(instance_id)
        if _gamepad:
            _gamepad_connected = True
            print(f"[jak-gamepad] Opened gamepad (instance_id={instance_id})")
        else:
            err = _sdl3.SDL_GetError()
            err_str = err.decode('utf-8', errors='replace') if err else "unknown"
            print(f"[jak-gamepad] Failed to open gamepad: {err_str}")
        _sdl3.SDL_free(ids)
    else:
        if ids:
            _sdl3.SDL_free(ids)
        print("[jak-gamepad] No gamepads detected")


def poll_gamepad():
    """Poll SDL3 for the current gamepad state. Call once per tick.

    Returns True if a gamepad is connected and was read successfully.
    """
    global _gamepad, _gamepad_connected, _poll_count

    if _sdl3 is None:
        return False

    # Pump SDL events (required for gamepad state updates)
    _sdl3.SDL_PumpEvents()

    # If no gamepad open, try to find one periodically
    if not _gamepad:
        _poll_count += 1
        if _poll_count % 60 == 0:  # check every ~2 seconds at 30fps
            _try_open_gamepad()
        if not _gamepad:
            return False

    # Read analog sticks
    lx = _sdl3.SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_LEFTX)
    ly = _sdl3.SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_LEFTY)
    rx = _sdl3.SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_RIGHTX)
    ry = _sdl3.SDL_GetGamepadAxis(_gamepad, SDL_GAMEPAD_AXIS_RIGHTY)

    gamepad_state["stick_x"]   = _apply_deadzone(lx)
    gamepad_state["stick_y"]   = _apply_deadzone(ly)  # SDL3: positive Y = down (matches our convention)
    gamepad_state["r_stick_x"] = _apply_deadzone(rx)
    gamepad_state["r_stick_y"] = _apply_deadzone(ry)

    # Read buttons
    gamepad_state["button_x"]        = bool(_sdl3.SDL_GetGamepadButton(_gamepad, SDL_GAMEPAD_BUTTON_SOUTH))
    gamepad_state["button_circle"]   = bool(_sdl3.SDL_GetGamepadButton(_gamepad, SDL_GAMEPAD_BUTTON_EAST))
    gamepad_state["button_square"]   = bool(_sdl3.SDL_GetGamepadButton(_gamepad, SDL_GAMEPAD_BUTTON_WEST))
    gamepad_state["button_triangle"] = bool(_sdl3.SDL_GetGamepadButton(_gamepad, SDL_GAMEPAD_BUTTON_NORTH))
    gamepad_state["button_l1"]       = bool(_sdl3.SDL_GetGamepadButton(_gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))
    gamepad_state["button_r1"]       = bool(_sdl3.SDL_GetGamepadButton(_gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))

    _gamepad_connected = True
    _poll_count += 1

    # Debug: log first few polls
    if _poll_count <= 3:
        print(f"[jak-gamepad] Poll #{_poll_count}: stick=({gamepad_state['stick_x']:.2f}, {gamepad_state['stick_y']:.2f})")

    return True


def is_connected():
    """Return True if a gamepad was detected on last poll."""
    return _gamepad_connected


def shutdown():
    """Clean up SDL3 gamepad resources."""
    global _gamepad, _gamepad_connected, _initialized

    if _sdl3 is not None and _gamepad is not None:
        try:
            _sdl3.SDL_CloseGamepad(_gamepad)
        except:
            pass
        _gamepad = None

    if _sdl3 is not None:
        try:
            _sdl3.SDL_QuitSubSystem(SDL_INIT_GAMEPAD)
        except:
            pass

    _gamepad_connected = False
    _initialized = False
    print("[jak-gamepad] Shutdown complete")
