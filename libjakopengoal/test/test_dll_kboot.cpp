// Test B: only reference MasterExit from kboot
#ifdef _WIN32
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { return TRUE; }
#endif

#include "game/kernel/common/kboot.h"

extern "C" __declspec(dllexport) int test_ping() { return 42; }
extern "C" __declspec(dllexport) int get_master_exit() {
    return (int)MasterExit;
}
