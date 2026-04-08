# .goalmod File Format Reference

The `.goalmod` format is a simple, human-readable file format for defining mods in OpenGOAL. Each `.goalmod` file describes one mod: its metadata, and the parameter changes it applies when enabled.

## File Discovery

The mod manager scans these directories for `.goalmod` files on startup:

1. `<project>/mods/` - Project-local mods directory (for development)
2. `<config>/OpenGOAL/<game>/mods/` - Per-game user mods
3. `<config>/OpenGOAL/mods/` - Shared user mods (all games)

Where `<config>` is `%APPDATA%` on Windows, `~/.config` on Linux, or `~/Library/Application Support` on macOS.

Files are re-discovered when you click **Rescan Mods** in the ImGui Mod Manager.

## Syntax

- Lines starting with `#` or `;` are **comments**
- Blank lines are ignored
- Sections start with `@section_name`
- Key-value pairs use `key = value` or `key: value` syntax
- Keys are **case-insensitive**
- Values can be: `float` (3.14), `int` (42), `bool` (true/false/on/off/yes/no), or `"quoted string"`

## Sections

### @mod (required)

Metadata about the mod. Must appear before any hooks.

| Field         | Required | Description                                           |
|---------------|----------|-------------------------------------------------------|
| `name`        | yes      | Display name of the mod                               |
| `author`      | no       | Who created the mod                                   |
| `version`     | no       | Version string (e.g. "1.0", "2.3.1")                 |
| `description` | no       | One-line description shown in the Mod Manager UI      |
| `game`        | no       | Game compatibility: `jak1`, `jak2`, `jak3`, or `any` (default: `any`) |

### @on_enable

Actions applied **once** when the mod is enabled. These are automatically reverted when the mod is disabled.

### @on_disable

Actions applied **once** when the mod is disabled. Use this for custom cleanup if the automatic revert behavior of `@on_enable` is not sufficient.

### @on_frame

Actions applied **every frame** while the mod is enabled. Use this for values that may be overwritten by the game each frame (like brightness/contrast). Automatically reverted when the mod is disabled.

## Actions

Inside hook sections, each line is an action:

```
parameter_name = value
```

This sets the named parameter to the given value when the hook fires.

### Revert Syntax (optional)

You can specify a custom revert value with the `->` syntax:

```
parameter_name = value -> revert_value
```

When the mod is disabled, `revert_value` will be used instead of the parameter's default.

## Available Parameters

| Parameter                    | Type  | Default | Description                                  |
|------------------------------|-------|---------|----------------------------------------------|
| `render.target_fps`         | float | 60.0    | Target framerate for the frame limiter        |
| `render.framelimiter`        | bool  | true    | Enable/disable the frame limiter              |
| `render.vsync`               | bool  | true    | Enable/disable vertical sync                  |
| `render.msaa_samples`        | int   | 1       | MSAA sample count (1 = disabled)              |
| `render.no_textures`         | bool  | false   | Strip all textures (debug visualization)      |
| `render.lod_tfrag`           | int   | 0       | TFragment LOD level (0=highest, 2=lowest)     |
| `render.lod_tie`             | int   | 0       | TIE LOD level (0=highest, 2=lowest)           |
| `render.brightness`          | int   | 0       | Brightness adjustment value                   |
| `render.contrast`            | int   | 128     | Contrast adjustment value                     |
| `debug.collision_enable`     | bool  | false   | Show collision mesh overlay                   |
| `debug.collision_wireframe`  | bool  | true    | Render collision as wireframe vs solid         |

New parameters can be registered by calling `ModManager::register_param()` from C++ code.

## Example Mod

```
# My First Mod
# Comments start with # or ;

@mod
name = "Performance Mode"
author = "YourName"
version = "1.0"
description = "Optimizes settings for maximum performance"
game = any

@on_enable
render.framelimiter = false
render.no_textures = false
render.lod_tfrag = 1
render.lod_tie = 1
render.vsync = false
```

## Multiple Mods

Multiple mods can be enabled simultaneously. If two mods modify the same parameter, the most recently enabled mod's value takes priority. When a mod is disabled, the parameter reverts to the next active mod's value, or to the default if no other mod targets it.

## Using the Mod Manager UI

1. Open the ImGui debug toolbar (default key shown in-game)
2. Click **Mods** in the menu bar
3. Toggle mods directly from the dropdown, or open the **Mod Manager** window for details
4. The Mod Manager window shows a list of all discovered mods on the left, with details on the right
5. Click **Rescan Mods** to pick up newly added `.goalmod` files without restarting

## Creating a New Mod

1. Create a `.goalmod` file in any of the mod directories listed above
2. Add a `@mod` section with at least a `name`
3. Add one or more hook sections (`@on_enable`, `@on_disable`, `@on_frame`) with parameter assignments
4. The mod will appear in the Mod Manager on the next rescan or restart
