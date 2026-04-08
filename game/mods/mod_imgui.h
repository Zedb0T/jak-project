#pragma once

/*!
 * @file mod_imgui.h
 * ImGui window for browsing, enabling, and disabling mods at runtime.
 */

namespace mods {

class ModImGui {
 public:
  /// Draw the mod manager window. Only call when m_open is true.
  void draw();

  bool m_open = false;

 private:
  int m_selected_mod = -1;
  bool m_show_params = false;
};

}  // namespace mods
