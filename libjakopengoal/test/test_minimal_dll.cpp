// Minimal DLL to test if the problem is static constructors from linked libs
// Build: cl /LD test_minimal_dll.cpp /Fe:test_minimal.dll
// Then try loading in Blender

#include <windows.h>
#include <cstdio>

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        OutputDebugStringA("[test_minimal] DllMain OK\n");
    }
    return TRUE;
}

extern "C" __declspec(dllexport) int test_func() {
    return 42;
}
