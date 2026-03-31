// Minimal DLL for isolating which static library causes DllMain failure in Blender.
// Links against specific libraries to narrow down the culprit.

#ifdef _WIN32
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { return TRUE; }
#endif

extern "C" __declspec(dllexport) int test_ping() { return 42; }

// Pull in symbols from linked libs to force them to actually link
#ifdef TEST_FMT
#include "fmt/core.h"
extern "C" __declspec(dllexport) void use_fmt() { auto s = fmt::format("hi {}", 1); }
#endif

#ifdef TEST_COMMON
#include "common/log/log.h"
extern "C" __declspec(dllexport) void use_common() { /* just link it */ }
#endif

#ifdef TEST_SDL3
#include "SDL3/SDL.h"
extern "C" __declspec(dllexport) void use_sdl() { /* static constructors from SDL3 */ }
#endif

#ifdef TEST_IMGUI
#include "imgui.h"
extern "C" __declspec(dllexport) void use_imgui() { ImGui::GetVersion(); }
#endif
