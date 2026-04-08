#pragma once

/*!
 * @file mod_manager.h
 * Central mod manager: discovers, loads, enables/disables .goalmod files at runtime.
 * Maintains a registry of modifiable parameters that mods can target.
 */

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/mods/goalmod_types.h"

#include "common/versions/versions.h"

namespace mods {

/// A registered parameter that mods can modify.
struct RegisteredParam {
  std::string name;
  ModValue default_value;
  ModValue current_value;
  /// Callback invoked when the value changes. The variant holds the new value.
  std::function<void(const ModValue&)> on_change;
};

class ModManager {
 public:
  static ModManager& get();

  /// Initialize the mod manager for a given game version.
  /// Scans mod directories and loads all .goalmod files.
  void init(GameVersion version);

  /// Shut down and revert all active mods.
  void shutdown();

  /// Called once per frame from the render thread.
  void tick();

  /// Rescan mod directories for new/changed .goalmod files.
  void rescan();

  // --- Parameter Registry ---

  /// Register a modifiable parameter with a default value and change callback.
  void register_param(const std::string& name,
                      ModValue default_value,
                      std::function<void(const ModValue&)> on_change);

  /// Get the current value of a registered parameter.
  const ModValue* get_param(const std::string& name) const;

  /// Get all registered parameter names (for debug display).
  std::vector<std::string> get_param_names() const;

  // --- Mod Control ---

  /// Enable a mod by index. Returns false if the mod has errors.
  bool enable_mod(size_t index);

  /// Disable a mod by index.
  void disable_mod(size_t index);

  /// Toggle a mod by index.
  void toggle_mod(size_t index);

  /// Get the list of all loaded mods.
  const std::vector<ModInstance>& get_mods() const { return m_mods; }

  /// Get mutable mod list (for ImGui).
  std::vector<ModInstance>& get_mods_mut() { return m_mods; }

  bool is_initialized() const { return m_initialized; }
  GameVersion get_game_version() const { return m_game_version; }

 private:
  ModManager() = default;

  void discover_mods();
  void apply_actions(const std::vector<ModAction>& actions);
  void revert_param(const std::string& name);
  bool is_mod_compatible(const ModDefinition& def) const;
  std::vector<fs::path> get_mod_directories() const;

  std::vector<ModInstance> m_mods;
  std::unordered_map<std::string, RegisteredParam> m_params;
  GameVersion m_game_version = GameVersion::Jak1;
  bool m_initialized = false;
  mutable std::mutex m_mutex;
};

}  // namespace mods
