// Host that drives oneTBB's Windows shutdown path.
//
// This executable does not link oneTBB. It loads tbb12.dll by hand so that it
// controls when the last loader reference disappears, then loads a plugin that
// links oneTBB and runs parallel work.
//
// Usage:
//   host.exe close     [--control] [--exit]
//   host.exe finalize  [--control] [--exit]
//
//   close      run work, unload the plugin, unload oneTBB
//   finalize   run work, finalize the scheduler, unload the plugin, unload oneTBB
//   --exit     do not unload oneTBB explicitly; let process termination do it
//   --control  never arm the fake allocator's detach notification

#include <windows.h>

#ifndef ONETBB_DLL_NAME
#error "ONETBB_DLL_NAME must be defined by the build system"
#endif

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using int_fn = int (*)();
using void_fn = void (*)();
using arm_detach_notification_fn = void (*)(void*, int);
using allocation_count_fn = long (*)();

// State shared with the trigger thread. It is only written before the thread
// starts, except for the two events.
HANDLE g_detach_event = nullptr;
HANDLE g_release_event = nullptr;
void* g_callback_function = nullptr;

void logf(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stdout, format, arguments);
    va_end(arguments);
    std::fflush(stdout);
}

std::string executable_directory() {
    char path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string result(path, length);
    const std::string::size_type slash = result.find_last_of("\\/");
    if (slash != std::string::npos) {
        result.resize(slash + 1);
    }
    return result;
}

HMODULE load_library(const std::string& directory, const char* name) {
    const std::string path = directory + name;
    SetLastError(ERROR_SUCCESS);
    HMODULE module = LoadLibraryA(path.c_str());
    logf("[host] LoadLibraryA(\"%s\") = %p (GetLastError=%lu)\n", path.c_str(),
         static_cast<void*>(module), static_cast<unsigned long>(GetLastError()));
    return module;
}

void* find_symbol(HMODULE module, const char* name) {
    FARPROC address = module != nullptr ? GetProcAddress(module, name) : nullptr;
    logf("[host] GetProcAddress(%s) = %p\n", name, reinterpret_cast<void*>(address));
    return reinterpret_cast<void*>(address);
}

// Address range occupied by a loaded module, from its PE headers.
struct module_range {
    void* base;
    std::size_t size;
};

module_range image_range(HMODULE module) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    if (module == nullptr) {
        return {nullptr, 0};
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return {nullptr, 0};
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return {nullptr, 0};
    }
    return {base, static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage)};
}

// Runs on its own thread, so it is not covered by the loader's DllMain
// exception frame. It waits until oneTBB has unloaded the fake allocator and
// the host has left the loader lock, then calls the function whose module was
// unloaded earlier.
DWORD WINAPI trigger_thread(LPVOID) {
    WaitForSingleObject(g_detach_event, INFINITE);
    WaitForSingleObject(g_release_event, INFINITE);
    logf("[host] trigger thread: calling into the unloaded module\n");
    reinterpret_cast<void_fn>(g_callback_function)();
    logf("[host] trigger thread: call returned; the module was still mapped\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const char* mode = "finalize";
    bool unload_one_tbb = true;
    bool arm_callback = true;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "close") == 0 || std::strcmp(argv[i], "finalize") == 0) {
            mode = argv[i];
        } else if (std::strcmp(argv[i], "--exit") == 0) {
            unload_one_tbb = false;
        } else if (std::strcmp(argv[i], "--control") == 0) {
            arm_callback = false;
        } else {
            std::fprintf(stderr, "usage: %s close|finalize [--control] [--exit]\n", argv[0]);
            return 64;
        }
    }

    // This program is meant to crash unattended: do not show a Windows Error
    // Reporting dialog or a debugger prompt.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    const bool finalize_mode = std::strcmp(mode, "finalize") == 0;
    logf("[host] scenario=%s%s%s\n", mode, unload_one_tbb ? " unload" : " exit",
         arm_callback ? "" : " control");

    const std::string directory = executable_directory();

    HMODULE callback_module = load_library(directory, "callback.dll");
    g_callback_function = find_symbol(callback_module, "callback_fn");
    if (callback_module == nullptr || g_callback_function == nullptr) {
        logf("[host] error: callback.dll is missing\n");
        return 2;
    }

    HMODULE one_tbb_module = load_library(directory, ONETBB_DLL_NAME);
    if (one_tbb_module == nullptr) {
        logf("[host] error: %s is missing\n", ONETBB_DLL_NAME);
        return 3;
    }

    HMODULE plugin_module = load_library(directory, "plugin.dll");
    auto attach_scheduler = reinterpret_cast<void_fn>(find_symbol(plugin_module, "attach_scheduler"));
    auto run_work = reinterpret_cast<int_fn>(find_symbol(plugin_module, "run_work"));
    auto finalize_scheduler =
        reinterpret_cast<int_fn>(find_symbol(plugin_module, "finalize_scheduler"));
    if (plugin_module == nullptr || attach_scheduler == nullptr || run_work == nullptr ||
        finalize_scheduler == nullptr) {
        logf("[host] error: plugin.dll is missing or incomplete\n");
        return 4;
    }

    if (finalize_mode) {
        attach_scheduler();
    }
    run_work();

    // oneTBB loaded tbbmalloc.dll from the directory of tbb12.dll while it ran
    // the work above. The host never loads that DLL itself, so the only loader
    // reference is oneTBB's.
    HMODULE allocator_module = GetModuleHandleA("tbbmalloc.dll");
    if (allocator_module == nullptr) {
        allocator_module = GetModuleHandleA("tbbmalloc_debug.dll");
    }
    if (allocator_module == nullptr) {
        logf("[host] error: oneTBB did not load tbbmalloc.dll from this directory\n");
        return 5;
    }
    logf("[host] allocator module = %p\n", static_cast<void*>(allocator_module));

    auto arm_detach_notification = reinterpret_cast<arm_detach_notification_fn>(
        find_symbol(allocator_module, "arm_detach_notification"));
    auto allocation_count =
        reinterpret_cast<allocation_count_fn>(find_symbol(allocator_module, "get_allocation_count"));
    if (arm_detach_notification == nullptr || allocation_count == nullptr) {
        logf("[host] error: this is not the fake tbbmalloc.dll\n");
        return 6;
    }

    const long allocations = allocation_count();
    logf("[host] fake allocator served %ld allocation(s)\n", allocations);
    if (allocations == 0) {
        logf("[host] error: oneTBB never allocated through the fake allocator\n");
        return 7;
    }

    g_detach_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_release_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (g_detach_event == nullptr || g_release_event == nullptr) {
        logf("[host] error: CreateEvent failed\n");
        return 8;
    }
    arm_detach_notification(g_detach_event, arm_callback ? 1 : 0);

    HANDLE trigger = CreateThread(nullptr, 0, trigger_thread, nullptr, 0, nullptr);
    if (trigger == nullptr) {
        logf("[host] error: CreateThread failed\n");
        return 9;
    }

    // From here on g_callback_function points into an unloaded module. Reserve
    // the freed range as inaccessible so that the call cannot land in reused
    // memory: this makes the access violation deterministic.
    const module_range callback_range = image_range(callback_module);
    logf("[host] FreeLibrary(\"callback.dll\") = %d\n", static_cast<int>(FreeLibrary(callback_module)));
    logf("[host] callback.dll after FreeLibrary = %p\n",
         static_cast<void*>(GetModuleHandleA("callback.dll")));
    if (callback_range.base != nullptr) {
        SetLastError(ERROR_SUCCESS);
        void* reserved = VirtualAlloc(callback_range.base, callback_range.size,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
        logf("[host] VirtualAlloc(%p, %zu, PAGE_NOACCESS) = %p (GetLastError=%lu)\n",
             callback_range.base, callback_range.size, reserved,
             static_cast<unsigned long>(GetLastError()));
    }

    if (finalize_mode && finalize_scheduler() != 0) {
        logf("[host] error: finalize() failed\n");
        return 10;
    }

    logf("[host] FreeLibrary(\"plugin.dll\") = %d\n", static_cast<int>(FreeLibrary(plugin_module)));
    if (unload_one_tbb) {
        logf("[host] FreeLibrary(\"%s\") = %d\n", ONETBB_DLL_NAME,
             static_cast<int>(FreeLibrary(one_tbb_module)));
        logf("[host] %s after FreeLibrary = %p\n", ONETBB_DLL_NAME,
             static_cast<void*>(GetModuleHandleA(ONETBB_DLL_NAME)));
    }

    // Was the fake allocator unloaded by somebody else (that is, by oneTBB)?
    const bool allocator_loaded =
        GetModuleHandleA("tbbmalloc.dll") != nullptr || GetModuleHandleA("tbbmalloc_debug.dll") != nullptr;
    logf("[host] tbbmalloc.dll still loaded = %d\n", allocator_loaded ? 1 : 0);

    if (allocator_loaded) {
        logf("[host] oneTBB did not unload the allocator in this scenario\n");
    } else if (!arm_callback) {
        // The detach notification is disarmed, so the trigger thread stays
        // blocked and the process does not crash.
        logf("[host] oneTBB unloaded the allocator from a detach handler "
             "(notification disarmed, not triggering)\n");
    } else {
        logf("[host] oneTBB unloaded the allocator from a detach handler; "
             "the loader lock is released, triggering the use-after-unload\n");
        SetEvent(g_release_event);
        const DWORD wait = WaitForSingleObject(trigger, 5000);
        logf("[host] error: trigger thread did not crash (wait=%lu)\n",
             static_cast<unsigned long>(wait));
        return 11;
    }

    logf("[host] scenario finished, returning from main\n");
    return 0;
}
