#include "mod_imgui.h"

#include "game/mods/mod_manager.h"

#include "third-party/imgui/imgui.h"

namespace mods {

namespace {
const char* value_to_string(const ModValue& val, char* buf, size_t buf_size) {
  if (auto* f = std::get_if<float>(&val)) {
    snprintf(buf, buf_size, "%.3f", *f);
  } else if (auto* i = std::get_if<int>(&val)) {
    snprintf(buf, buf_size, "%d", *i);
  } else if (auto* b = std::get_if<bool>(&val)) {
    snprintf(buf, buf_size, "%s", *b ? "true" : "false");
  } else if (auto* s = std::get_if<std::string>(&val)) {
    snprintf(buf, buf_size, "%s", s->c_str());
  } else {
    snprintf(buf, buf_size, "?");
  }
  return buf;
}

const char* hook_name(ModHook hook) {
  switch (hook) {
    case ModHook::OnEnable:
      return "on_enable";
    case ModHook::OnDisable:
      return "on_disable";
    case ModHook::OnFrame:
      return "on_frame";
    default:
      return "unknown";
  }
}
}  // namespace

void ModImGui::draw() {
  if (!m_open) {
    return;
  }

  auto& mgr = ModManager::get();
  if (!mgr.is_initialized()) {
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(520, 400), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Mod Manager", &m_open)) {
    ImGui::End();
    return;
  }

  // toolbar
  if (ImGui::Button("Rescan Mods")) {
    mgr.rescan();
  }
  ImGui::SameLine();
  ImGui::Checkbox("Show Parameters", &m_show_params);
  ImGui::SameLine();
  ImGui::TextDisabled("(%zu mod(s) loaded)", mgr.get_mods().size());

  ImGui::Separator();

  // two-panel layout: mod list on left, details on right
  auto& mods = mgr.get_mods_mut();

  // left panel: mod list with toggle checkboxes
  ImGui::BeginChild("ModList", ImVec2(220, 0), true);
  for (size_t i = 0; i < mods.size(); i++) {
    auto& mod = mods[i];

    ImGui::PushID(static_cast<int>(i));

    if (mod.failed) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
    }

    // checkbox to toggle
    bool enabled = mod.enabled;
    if (ImGui::Checkbox("##toggle", &enabled)) {
      if (enabled) {
        mgr.enable_mod(i);
      } else {
        mgr.disable_mod(i);
      }
    }

    if (mod.failed) {
      ImGui::PopStyleColor();
    }

    ImGui::SameLine();

    // selectable name
    bool selected = (m_selected_mod == static_cast<int>(i));
    std::string label = mod.definition.name;
    if (mod.failed) {
      label += " [ERROR]";
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
    } else if (mod.enabled) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
    }

    if (ImGui::Selectable(label.c_str(), selected)) {
      m_selected_mod = static_cast<int>(i);
    }

    if (mod.failed || mod.enabled) {
      ImGui::PopStyleColor();
    }

    ImGui::PopID();
  }

  if (mods.empty()) {
    ImGui::TextDisabled("No .goalmod files found.");
    ImGui::TextDisabled("Place them in the 'mods/' folder.");
  }

  ImGui::EndChild();

  ImGui::SameLine();

  // right panel: mod details
  ImGui::BeginChild("ModDetails", ImVec2(0, 0), true);

  if (m_selected_mod >= 0 && m_selected_mod < static_cast<int>(mods.size())) {
    auto& mod = mods[m_selected_mod];
    auto& def = mod.definition;

    // header
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", def.name.c_str());
    ImGui::Separator();

    if (!def.version.empty()) {
      ImGui::Text("Version: %s", def.version.c_str());
    }
    if (!def.author.empty()) {
      ImGui::Text("Author: %s", def.author.c_str());
    }
    if (!def.game.empty()) {
      ImGui::Text("Game: %s", def.game.c_str());
    }

    ImGui::Spacing();

    if (!def.description.empty()) {
      ImGui::TextWrapped("%s", def.description.c_str());
    }

    ImGui::Spacing();

    // status
    if (mod.failed) {
      ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error: %s",
                          mod.error_message.c_str());
    } else {
      ImGui::Text("Status: %s", mod.enabled ? "Enabled" : "Disabled");
    }

    ImGui::Spacing();

    // file path
    ImGui::TextDisabled("File: %s", mod.file_path.filename().string().c_str());

    ImGui::Spacing();
    ImGui::Separator();

    // hooks detail
    if (ImGui::TreeNode("Hook Details")) {
      for (auto& hook : def.hooks) {
        if (ImGui::TreeNode(hook_name(hook.hook))) {
          char buf[64];
          for (auto& action : hook.actions) {
            ImGui::BulletText("%s = %s", action.target.c_str(),
                              value_to_string(action.value, buf, sizeof(buf)));
          }
          ImGui::TreePop();
        }
      }
      ImGui::TreePop();
    }

    ImGui::Spacing();

    // enable/disable buttons
    if (!mod.failed) {
      if (mod.enabled) {
        if (ImGui::Button("Disable")) {
          mgr.disable_mod(m_selected_mod);
        }
      } else {
        if (ImGui::Button("Enable")) {
          mgr.enable_mod(m_selected_mod);
        }
      }
    }
  } else {
    ImGui::TextDisabled("Select a mod to view details.");
  }

  ImGui::EndChild();

  // parameter viewer panel (collapsible, below the main area)
  if (m_show_params) {
    ImGui::Separator();
    if (ImGui::TreeNode("Registered Parameters")) {
      auto names = mgr.get_param_names();
      char buf[64];
      for (auto& name : names) {
        auto* val = mgr.get_param(name);
        if (val) {
          ImGui::Text("  %s = %s", name.c_str(), value_to_string(*val, buf, sizeof(buf)));
        }
      }
      ImGui::TreePop();
    }
  }

  ImGui::End();
}

}  // namespace mods
