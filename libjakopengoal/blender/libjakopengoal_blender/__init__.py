"""
libjakopengoal-blender - Blender addon for embedding Jak into a Blender scene.

Inspired by libsm64-blender, this addon uses the libjakopengoal shared library
to run the OpenGOAL runtime headlessly and render Jak's animated mesh in real-time
within Blender's 3D viewport.

Usage:
  1. Install the addon (Edit > Preferences > Add-ons > Install)
  2. Set the Game Data Path in the addon preferences (path to jak-project root)
  3. Place the 3D cursor where you want Jak to spawn
  4. Open the sidebar (N panel) > "Jak" tab
  5. Click "Insert Jak"
  6. Use WASD to move, J/K/L for actions
  7. Click "Remove Jak" or delete the Jak object to stop
"""

bl_info = {
    "name": "libjakopengoal - Jak in Blender",
    "author": "jak-project",
    "description": "Embed Jak (from Jak and Daxter) into your Blender scene",
    "blender": (3, 0, 0),
    "version": (0, 1, 0),
    "location": "View3D > Sidebar > Jak",
    "category": "3D View",
}

import bpy
from bpy.props import StringProperty, FloatProperty, BoolProperty
import os

# Make sure our module can find itself
import importlib
from . import jak
importlib.reload(jak)  # force reload during development


# ---------------------------------------------------------------------------
#  Addon Preferences
# ---------------------------------------------------------------------------

class LIBJAKOPENGOAL_Preferences(bpy.types.AddonPreferences):
    bl_idname = __package__

    game_data_path: StringProperty(
        name="Game Data Path",
        description="Path to the jak-project root directory (containing iso_data/ and out/)",
        subtype='DIR_PATH',
        default="",
    )

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "game_data_path")
        if not self.game_data_path:
            layout.label(text="Set this to your jak-project directory!", icon='ERROR')


# ---------------------------------------------------------------------------
#  Properties
# ---------------------------------------------------------------------------

class LIBJAKOPENGOAL_SceneProperties(bpy.types.PropertyGroup):
    scale: FloatProperty(
        name="Scale",
        description="Scale factor (Blender units to Jak units)",
        default=75.0,
        min=1.0,
        max=500.0,
    )
    camera_follow: BoolProperty(
        name="Camera Follow",
        description="Move the 3D cursor to follow Jak",
        default=True,
    )
    camera_shift: bpy.props.FloatVectorProperty(
        name="Camera Offset",
        description="Camera offset from Jak's origin (in Blender coordinates)",
        default=(0.0, 0.0, 1.0),
        soft_min=-10.0,
        soft_max=10.0,
        step=10,
        precision=3,
        subtype='XYZ',
        unit='LENGTH',
        size=3,
    )


# ---------------------------------------------------------------------------
#  Operators
# ---------------------------------------------------------------------------

class LIBJAKOPENGOAL_OT_InsertJak(bpy.types.Operator):
    bl_idname = "libjakopengoal.insert_jak"
    bl_label = "Insert Jak"
    bl_description = "Spawn Jak at the 3D cursor position"

    def execute(self, context):
        prefs = context.preferences.addons[__package__].preferences
        game_data_path = bpy.path.abspath(prefs.game_data_path)

        if not game_data_path or not os.path.isdir(game_data_path):
            self.report({'ERROR'}, "Invalid Game Data Path! Set it in addon preferences.")
            return {'CANCELLED'}

        # Check for iso_data and out directories
        if not os.path.isdir(os.path.join(game_data_path, "out")):
            self.report({'ERROR'}, f"Cannot find 'out/' in {game_data_path}. "
                        "Make sure the game is compiled.")
            return {'CANCELLED'}

        props = context.scene.libjakopengoal
        jak.JAK_SCALE_FACTOR = props.scale

        try:
            jak.insert_jak(game_data_path, props.scale, props.camera_follow)
        except Exception as e:
            self.report({'ERROR'}, f"Failed to insert Jak: {e}")
            return {'CANCELLED'}

        # Start keyboard control modal
        bpy.ops.libjakopengoal.control_jak('INVOKE_DEFAULT')

        self.report({'INFO'}, "Jak is booting... (check console for progress)")
        return {'FINISHED'}


class LIBJAKOPENGOAL_OT_RemoveJak(bpy.types.Operator):
    bl_idname = "libjakopengoal.remove_jak"
    bl_label = "Remove Jak"
    bl_description = "Remove Jak from the scene and shut down the runtime"

    def execute(self, context):
        jak.stop_jak()

        # Remove the mesh object
        if jak.jak_obj_name and jak.jak_obj_name in bpy.data.objects:
            obj = bpy.data.objects[jak.jak_obj_name]
            bpy.data.objects.remove(obj, do_unlink=True)

        self.report({'INFO'}, "Jak removed.")
        return {'FINISHED'}


class LIBJAKOPENGOAL_OT_ControlJak(bpy.types.Operator):
    """Modal operator to capture keyboard input for controlling Jak."""
    bl_idname = "libjakopengoal.control_jak"
    bl_label = "Control Jak (Keyboard)"
    bl_description = "Use WASD to move Jak, J=Jump, K=Punch, L=Roll"

    def modal(self, context, event):
        # Stop if Jak is gone
        if jak.jak_dll is None or jak.jak_id < 0:
            if jak.jak_obj_name and jak.jak_obj_name not in bpy.data.objects:
                return {'CANCELLED'}

        if event.type == 'ESC':
            # Clear inputs
            for key in jak.input_value:
                jak.input_value[key] = False if isinstance(jak.input_value[key], bool) else 0.0
            return {'CANCELLED'}

        pressed = (event.value == 'PRESS')
        released = (event.value == 'RELEASE')

        # Movement (WASD)
        if event.type == 'W':
            if pressed:   jak.input_value["stick_y"] = -1.0
            if released:  jak.input_value["stick_y"] = 0.0
        elif event.type == 'S':
            if pressed:   jak.input_value["stick_y"] = 1.0
            if released:  jak.input_value["stick_y"] = 0.0
        elif event.type == 'A':
            if pressed:   jak.input_value["stick_x"] = -1.0
            if released:  jak.input_value["stick_x"] = 0.0
        elif event.type == 'D':
            if pressed:   jak.input_value["stick_x"] = 1.0
            if released:  jak.input_value["stick_x"] = 0.0

        # Actions
        elif event.type == 'J':  # X button (jump)
            jak.input_value["button_x"] = pressed
        elif event.type == 'K':  # Square (punch/spin kick)
            jak.input_value["button_square"] = pressed
        elif event.type == 'L':  # Circle (roll/attack)
            jak.input_value["button_circle"] = pressed
        elif event.type == 'I':  # Triangle
            jak.input_value["button_triangle"] = pressed
        elif event.type == 'U':  # L1
            jak.input_value["button_l1"] = pressed
        elif event.type == 'O':  # R1
            jak.input_value["button_r1"] = pressed
        else:
            return {'PASS_THROUGH'}

        return {'RUNNING_MODAL'}

    def invoke(self, context, event):
        context.window_manager.modal_handler_add(self)
        self.report({'INFO'}, "Controlling Jak: WASD=move, J=jump, K=punch, L=roll, ESC=release")
        return {'RUNNING_MODAL'}


# ---------------------------------------------------------------------------
#  UI Panel
# ---------------------------------------------------------------------------

class LIBJAKOPENGOAL_PT_MainPanel(bpy.types.Panel):
    bl_label = "Jak"
    bl_idname = "LIBJAKOPENGOAL_PT_main"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Jak"

    def draw(self, context):
        layout = self.layout
        props = context.scene.libjakopengoal

        # Status
        if jak.jak_dll is not None:
            if jak.jak_id >= 0:
                layout.label(text="Jak is active!", icon='PLAY')
                # Show state info
                box = layout.box()
                box.label(text=f"Position: ({jak.jak_state.position[0]:.1f}, "
                          f"{jak.jak_state.position[1]:.1f}, "
                          f"{jak.jak_state.position[2]:.1f})")
                box.label(text=f"HP: {jak.jak_state.hp:.0%}")
                box.label(text=f"Tris: {jak.jak_geo.num_triangles_used}")
            else:
                layout.label(text="Runtime booting...", icon='TIME')
        else:
            layout.label(text="Jak is not loaded.", icon='PAUSE')

        layout.separator()

        # Settings
        layout.prop(props, "scale")
        layout.prop(props, "camera_follow")
        layout.prop(props, "camera_shift")

        layout.separator()

        # Buttons
        row = layout.row(align=True)
        row.scale_y = 1.5
        if jak.jak_dll is None:
            row.operator("libjakopengoal.insert_jak", icon='PLAY')
        else:
            row.operator("libjakopengoal.remove_jak", icon='CANCEL')

        # Controls help
        layout.separator()
        box = layout.box()
        box.label(text="Controls:", icon='KEYBOARD_SHORTCUT' if hasattr(bpy.types, 'KEYBOARD_SHORTCUT') else 'INFO')
        col = box.column(align=True)
        col.label(text="WASD - Move")
        col.label(text="J - Jump (X)")
        col.label(text="K - Punch (Square)")
        col.label(text="L - Roll (Circle)")
        col.label(text="I - Triangle")
        col.label(text="U/O - L1/R1")
        col.label(text="ESC - Release controls")


# ---------------------------------------------------------------------------
#  Registration
# ---------------------------------------------------------------------------

classes = (
    LIBJAKOPENGOAL_Preferences,
    LIBJAKOPENGOAL_SceneProperties,
    LIBJAKOPENGOAL_OT_InsertJak,
    LIBJAKOPENGOAL_OT_RemoveJak,
    LIBJAKOPENGOAL_OT_ControlJak,
    LIBJAKOPENGOAL_PT_MainPanel,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.libjakopengoal = bpy.props.PointerProperty(type=LIBJAKOPENGOAL_SceneProperties)

    print("[libjakopengoal-blender] Addon registered.")


def unregister():
    # Stop Jak if running
    jak.stop_jak()

    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)

    if hasattr(bpy.types.Scene, 'libjakopengoal'):
        del bpy.types.Scene.libjakopengoal

    print("[libjakopengoal-blender] Addon unregistered.")


if __name__ == "__main__":
    register()
