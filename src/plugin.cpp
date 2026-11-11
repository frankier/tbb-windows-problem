// Plugin that links oneTBB. The host loads it by hand, so tbb12.dll becomes a
// load-time dependency of this DLL instead of a dependency of the host.

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/cache_aligned_allocator.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_for.h>

#include <atomic>
#include <cstdio>
#include <new>

namespace {

oneapi::tbb::task_scheduler_handle g_scheduler_handle;

} // namespace

// Attach a scheduler handle. oneTBB adds a reference to its global state.
extern "C" __declspec(dllexport) void attach_scheduler() {
    g_scheduler_handle = oneapi::tbb::task_scheduler_handle{oneapi::tbb::attach{}};
    std::printf("[plugin] task_scheduler_handle attached\n");
    std::fflush(stdout);
}

// Allocate through oneTBB's allocator and run parallel work. The allocation
// makes oneTBB resolve tbbmalloc.dll deterministically.
extern "C" __declspec(dllexport) int run_work() {
    oneapi::tbb::cache_aligned_allocator<char> allocator;
    char* buffer = allocator.allocate(128);
    allocator.deallocate(buffer, 128);

    std::atomic<int> sum{0};
    oneapi::tbb::parallel_for(oneapi::tbb::blocked_range<int>(0, 100000),
                              [&](const oneapi::tbb::blocked_range<int>& range) {
                                  int local = 0;
                                  for (int i = range.begin(); i != range.end(); ++i) {
                                      local += i % 7;
                                  }
                                  sum.fetch_add(local, std::memory_order_relaxed);
                              });
    std::printf("[plugin] parallel_for finished, sum=%d\n", sum.load());
    std::fflush(stdout);
    return sum.load();
}

// Finalize the scheduler. This drops the reference that DoOneTimeInitialization
// took, so the last oneTBB reference is left to the host's unload.
extern "C" __declspec(dllexport) int finalize_scheduler() {
    const bool ok = oneapi::tbb::finalize(g_scheduler_handle, std::nothrow);
    std::printf("[plugin] finalize() returned %s\n", ok ? "true" : "false");
    std::fflush(stdout);
    return ok ? 0 : 1;
}
