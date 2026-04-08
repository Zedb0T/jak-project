#include "goalmod_parser.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

namespace mods {

namespace {

std::string trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string to_lower(const std::string& s) {
  std::string result = s;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

/// Try to parse a value from a string. Supports float, int, bool, and quoted strings.
ModValue parse_value(const std::string& raw) {
  std::string s = trim(raw);

  // bool
  if (s == "true" || s == "on" || s == "yes") {
    return true;
  }
  if (s == "false" || s == "off" || s == "no") {
    return false;
  }

  // quoted string
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }

  // try int (no decimal point)
  if (s.find('.') == std::string::npos) {
    try {
      size_t pos = 0;
      int iv = std::stoi(s, &pos);
      if (pos == s.size()) {
        return iv;
      }
    } catch (...) {
    }
  }

  // try float
  try {
    size_t pos = 0;
    float fv = std::stof(s, &pos);
    if (pos == s.size()) {
      return fv;
    }
  } catch (...) {
  }

  // fallback to string
  return s;
}

ModHook parse_hook_name(const std::string& name) {
  std::string lower = to_lower(name);
  if (lower == "on_enable" || lower == "onenable") {
    return ModHook::OnEnable;
  }
  if (lower == "on_disable" || lower == "ondisable") {
    return ModHook::OnDisable;
  }
  if (lower == "on_frame" || lower == "onframe") {
    return ModHook::OnFrame;
  }
  // default to OnEnable
  return ModHook::OnEnable;
}

}  // namespace

std::optional<ModDefinition> parse_goalmod(const std::string& content, std::string& error_out) {
  ModDefinition def;
  def.game = "any";

  // Parsing state
  enum class Section { None, Mod, Hook };
  Section current_section = Section::None;
  ModHookEntry current_hook;
  bool has_hook = false;

  std::istringstream stream(content);
  std::string line;
  int line_num = 0;

  while (std::getline(stream, line)) {
    line_num++;
    std::string trimmed = trim(line);

    // skip empty lines and comments
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
      continue;
    }

    // section headers: @mod, @on_enable, @on_disable, @on_frame
    if (trimmed[0] == '@') {
      // save previous hook section if any
      if (has_hook && !current_hook.actions.empty()) {
        def.hooks.push_back(current_hook);
      }
      has_hook = false;

      std::string section_name = to_lower(trim(trimmed.substr(1)));

      if (section_name == "mod" || section_name == "meta") {
        current_section = Section::Mod;
      } else {
        current_section = Section::Hook;
        current_hook = {};
        current_hook.hook = parse_hook_name(section_name);
        has_hook = true;
      }
      continue;
    }

    // key = value pairs
    auto eq_pos = trimmed.find('=');
    if (eq_pos == std::string::npos) {
      // try key: value syntax
      eq_pos = trimmed.find(':');
      if (eq_pos == std::string::npos) {
        error_out = "line " + std::to_string(line_num) + ": expected 'key = value', got: " + trimmed;
        return std::nullopt;
      }
    }

    std::string key = to_lower(trim(trimmed.substr(0, eq_pos)));
    std::string val_str = trim(trimmed.substr(eq_pos + 1));

    if (current_section == Section::Mod) {
      if (key == "name") {
        def.name = val_str;
        // strip quotes if present
        if (def.name.size() >= 2 && def.name.front() == '"' && def.name.back() == '"') {
          def.name = def.name.substr(1, def.name.size() - 2);
        }
      } else if (key == "author") {
        def.author = val_str;
        if (def.author.size() >= 2 && def.author.front() == '"' && def.author.back() == '"') {
          def.author = def.author.substr(1, def.author.size() - 2);
        }
      } else if (key == "version") {
        def.version = val_str;
        if (def.version.size() >= 2 && def.version.front() == '"' && def.version.back() == '"') {
          def.version = def.version.substr(1, def.version.size() - 2);
        }
      } else if (key == "description" || key == "desc") {
        def.description = val_str;
        if (def.description.size() >= 2 && def.description.front() == '"' &&
            def.description.back() == '"') {
          def.description = def.description.substr(1, def.description.size() - 2);
        }
      } else if (key == "game") {
        def.game = to_lower(val_str);
        if (def.game.size() >= 2 && def.game.front() == '"' && def.game.back() == '"') {
          def.game = def.game.substr(1, def.game.size() - 2);
        }
      }
    } else if (current_section == Section::Hook) {
      // In a hook section, lines are: target = value
      // Optionally: target = value -> revert_value
      ModAction action;
      action.target = key;

      // check for revert syntax: value -> revert_value
      auto arrow_pos = val_str.find("->");
      if (arrow_pos != std::string::npos) {
        std::string main_val = trim(val_str.substr(0, arrow_pos));
        std::string revert_val = trim(val_str.substr(arrow_pos + 2));
        action.value = parse_value(main_val);
        action.revert_value = parse_value(revert_val);
      } else {
        action.value = parse_value(val_str);
        action.revert_value = action.value;  // will be overridden by registry defaults
      }

      current_hook.actions.push_back(action);
    } else {
      error_out = "line " + std::to_string(line_num) + ": data outside of section: " + trimmed;
      return std::nullopt;
    }
  }

  // save last hook section
  if (has_hook && !current_hook.actions.empty()) {
    def.hooks.push_back(current_hook);
  }

  // validation
  if (def.name.empty()) {
    error_out = "mod is missing a 'name' field in the @mod section";
    return std::nullopt;
  }

  return def;
}

std::optional<ModDefinition> parse_goalmod_file(const fs::path& path, std::string& error_out) {
  try {
    std::string content = file_util::read_text_file(path);
    return parse_goalmod(content, error_out);
  } catch (const std::exception& e) {
    error_out = std::string("failed to read file: ") + e.what();
    return std::nullopt;
  }
}

}  // namespace mods
