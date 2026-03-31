// Test: reference jak_bridge state symbols but NOT the heavy functions
#ifdef _WIN32
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { return TRUE; }
#endif

// Minimal forward declarations instead of full jak_bridge.h
namespace jak_bridge {
  bool is_runtime_ready();
  void set_runtime_ready(bool);
}

extern "C" __declspec(dllexport) int test_ping() { return 42; }
extern "C" __declspec(dllexport) bool get_ready() {
    return jak_bridge::is_runtime_ready();
}
