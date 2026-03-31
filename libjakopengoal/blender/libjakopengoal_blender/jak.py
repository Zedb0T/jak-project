"""
jak.py - Core module for libjakopengoal Blender integration.

Handles DLL loading via ctypes, Jak lifecycle management,
mesh updates, and collision surface extraction from Blender scene.

Mirrors the architecture of libsm64-blender's mario.py.
"""

import ctypes as ct
import os
import platform
import math
import mathutils
import bpy
import bmesh

# ---------------------------------------------------------------------------
#  Constants
# ---------------------------------------------------------------------------

JAK_TEXTURE_WIDTH  = 1024
JAK_TEXTURE_HEIGHT = 512
JAK_GEO_MAX_TRIANGLES = 4096

JAK_SCALE_FACTOR = 75.0  # Blender units -> Jak units (tweakable)

# Surface types
JAK_SURFACE_DEFAULT = 0
JAK_SURFACE_ICE     = 1
JAK_SURFACE_STONE   = 2
JAK_SURFACE_WOOD    = 3
JAK_SURFACE_GRASS   = 4
JAK_SURFACE_SAND    = 5
JAK_SURFACE_METAL   = 6
JAK_SURFACE_DIRT    = 7

# Button flags
JAK_BUTTON_X        = (1 << 14)
JAK_BUTTON_SQUARE   = (1 << 15)
JAK_BUTTON_CIRCLE   = (1 << 13)
JAK_BUTTON_TRIANGLE = (1 << 12)
JAK_BUTTON_L1       = (1 << 10)
JAK_BUTTON_R1       = (1 << 11)

# ---------------------------------------------------------------------------
#  ctypes structure definitions (must match libjakopengoal.h exactly)
# ---------------------------------------------------------------------------

class JakSurface(ct.Structure):
    _fields_ = [
        ("type",     ct.c_int16),
        ("flags",    ct.c_int16),
        ("vertices", (ct.c_float * 3) * 3),   # float[3][3]
        ("normal",   ct.c_float * 3),
    ]

class JakObjectTransform(ct.Structure):
    _fields_ = [
        ("position", ct.c_float * 3),
        ("rotation", ct.c_float * 3),
    ]

class JakSurfaceObject(ct.Structure):
    _fields_ = [
        ("transform",     JakObjectTransform),
        ("surface_count", ct.c_uint32),
        ("surfaces",      ct.POINTER(JakSurface)),
    ]

class JakInputs(ct.Structure):
    _fields_ = [
        ("cam_x",     ct.c_float),
        ("cam_z",     ct.c_float),
        ("stick_x",   ct.c_float),
        ("stick_y",   ct.c_float),
        ("r_stick_x", ct.c_float),
        ("r_stick_y", ct.c_float),
        ("buttons",   ct.c_uint16),
    ]

class JakState(ct.Structure):
    _fields_ = [
        ("position",         ct.c_float * 3),
        ("velocity",         ct.c_float * 3),
        ("face_angle",       ct.c_float),
        ("forward_velocity", ct.c_float),
        ("hp",               ct.c_float),
        ("eco_type",         ct.c_int32),
        ("action",           ct.c_uint32),
        ("anim_id",          ct.c_int32),
        ("anim_frame",       ct.c_int16),
        ("flags",            ct.c_uint32),
        ("orb_count",        ct.c_int32),
        ("buzz_count",       ct.c_int32),
        ("cell_count",       ct.c_int32),
        ("on_ground",        ct.c_bool),
        ("in_water",         ct.c_bool),
    ]

class JakGeometryBuffers(ct.Structure):
    _fields_ = [
        ("position",           ct.POINTER(ct.c_float)),
        ("normal",             ct.POINTER(ct.c_float)),
        ("color",              ct.POINTER(ct.c_float)),
        ("uv",                 ct.POINTER(ct.c_float)),
        ("num_triangles_used", ct.c_uint16),
    ]

    def __init__(self):
        max_verts = JAK_GEO_MAX_TRIANGLES * 3
        self._pos_buf   = (ct.c_float * (max_verts * 3))()
        self._norm_buf  = (ct.c_float * (max_verts * 3))()
        self._color_buf = (ct.c_float * (max_verts * 4))()
        self._uv_buf    = (ct.c_float * (max_verts * 2))()
        self.position = ct.cast(self._pos_buf,   ct.POINTER(ct.c_float))
        self.normal   = ct.cast(self._norm_buf,  ct.POINTER(ct.c_float))
        self.color    = ct.cast(self._color_buf, ct.POINTER(ct.c_float))
        self.uv       = ct.cast(self._uv_buf,   ct.POINTER(ct.c_float))
        self.num_triangles_used = 0

class JakTextureInfo(ct.Structure):
    _fields_ = [
        ("width",        ct.c_uint32),
        ("height",       ct.c_uint32),
        ("num_textures", ct.c_uint32),
    ]

# ---------------------------------------------------------------------------
#  Module state
# ---------------------------------------------------------------------------

jak_dll = None          # ctypes DLL handle
jak_id  = -1            # handle from jak_create
jak_obj_name = None     # name of the Blender mesh object
jak_inputs   = JakInputs()
jak_state    = JakState()
jak_geo      = JakGeometryBuffers()
texture_buff = None
scene_origin = [0.0, 0.0, 0.0]
original_fps = 30
original_cursor = [0.0, 0.0, 0.0]
frame_counter = 0
follow_cam = False      # camera follow enabled

# Input state set by the modal keyboard operator
input_value = {
    "stick_x": 0.0, "stick_y": 0.0,
    "button_x": False, "button_square": False,
    "button_circle": False, "button_triangle": False,
    "button_l1": False, "button_r1": False,
}

# ---------------------------------------------------------------------------
#  Coordinate transforms
#  Blender: right-hand, Z-up, Y-forward
#  Jak/SM64: Y-up
#  Blender(x,y,z) -> Jak(x, z, -y)   (then * scale)
#  Jak(x,y,z) -> Blender(x, -z, y)   (then / scale)
# ---------------------------------------------------------------------------

def blender_to_jak(bx, by, bz, origin):
    return (
        (bx - origin[0]) * JAK_SCALE_FACTOR,
        (bz - origin[2]) * JAK_SCALE_FACTOR,
       -(by - origin[1]) * JAK_SCALE_FACTOR,
    )

def jak_to_blender(jx, jy, jz, origin):
    return (
        origin[0] + jx / JAK_SCALE_FACTOR,
        origin[1] - jz / JAK_SCALE_FACTOR,
        origin[2] + jy / JAK_SCALE_FACTOR,
    )

# ---------------------------------------------------------------------------
#  Gamepad support (SDL3 + GameControllerDB)
# ---------------------------------------------------------------------------

_gamepad_available = False
_gp = None

try:
    from . import gamepad as _gp
    print("[jak] Gamepad module imported OK")
except Exception as e:
    print(f"[jak] Gamepad module import failed: {e}")
    _gp = None

def _init_gamepad(lib_dir):
    """Initialize gamepad with SDL3 from the given lib directory."""
    global _gamepad_available
    if _gp is None:
        return
    try:
        _gamepad_available = _gp.init(lib_dir)
        if _gamepad_available:
            print("[jak] Gamepad initialized via SDL3")
        else:
            print("[jak] Gamepad init failed (keyboard only)")
    except Exception as e:
        print(f"[jak] Gamepad init error: {e}")
        _gamepad_available = False

def _shutdown_gamepad():
    """Shut down gamepad subsystem."""
    global _gamepad_available
    if _gp is not None and _gamepad_available:
        try:
            _gp.shutdown()
        except:
            pass
    _gamepad_available = False

def _sample_gamepad():
    """Poll gamepad and apply its state directly to input_value.

    When a gamepad is connected, its values always override keyboard.
    When disconnected, keyboard input_value is used instead.
    Returns True if gamepad was used.
    """
    if not _gamepad_available or _gp is None:
        return False

    if not _gp.poll_gamepad():
        return False

    # Gamepad is connected — use its values directly
    gs = _gp.gamepad_state
    input_value["stick_x"]         = gs["stick_x"]
    input_value["stick_y"]         = gs["stick_y"]
    input_value["button_x"]        = gs["button_x"]
    input_value["button_square"]   = gs["button_square"]
    input_value["button_circle"]   = gs["button_circle"]
    input_value["button_triangle"] = gs["button_triangle"]
    input_value["button_l1"]       = gs["button_l1"]
    input_value["button_r1"]       = gs["button_r1"]
    return True

# ---------------------------------------------------------------------------
#  DLL loading
# ---------------------------------------------------------------------------

def load_dll(lib_dir):
    """Load the jakopengoal_thin shared library and declare function signatures.

    The thin DLL (~30KB) contains only the C API surface and communicates with
    jak_server.exe via shared memory. It has no heavy dependencies and loads
    safely in Blender's process (no static constructor issues).

    jak_server.exe must be in the same directory as the thin DLL — it will be
    launched automatically by jak_global_init().
    """
    global jak_dll

    print(f"[jak-dll] lib_dir: {lib_dir}")

    if platform.system() == "Windows":
        dll_name = "jakopengoal_thin.dll"
    else:
        dll_name = "libjakopengoal_thin.so"

    dll_path = os.path.join(lib_dir, dll_name)
    if not os.path.isfile(dll_path):
        raise FileNotFoundError(f"Cannot find {dll_path}")

    # Verify jak_server.exe exists next to the DLL
    server_name = "jak_server.exe" if platform.system() == "Windows" else "jak_server"
    server_path = os.path.join(lib_dir, server_name)
    if not os.path.isfile(server_path):
        raise FileNotFoundError(
            f"Cannot find {server_name} at {server_path}.\n"
            f"Both {dll_name} and {server_name} must be in the same directory."
        )

    size_kb = os.path.getsize(dll_path) / 1024
    print(f"[jak-dll] Loading {dll_name} ({size_kb:.1f} KB) ...")

    if platform.system() == "Windows":
        try:
            jak_dll = ct.CDLL(dll_path, winmode=0)
            print(f"[jak-dll] OK via ct.CDLL(winmode=0)")
        except Exception as e1:
            try:
                jak_dll = ct.CDLL(dll_path)
                print(f"[jak-dll] OK via ct.CDLL(default)")
            except Exception as e2:
                raise RuntimeError(
                    f"Failed to load {dll_name}.\n"
                    f"  winmode=0: {e1}\n"
                    f"  default:   {e2}"
                )
    else:
        jak_dll = ct.cdll.LoadLibrary(dll_path)

    print(f"[jak-dll] === DLL LOADING COMPLETE ===")

    # Lifecycle
    jak_dll.jak_global_init.argtypes = [ct.c_char_p, ct.POINTER(ct.c_ubyte)]
    jak_dll.jak_global_init.restype  = ct.c_int32

    jak_dll.jak_global_terminate.argtypes = []
    jak_dll.jak_global_terminate.restype  = None

    jak_dll.jak_is_ready.argtypes = []
    jak_dll.jak_is_ready.restype  = ct.c_bool

    # Static collision
    jak_dll.jak_static_surfaces_load.argtypes = [ct.POINTER(JakSurface), ct.c_uint32]
    jak_dll.jak_static_surfaces_load.restype  = None

    # Jak instance
    jak_dll.jak_create.argtypes = [ct.c_float, ct.c_float, ct.c_float]
    jak_dll.jak_create.restype  = ct.c_int32

    jak_dll.jak_delete.argtypes = [ct.c_int32]
    jak_dll.jak_delete.restype  = None

    # Tick
    jak_dll.jak_tick.argtypes = [
        ct.c_int32,
        ct.POINTER(JakInputs),
        ct.POINTER(JakState),
        ct.POINTER(JakGeometryBuffers),
    ]
    jak_dll.jak_tick.restype = None

    # Texture info
    jak_dll.jak_get_texture_info.argtypes = []
    jak_dll.jak_get_texture_info.restype  = JakTextureInfo

    # Position setter
    jak_dll.jak_set_position.argtypes = [ct.c_int32, ct.c_float, ct.c_float, ct.c_float]
    jak_dll.jak_set_position.restype  = None

    return jak_dll

# ---------------------------------------------------------------------------
#  Scene collision extraction
# ---------------------------------------------------------------------------

def get_all_surfaces_from_scene(depsgraph, origin, scale):
    """Extract all mesh triangles from the scene as JakSurface array."""
    surfaces = []

    for obj in bpy.context.scene.collection.all_objects:
        if obj.type != 'MESH':
            continue
        # Skip our own Jak mesh
        if obj.name == jak_obj_name:
            continue

        eval_obj = obj.evaluated_get(depsgraph)
        mesh = eval_obj.to_mesh()
        mesh.calc_loop_triangles()

        world_mat = obj.matrix_world

        for tri in mesh.loop_triangles:
            surf = JakSurface()
            surf.type = JAK_SURFACE_STONE
            surf.flags = 0

            # Check material for surface type override
            if tri.material_index < len(obj.data.materials):
                mat_slot = obj.data.materials[tri.material_index]
                if mat_slot and "jak_surface_type" in mat_slot:
                    surf.type = int(mat_slot["jak_surface_type"])

            for i, vert_idx in enumerate(tri.vertices):
                world_co = world_mat @ mesh.vertices[vert_idx].co
                jx, jy, jz = blender_to_jak(world_co.x, world_co.y, world_co.z, origin)
                surf.vertices[i][0] = jx
                surf.vertices[i][1] = jy
                surf.vertices[i][2] = jz

            # Auto-compute normal
            surf.normal[0] = 0.0
            surf.normal[1] = 0.0
            surf.normal[2] = 0.0

            surfaces.append(surf)

        eval_obj.to_mesh_clear()

    if not surfaces:
        return None, 0

    arr = (JakSurface * len(surfaces))(*surfaces)
    return arr, len(surfaces)

# ---------------------------------------------------------------------------
#  Blender mesh / material setup
# ---------------------------------------------------------------------------

def create_jak_texture_image(tex_data):
    """Create a Blender image from the Jak texture atlas RGBA buffer."""
    img_name = "JakTextureAtlas"
    if img_name in bpy.data.images:
        bpy.data.images.remove(bpy.data.images[img_name])

    img = bpy.data.images.new(img_name, JAK_TEXTURE_WIDTH, JAK_TEXTURE_HEIGHT, alpha=True)

    # Convert RGBA bytes to Blender float pixels [0..1]
    num_pixels = JAK_TEXTURE_WIDTH * JAK_TEXTURE_HEIGHT
    pixels = [0.0] * (num_pixels * 4)
    for i in range(num_pixels):
        # Blender images are bottom-up, our texture is top-down
        src_row = JAK_TEXTURE_HEIGHT - 1 - (i // JAK_TEXTURE_WIDTH)
        src_col = i % JAK_TEXTURE_WIDTH
        src_idx = (src_row * JAK_TEXTURE_WIDTH + src_col) * 4
        pixels[i * 4 + 0] = tex_data[src_idx + 0] / 255.0
        pixels[i * 4 + 1] = tex_data[src_idx + 1] / 255.0
        pixels[i * 4 + 2] = tex_data[src_idx + 2] / 255.0
        pixels[i * 4 + 3] = tex_data[src_idx + 3] / 255.0

    img.pixels[:] = pixels
    img.pack()
    return img


def create_jak_material(tex_image):
    """Create a material for Jak with texture + vertex colors."""
    mat_name = "JakMaterial"
    if mat_name in bpy.data.materials:
        bpy.data.materials.remove(bpy.data.materials[mat_name])

    mat = bpy.data.materials.new(mat_name)
    mat.use_nodes = True
    mat.blend_method = 'CLIP' if hasattr(mat, 'blend_method') else 'OPAQUE'

    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    nodes.clear()

    # Nodes
    out_node = nodes.new('ShaderNodeOutputMaterial')
    out_node.location = (400, 0)

    bsdf = nodes.new('ShaderNodeBsdfPrincipled')
    bsdf.location = (200, 0)

    tex_node = nodes.new('ShaderNodeTexImage')
    tex_node.location = (-200, 0)
    tex_node.image = tex_image

    vcol_node = nodes.new('ShaderNodeVertexColor')
    vcol_node.location = (-200, -200)
    vcol_node.layer_name = "JakColors"

    mix_node = nodes.new('ShaderNodeMixRGB')
    mix_node.location = (0, 0)
    mix_node.blend_type = 'MULTIPLY'
    mix_node.inputs['Fac'].default_value = 1.0

    # Links
    links.new(tex_node.outputs['Color'], mix_node.inputs['Color1'])
    links.new(vcol_node.outputs['Color'], mix_node.inputs['Color2'])
    links.new(mix_node.outputs['Color'], bsdf.inputs['Base Color'])
    links.new(bsdf.outputs['BSDF'], out_node.inputs['Surface'])

    return mat


def create_jak_mesh_object(material):
    """Create the Blender mesh object for Jak with pre-allocated triangles."""
    global jak_obj_name

    obj_name = "Jak"
    # Remove old one if it exists
    if obj_name in bpy.data.objects:
        old = bpy.data.objects[obj_name]
        bpy.data.objects.remove(old, do_unlink=True)

    max_verts = JAK_GEO_MAX_TRIANGLES * 3
    mesh = bpy.data.meshes.new("JakMesh")

    # Create vertices (all at origin initially)
    verts = [(0.0, 0.0, 0.0)] * max_verts
    faces = [(i * 3, i * 3 + 1, i * 3 + 2) for i in range(JAK_GEO_MAX_TRIANGLES)]

    mesh.from_pydata(verts, [], faces)

    # UV layer
    uv_layer = mesh.uv_layers.new(name="JakUV")

    # Vertex color layer
    if hasattr(mesh, 'vertex_colors'):
        vcol_layer = mesh.vertex_colors.new(name="JakColors")
    else:
        vcol_layer = mesh.color_attributes.new(name="JakColors", type='BYTE_COLOR', domain='CORNER')

    mesh.materials.append(material)
    mesh.update()

    obj = bpy.data.objects.new(obj_name, mesh)
    bpy.context.collection.objects.link(obj)
    jak_obj_name = obj_name
    return obj

# ---------------------------------------------------------------------------
#  Mesh update (called each frame)
# ---------------------------------------------------------------------------

def update_jak_mesh(obj):
    """Update the Jak mesh: positions + UVs + vertex colors.

    Uses direct mesh.vertices access (like libsm64-blender) for the first
    N frames so UVs and colors get set, then switches to the fast path.
    """
    mesh = obj.data
    num_tris = jak_geo.num_triangles_used
    num_verts = num_tris * 3

    # Update vertex positions via direct access (fast, no bmesh overhead)
    for i in range(num_tris):
        for v in range(3):
            idx = i * 3 + v
            jx = jak_geo.position[idx * 3 + 0]
            jy = jak_geo.position[idx * 3 + 1]
            jz = jak_geo.position[idx * 3 + 2]
            bx, by, bz = jak_to_blender(jx, jy, jz, scene_origin)
            mesh.vertices[idx].co.x = bx
            mesh.vertices[idx].co.y = by
            mesh.vertices[idx].co.z = bz

    # Update UVs
    uv_layer = mesh.uv_layers.get("JakUV")
    if uv_layer:
        for i in range(num_verts):
            if i < len(uv_layer.data):
                u = jak_geo.uv[i * 2 + 0]
                v = jak_geo.uv[i * 2 + 1]
                uv_layer.data[i].uv = (u, v)

    # Update vertex colors
    vcol = None
    if hasattr(mesh, 'vertex_colors') and "JakColors" in mesh.vertex_colors:
        vcol = mesh.vertex_colors["JakColors"]
    elif hasattr(mesh, 'color_attributes') and "JakColors" in mesh.color_attributes:
        vcol = mesh.color_attributes["JakColors"]

    if vcol:
        for i in range(num_verts):
            if i < len(vcol.data):
                r = jak_geo.color[i * 4 + 0]
                g = jak_geo.color[i * 4 + 1]
                b = jak_geo.color[i * 4 + 2]
                a = jak_geo.color[i * 4 + 3]
                vcol.data[i].color = (r, g, b, a)

    mesh.update()


def update_jak_mesh_fast(obj):
    """Fast path: only update vertex positions using bmesh (skip UVs/colors).

    Only iterates over the triangles actually in use, not all 4096*3 verts.
    """
    mesh = obj.data
    num_tris = jak_geo.num_triangles_used
    num_verts = num_tris * 3

    bm = bmesh.new()
    bm.from_mesh(mesh)
    bm.verts.ensure_lookup_table()

    for i in range(num_verts):
        jx = jak_geo.position[i * 3 + 0]
        jy = jak_geo.position[i * 3 + 1]
        jz = jak_geo.position[i * 3 + 2]
        bx, by, bz = jak_to_blender(jx, jy, jz, scene_origin)
        bm.verts[i].co.x = bx
        bm.verts[i].co.y = by
        bm.verts[i].co.z = bz

    # Collapse remaining verts to origin (degenerate tris become invisible)
    total_verts = len(bm.verts)
    for i in range(num_verts, total_verts):
        bm.verts[i].co.x = 0.0
        bm.verts[i].co.y = 0.0
        bm.verts[i].co.z = 0.0

    bm.to_mesh(mesh)
    bm.free()
    mesh.update()

# ---------------------------------------------------------------------------
#  Main lifecycle functions
# ---------------------------------------------------------------------------

def insert_jak(game_data_path, scale, camera_follow):
    """Initialize the library, load collision, spawn Jak, start ticking."""
    global jak_dll, jak_id, texture_buff, scene_origin
    global original_fps, original_cursor, frame_counter, jak_geo, follow_cam

    follow_cam = camera_follow
    print(f"[jak-insert] === INSERT JAK START ===")
    print(f"[jak-insert] game_data_path: {game_data_path}")
    print(f"[jak-insert] scale: {scale}")

    # Store scene origin at the 3D cursor
    cursor = bpy.context.scene.cursor.location
    scene_origin = [cursor.x, cursor.y, cursor.z]
    original_cursor = [cursor.x, cursor.y, cursor.z]
    print(f"[jak-insert] cursor: {scene_origin}")

    # Store original FPS
    original_fps = bpy.context.scene.render.fps

    # Clean up any prior state
    print(f"[jak-insert] Cleaning up prior state...")
    stop_jak()

    # Load thin DLL — search build directories, then addon lib/
    # Both jakopengoal_thin.dll and jak_server.exe must be in the same directory.
    print(f"[jak-insert] Loading thin DLL...")
    dll_name = "jakopengoal_thin.dll" if platform.system() == "Windows" else "libjakopengoal_thin.so"
    build_static_dir = os.path.join(game_data_path, "build_static", "bin", "Release")
    build_lib_dir = os.path.join(game_data_path, "build", "bin", "Release")
    addon_lib_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib")

    if os.path.isfile(os.path.join(build_static_dir, dll_name)):
        lib_dir = build_static_dir
        print(f"[jak-insert] Using static build directory: {lib_dir}")
    elif os.path.isfile(os.path.join(build_lib_dir, dll_name)):
        lib_dir = build_lib_dir
        print(f"[jak-insert] Using build directory: {lib_dir}")
    elif os.path.isfile(os.path.join(addon_lib_dir, dll_name)):
        lib_dir = addon_lib_dir
        print(f"[jak-insert] Using addon lib directory: {lib_dir}")
    else:
        raise FileNotFoundError(
            f"Cannot find {dll_name} in build_static, build, or addon directories.\n"
            f"Searched:\n  {build_static_dir}\n  {build_lib_dir}\n  {addon_lib_dir}"
        )

    load_dll(lib_dir)
    print(f"[jak-insert] DLL loaded successfully!")

    # Initialize gamepad (SDL3 + GameControllerDB)
    _init_gamepad(lib_dir)

    # Allocate texture buffer
    tex_size = JAK_TEXTURE_WIDTH * JAK_TEXTURE_HEIGHT * 4
    texture_buff = (ct.c_ubyte * tex_size)()

    # Initialize the GOAL runtime
    game_path_bytes = game_data_path.encode('utf-8')
    result = jak_dll.jak_global_init(game_path_bytes, texture_buff)
    if result < 0:
        raise RuntimeError(f"jak_global_init failed with code {result}")

    print("[libjakopengoal-blender] Waiting for GOAL runtime to boot...")

    # We can't block here — Blender would freeze.
    # Instead, register a timer that polls jak_is_ready() and continues setup when ready.
    jak_geo = JakGeometryBuffers()
    frame_counter = 0

    bpy.app.timers.register(
        lambda: _poll_ready(game_data_path, scale, camera_follow),
        first_interval=1.0
    )

    return True


def _poll_ready(game_data_path, scale, camera_follow):
    """Timer callback that polls jak_is_ready() and finishes setup."""
    global jak_id, jak_obj_name

    if jak_dll is None:
        return None  # cancelled

    if not jak_dll.jak_is_ready():
        # Check if server log exists for debugging
        build_dir = os.path.join(bpy.path.abspath(
            bpy.context.preferences.addons.get(__package__, bpy.context.preferences.addons.get(
                "libjakopengoal_blender")).preferences.game_data_path
        ), "build_static", "bin", "Release")
        log_path = os.path.join(build_dir, "jak_server.log")
        if os.path.isfile(log_path):
            try:
                with open(log_path, 'r', errors='replace') as f:
                    lines = f.readlines()
                    last_lines = lines[-5:] if len(lines) >= 5 else lines
                    print(f"[libjakopengoal-blender] Server log tail ({len(lines)} lines):")
                    for line in last_lines:
                        print(f"  {line.rstrip()}")
            except:
                pass
        print("[libjakopengoal-blender] Still waiting for runtime...")
        return 2.0  # poll again in 2 seconds

    print("[libjakopengoal-blender] Runtime ready!")

    # Extract collision from scene
    depsgraph = bpy.context.evaluated_depsgraph_get()
    surf_arr, surf_count = get_all_surfaces_from_scene(depsgraph, scene_origin, JAK_SCALE_FACTOR)

    if surf_arr is not None and surf_count > 0:
        jak_dll.jak_static_surfaces_load(surf_arr, surf_count)
        print(f"[libjakopengoal-blender] Loaded {surf_count} collision triangles")
    else:
        print("[libjakopengoal-blender] WARNING: No collision surfaces found in scene!")

    # Spawn Jak at the origin (in Jak coordinates = 0,0,0)
    jak_id = jak_dll.jak_create(0.0, 0.0, 0.0)
    print(f"[libjakopengoal-blender] Jak created (id={jak_id})")

    # Create texture image and material
    tex_info = jak_dll.jak_get_texture_info()
    print(f"[libjakopengoal-blender] Texture atlas: {tex_info.width}x{tex_info.height}, "
          f"{tex_info.num_textures} textures")

    if tex_info.num_textures > 0:
        tex_image = create_jak_texture_image(texture_buff)
        material = create_jak_material(tex_image)
    else:
        # Fallback: simple material
        material = bpy.data.materials.new("JakMaterial")
        material.diffuse_color = (0.2, 0.5, 1.0, 1.0)

    # Create the mesh object
    jak_mesh_obj = create_jak_mesh_object(material)

    # Set up frame handler for ticking
    bpy.context.scene.render.fps = 30

    # Register the tick handler
    bpy.app.handlers.frame_change_pre.append(tick_jak)

    # Start playback
    bpy.ops.screen.animation_play()

    print("[libjakopengoal-blender] Jak is live!")
    return None  # stop timer


def tick_jak(scene, depsgraph):
    """Called every frame by Blender. Advances Jak simulation and updates mesh."""
    global frame_counter

    if jak_dll is None or jak_id < 0:
        return

    # Check if the Jak object still exists
    if jak_obj_name not in bpy.data.objects:
        print("[libjakopengoal-blender] Jak object deleted, stopping.")
        stop_jak()
        return

    obj = bpy.data.objects[jak_obj_name]

    # Poll gamepad (overrides keyboard input_value when gamepad has active input)
    _sample_gamepad()

    # Build inputs from input_value (set by keyboard modal or gamepad)
    jak_inputs.stick_x = input_value["stick_x"]
    jak_inputs.stick_y = input_value["stick_y"]

    buttons = 0
    if input_value["button_x"]:        buttons |= JAK_BUTTON_X
    if input_value["button_square"]:   buttons |= JAK_BUTTON_SQUARE
    if input_value["button_circle"]:   buttons |= JAK_BUTTON_CIRCLE
    if input_value["button_triangle"]: buttons |= JAK_BUTTON_TRIANGLE
    if input_value["button_l1"]:       buttons |= JAK_BUTTON_L1
    if input_value["button_r1"]:       buttons |= JAK_BUTTON_R1
    jak_inputs.buttons = buttons

    # Also pass right stick from gamepad if available
    if _gamepad_available and _gp.is_connected():
        jak_inputs.r_stick_x = _gp.gamepad_state["r_stick_x"]
        jak_inputs.r_stick_y = _gp.gamepad_state["r_stick_y"]

    # Find the 3D viewport
    view3d = None
    for a in bpy.context.window.screen.areas:
        if a.type == 'VIEW_3D':
            view3d = a
            break

    # Camera look direction from 3D viewport (matches libsm64-blender pattern)
    if view3d is not None:
        r3d = view3d.spaces[0].region_3d
        look_dir = r3d.view_rotation @ mathutils.Vector((0.0, 0.0, -1.0))
        # Convert to Jak coordinate system
        jak_inputs.cam_x = look_dir.x
        jak_inputs.cam_z = -look_dir.y  # Blender Y -> Jak -Z

    # Tick the simulation
    jak_dll.jak_tick(jak_id, ct.byref(jak_inputs), ct.byref(jak_state), ct.byref(jak_geo))

    # Camera follow (matches libsm64-blender pattern)
    if follow_cam and view3d is not None:
        try:
            # Convert Jak position back to Blender coordinates + apply camera_shift offset
            props = bpy.context.scene.libjakopengoal
            bpy.context.scene.cursor.location = (
                scene_origin[0] + jak_state.position[0] / JAK_SCALE_FACTOR + props.camera_shift.x,
                scene_origin[1] - jak_state.position[2] / JAK_SCALE_FACTOR + props.camera_shift.y,
                scene_origin[2] + jak_state.position[1] / JAK_SCALE_FACTOR + props.camera_shift.z,
            )
            # Center viewport on cursor — use Blender 4.x temp_override API
            for region in view3d.regions:
                if region.type == 'WINDOW':
                    with bpy.context.temp_override(area=view3d, region=region):
                        bpy.ops.view3d.view_center_cursor()
                    break
        except Exception as e:
            # Don't let camera follow errors kill the tick handler
            pass

    # Update mesh
    if frame_counter < 15:
        update_jak_mesh(obj)
    else:
        update_jak_mesh_fast(obj)

    frame_counter += 1


def stop_jak():
    """Clean up: delete Jak, terminate runtime, restore settings."""
    global jak_dll, jak_id, jak_obj_name, frame_counter

    # Remove frame handler
    handlers_to_remove = [h for h in bpy.app.handlers.frame_change_pre if h == tick_jak]
    for h in handlers_to_remove:
        bpy.app.handlers.frame_change_pre.remove(h)

    # Stop playback
    try:
        if bpy.context.screen:
            bpy.ops.screen.animation_cancel()
    except:
        pass

    # Delete Jak instance
    if jak_dll is not None and jak_id >= 0:
        try:
            jak_dll.jak_delete(jak_id)
        except:
            pass
        jak_id = -1

    # Terminate runtime
    if jak_dll is not None:
        try:
            jak_dll.jak_global_terminate()
        except:
            pass
        jak_dll = None

    # Shut down gamepad
    _shutdown_gamepad()

    # Restore FPS
    try:
        bpy.context.scene.render.fps = original_fps
    except:
        pass

    # Restore cursor
    try:
        bpy.context.scene.cursor.location = original_cursor
    except:
        pass

    frame_counter = 0
    print("[libjakopengoal-blender] Stopped.")
