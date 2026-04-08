#pragma once

/*!
 * @file goalmod_types.h
 * Types and structures for the .goalmod modding system.
 */

#include <string>
#include <variant>
#include <vector>

#include "third-party/filesystem.hpp"
namespace fs = ghc::filesystem;

namespace mods {

/// The value types a mod parameter can hold.
using ModValue = std::variant<float, int, bool, std::string>;

/// A single action that a mod performs (set a named parameter to a value).
struct ModAction {
  std::string target;  // e.g. "render.fog_intensity", "game.speed"
  ModValue value;
  ModValue revert_value;  // value to restore when mod is disabled
};

/// When a set of actions should be applied.
enum class ModHook {
  OnEnable,   // applied once when the mod is enabled
  OnDisable,  // applied once when the mod is disabled
  OnFrame,    // applied every frame while enabled
};

/// A group of actions bound to a specific hook.
struct ModHookEntry {
  ModHook hook;
  std::vector<ModAction> actions;
};

/// Full definition of a mod parsed from a .goalmod file.
struct ModDefinition {
  std::string name;
  std::string author;
  std::string version;
  std::string description;
  std::string game;  // "jak1", "jak2", "jak3", "any"
  std::vector<ModHookEntry> hooks;
};

/// Runtime state for a loaded mod.
struct ModInstance {
  ModDefinition definition;
  fs::path file_path;
  bool enabled = false;
  bool failed = false;
  std::string error_message;
};

}  // namespace mods
