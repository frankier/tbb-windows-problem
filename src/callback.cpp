// Module that the host unloads before oneTBB shuts down.
//
// fake_tbbmalloc.cpp keeps a pointer to callback_fn() and calls it from its own
// DLL_PROCESS_DETACH. By that time this module is no longer mapped, so the call
// faults. This is a deterministic stand-in for every "a DLL used code from a
// module that was already unloaded" bug that Microsoft warns about in
// https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-entry-point-function

#include <windows.h>

namespace {

void raw_write(const char* text) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        WriteFile(out, text, static_cast<DWORD>(lstrlenA(text)), &written, nullptr);
    }
}

} // namespace

extern "C" __declspec(dllexport) void callback_fn() {
    // These instructions live in a module that has already been unloaded.
    raw_write("[callback] callback_fn entered; this module was unloaded\n");
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        raw_write("[callback] DLL_PROCESS_DETACH\n");
    }
    return TRUE;
}
