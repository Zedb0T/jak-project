// Test A: only reference exec_runtime
#ifdef _WIN32
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { return TRUE; }
#endif

#include "game/runtime.h"

extern "C" __declspec(dllexport) int test_ping() { return 42; }
extern "C" __declspec(dllexport) void* get_exec_runtime_ptr() {
    return (void*)&exec_runtime;
}
