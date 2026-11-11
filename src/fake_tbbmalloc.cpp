// Stand-in for oneTBB's tbbmalloc.dll.
//
// oneTBB resolves "tbbmalloc.dll" relative to tbb12.dll and keeps one loader
// reference to it. This DLL exports the four functions oneTBB needs, records how
// often they are used (so the host can prove that oneTBB really went through
// this allocator), and reports when oneTBB unloads it with FreeLibrary().
//
// The detach handler only signals an event: the offending use-after-unload is
// performed by a host thread, outside the loader-lock exception frame that the
// loader installs around DllMain. Without that indirection the loader swallows
// the access violation and the process can survive long enough to produce a
// misleading exit code.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <malloc.h>

namespace {

volatile LONG g_allocations = 0;
volatile LONG g_armed = 0;
HANDLE g_detach_event = nullptr;

void raw_write(const char* text) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        WriteFile(out, text, static_cast<DWORD>(lstrlenA(text)), &written, nullptr);
    }
}

} // namespace

extern "C" __declspec(dllexport) void* scalable_malloc(std::size_t size) {
    InterlockedIncrement(&g_allocations);
    return std::malloc(size);
}

extern "C" __declspec(dllexport) void scalable_free(void* pointer) {
    std::free(pointer);
}

extern "C" __declspec(dllexport) void* scalable_aligned_malloc(std::size_t size, std::size_t alignment) {
    InterlockedIncrement(&g_allocations);
    return _aligned_malloc(size, alignment);
}

extern "C" __declspec(dllexport) void scalable_aligned_free(void* pointer) {
    _aligned_free(pointer);
}

// Host-facing controls. These are deliberately not part of oneTBB's ABI.
extern "C" __declspec(dllexport) long get_allocation_count() {
    return g_allocations;
}

extern "C" __declspec(dllexport) void arm_detach_notification(void* event_handle, int arm) {
    g_detach_event = reinterpret_cast<HANDLE>(event_handle);
    g_armed = arm != 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason != DLL_PROCESS_DETACH) {
        return TRUE;
    }

    char message[256];
    std::snprintf(message, sizeof(message),
                  "[fake-tbbmalloc] DLL_PROCESS_DETACH reserved=%p armed=%ld allocations=%ld\n",
                  reserved, static_cast<long>(g_armed), static_cast<long>(g_allocations));
    raw_write(message);

    // reserved == nullptr means somebody called FreeLibrary() on this DLL.
    // reserved != nullptr means the process or thread is going away.
    if (reserved == nullptr && g_armed != 0 && g_detach_event != nullptr) {
        raw_write("[fake-tbbmalloc] oneTBB is unloading me from a detach handler; notifying the host\n");
        SetEvent(g_detach_event);
    }
    return TRUE;
}
