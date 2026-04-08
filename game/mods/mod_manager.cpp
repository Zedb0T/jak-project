#include "mod_manager.h"

#include <regex>

#include "game/mods/goalmod_parser.h"

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

namespace mods {

ModManager& ModManager::get() {
  static ModManager instance;
  return instance;
}

void ModManager::init(GameVersion version) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_game_version = version;
  m_initialized = true;
  lg::info("[ModManager] Initializing for {}", game_version_names[version]);
  discover_mods();
  lg::info("[ModManager] Found {} mod(s)", m_mods.size());
}

void ModManager::shutdown() {
  std::lock_guard<std::mutex> lock(m_mutex);
  // disable all active mods to revert parameters
  for (size_t i = 0; i < m_mods.size(); i++) {
    if (m_mods[i].enabled) {
      m_mods[i].enabled = false;
      for (auto& hook : m_mods[i].definition.hooks) {
        if (hook.hook == ModHook::OnDisable) {
          apply_actions(hook.actions);
        }
      }
      // revert on_enable actions
      for (auto& hook : m_mods[i].definition.hooks) {
        if (hook.hook == ModHook::OnEnable) {
          for (auto& action : hook.actions) {
            revert_param(action.target);
          }
        }
      }
    }
  }
  m_mods.clear();
  m_initialized = false;
  lg::info("[ModManager] Shut down");
}

void ModManager::tick() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_initialized) {
    return;
  }

  for (auto& mod : m_mods) {
    if (!mod.enabled || mod.failed) {
      continue;
    }
    for (auto& hook : mod.definition.hooks) {
      if (hook.hook == ModHook::OnFrame) {
        apply_actions(hook.actions);
      }
    }
  }
}

void ModManager::rescan() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_initialized) {
    return;
  }

  // remember which mods were enabled by name
  std::unordered_map<std::string, bool> was_enabled;
  for (auto& mod : m_mods) {
    was_enabled[mod.definition.name] = mod.enabled;
    // revert active mods
    if (mod.enabled) {
      for (auto& hook : mod.definition.hooks) {
        if (hook.hook == ModHook::OnEnable) {
          for (auto& action : hook.actions) {
            revert_param(action.target);
          }
        }
      }
    }
  }

  m_mods.clear();
  discover_mods();

  // re-enable previously enabled mods
  for (size_t i = 0; i < m_mods.size(); i++) {
    auto it = was_enabled.find(m_mods[i].definition.name);
    if (it != was_enabled.end() && it->second) {
      m_mods[i].enabled = true;
      for (auto& hook : m_mods[i].definition.hooks) {
        if (hook.hook == ModHook::OnEnable) {
          apply_actions(hook.actions);
        }
      }
    }
  }

  lg::info("[ModManager] Rescan complete, {} mod(s) loaded", m_mods.size());
}

// --- Parameter Registry ---

void ModManager::register_param(const std::string& name,
                                ModValue default_value,
                                std::function<void(const ModValue&)> on_change) {
  std::lock_guard<std::mutex> lock(m_mutex);
  RegisteredParam param;
  param.name = name;
  param.default_value = default_value;
  param.current_value = default_value;
  param.on_change = std::move(on_change);
  m_params[name] = std::move(param);
}

const ModValue* ModManager::get_param(const std::string& name) const {
  auto it = m_params.find(name);
  if (it == m_params.end()) {
    return nullptr;
  }
  return &it->second.current_value;
}

std::vector<std::string> ModManager::get_param_names() const {
  std::vector<std::string> names;
  names.reserve(m_params.size());
  for (auto& [name, _] : m_params) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

// --- Mod Control ---

bool ModManager::enable_mod(size_t index) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (index >= m_mods.size()) {
    return false;
  }

  auto& mod = m_mods[index];
  if (mod.failed) {
    return false;
  }

  mod.enabled = true;

  // run on_enable hooks
  for (auto& hook : mod.definition.hooks) {
    if (hook.hook == ModHook::OnEnable) {
      apply_actions(hook.actions);
    }
  }

  lg::info("[ModManager] Enabled mod: {}", mod.definition.name);
  return true;
}

void ModManager::disable_mod(size_t index) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (index >= m_mods.size()) {
    return;
  }

  auto& mod = m_mods[index];
  mod.enabled = false;

  // run on_disable hooks
  for (auto& hook : mod.definition.hooks) {
    if (hook.hook == ModHook::OnDisable) {
      apply_actions(hook.actions);
    }
  }

  // revert on_enable and on_frame parameters to defaults
  for (auto& hook : mod.definition.hooks) {
    if (hook.hook == ModHook::OnEnable || hook.hook == ModHook::OnFrame) {
      for (auto& action : hook.actions) {
        revert_param(action.target);
      }
    }
  }

  lg::info("[ModManager] Disabled mod: {}", mod.definition.name);
}

void ModManager::toggle_mod(size_t index) {
  // read enabled state before calling enable/disable (which acquire the lock)
  bool currently_enabled;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= m_mods.size()) {
      return;
    }
    currently_enabled = m_mods[index].enabled;
  }
  if (currently_enabled) {
    disable_mod(index);
  } else {
    enable_mod(index);
  }
}

// --- Internal ---

void ModManager::discover_mods() {
  auto dirs = get_mod_directories();
  std::regex goalmod_pattern(".*\\.goalmod$");

  for (auto& dir : dirs) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
      continue;
    }

    auto files = file_util::find_files_in_dir(dir, goalmod_pattern);
    for (auto& file : files) {
      std::string error;
      auto def = parse_goalmod_file(file, error);

      ModInstance instance;
      instance.file_path = file;

      if (!def.has_value()) {
        instance.failed = true;
        instance.error_message = error;
        instance.definition.name = file.filename().string();
        lg::warn("[ModManager] Failed to parse {}: {}", file.string(), error);
      } else {
        instance.definition = def.value();
        if (!is_mod_compatible(instance.definition)) {
          instance.failed = true;
          instance.error_message = "incompatible with current game version";
          lg::info("[ModManager] Skipping incompatible mod: {} (requires {})",
                   instance.definition.name, instance.definition.game);
        }
      }

      m_mods.push_back(std::move(instance));
    }
  }
}

void ModManager::apply_actions(const std::vector<ModAction>& actions) {
  for (auto& action : actions) {
    auto it = m_params.find(action.target);
    if (it == m_params.end()) {
      lg::warn("[ModManager] Unknown parameter: {}", action.target);
      continue;
    }

    it->second.current_value = action.value;
    if (it->second.on_change) {
      it->second.on_change(action.value);
    }
  }
}

void ModManager::revert_param(const std::string& name) {
  auto it = m_params.find(name);
  if (it == m_params.end()) {
    return;
  }

  // check if any other enabled mod also targets this parameter
  // if so, don't revert - let the other mod's value stand
  for (auto& mod : m_mods) {
    if (!mod.enabled || mod.failed) {
      continue;
    }
    for (auto& hook : mod.definition.hooks) {
      for (auto& action : hook.actions) {
        if (action.target == name) {
          // another active mod uses this param, apply its value instead
          it->second.current_value = action.value;
          if (it->second.on_change) {
            it->second.on_change(action.value);
          }
          return;
        }
      }
    }
  }

  // no other mod uses it, revert to default
  it->second.current_value = it->second.default_value;
  if (it->second.on_change) {
    it->second.on_change(it->second.default_value);
  }
}

bool ModManager::is_mod_compatible(const ModDefinition& def) const {
  if (def.game == "any" || def.game.empty()) {
    return true;
  }
  std::string current_game = game_version_names[m_game_version];
  return def.game == current_game;
}

std::vector<fs::path> ModManager::get_mod_directories() const {
  std::vector<fs::path> dirs;

  // 1. Project-local mods directory
  auto project_dir = file_util::get_jak_project_dir() / "mods";
  dirs.push_back(project_dir);

  // 2. User config mods directory (per-game)
  auto config_dir = file_util::get_user_config_dir() / "OpenGOAL" /
                    game_version_names[m_game_version] / "mods";
  dirs.push_back(config_dir);

  // 3. User config mods directory (shared)
  auto shared_dir = file_util::get_user_config_dir() / "OpenGOAL" / "mods";
  dirs.push_back(shared_dir);

  return dirs;
}

}  // namespace mods
