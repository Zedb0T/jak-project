/**
 * runtime_launcher.cpp - Separate translation unit that calls exec_runtime().
 *
 * This is deliberately in its own file so that jak_bridge.obj does NOT
 * reference exec_runtime directly. The linker only pulls in runtime_launcher.obj
 * (and consequently runtime.obj) when someone calls launch_runtime_headless().
 *
 * When built with RUNTIME_HEADLESS defined, runtime.obj won't include graphics
 * headers (game/graphics/gfx.h, game/external/discord.h), so its static
 * constructors won't conflict with Blender's process environment.
 */

#include "jak_bridge.h"

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "game/runtime.h"

namespace jak_bridge {

int launch_runtime_headless(const std::string& game_data_path) {
  // Set up the game data path so the runtime can find iso_data/ and out/
  if (!game_data_path.empty()) {
    fs::path proj_path(game_data_path);
    file_util::setup_project_path(proj_path);
  }

  // Configure headless launch options
  GameLaunchOptions options;
  options.game_version = GameVersion::Jak1;
  options.disable_display = true;  // No window - headless mode

  // Build argv for the GOAL runtime
  const char* argv[] = {"libjakopengoal", "-boot", "-fakeiso", "-debug", "-lib-jak"};
  int argc = 5;

  lg::info("[libjakopengoal] Launching exec_runtime (headless)...");
  RuntimeExitStatus status = exec_runtime(options, argc, argv);
  return static_cast<int>(status);
}

}  // namespace jak_bridge
