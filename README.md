# oneTBB: FreeLibrary() from DllMain — Windows crash reproducer

A minimal, self-contained reproducer for the Windows defect in oneTBB that makes
`tbb12.dll` call `FreeLibrary()` on its dependencies from `DLL_PROCESS_DETACH`,
while the Windows loader lock is held. On the affected revision the reproducer
terminates with `0xC0000005` (`STATUS_ACCESS_VIOLATION`, exit code
`-1073741819`).

Default target: upstream `uxlfoundation/oneTBB` master at
`f1e7e5cbf0b77a94d27700141b9188580bd9c778` (2023.2.0, before the deferred-unload
change).

Related: PR #2221 / issue #2220 in `uxlfoundation/oneTBB`.

## Quick start (Windows)

Requirements: Visual Studio 2022 (or Build Tools) with the C++ x64 toolset,
CMake >= 3.20, Git, and network access for the first configure (it clones oneTBB).

```bat
scripts\build.bat
scripts\run.bat
```

`build.bat` configures into `build\`, builds oneTBB and the reproducer, and
stages a self-contained directory in `build\dist` (static CRT, no VC++
redistributable needed). `run.bat` runs each scenario as a child process and
reports its exit code.

Use an **x64 Native Tools Command Prompt** so that `cl.exe` is on `PATH`.

Expected result on `f1e7e5c`:

```text
close              exited normally
close --exit       exited normally
finalize           access violation 0xC0000005
finalize --exit    exited normally
```

The `finalize` run prints the following immediately before it dies:

```text
[fake-tbbmalloc] DLL_PROCESS_DETACH reserved=0000000000000000 armed=1 ...
[fake-tbbmalloc] oneTBB is unloading me from a detach handler; notifying the host
[host] oneTBB unloaded the allocator from a detach handler; the loader lock is released, triggering the use-after-unload
[host] trigger thread: calling into the unloaded module
```

`reserved=0000000000000000` is the key line: it means the DLL was unloaded by an
explicit `FreeLibrary()` call, not by process termination. The call came from
oneTBB's `DLL_PROCESS_DETACH`.

## What actually happens

oneTBB's Windows shutdown path on `f1e7e5c`:

```text
FreeLibrary("tbb12.dll")
└─ tbb12.dll!DllMain(DLL_PROCESS_DETACH)                  src/tbb/main.cpp:137
   └─ __TBB_InitOnce::remove_ref()                       src/tbb/main.cpp:143/148
      └─ governor::release_resources()                   src/tbb/governor.cpp:104
         └─ dynamic_unlink_all()                         src/tbb/governor.cpp:122
            └─ FreeLibrary("tbbmalloc.dll")              src/tbb/dynamic_link.cpp:518-520
```

Microsoft's [DLL entry-point guidance](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-entry-point-function)
and [DllMain reference](https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain)
state that the entry point must not call `FreeLibrary()` (or otherwise take the
loader lock), including from CRT-invoked static destructors. The documented
consequence is exactly what the reproducer demonstrates: a DLL is used after
its termination code has run.

`release_resources()` is reached only when oneTBB's internal reference count
hits zero. That is why the crash needs a `finalize()` before the unload:

* without `finalize()`, the scheduler still holds a reference, so `tbb12.dll`
  is not unloaded and `DllMain` never runs (that is why ordinary applications
  and the existing CI tests do not catch this);
* with `finalize()`, the scheduler reference is dropped, the explicit
  `FreeLibrary("tbb12.dll")` is the last reference, and `DllMain` runs while
  the loader lock is held.

## How the reproducer works

```text
host.exe            does not link oneTBB; loads tbb12.dll and plugin.dll by hand
plugin.dll          links oneTBB; exports attach_scheduler / run_work / finalize_scheduler
callback.dll        a module the host unloads before oneTBB shuts down
tbbmalloc.dll       stand-in allocator (exports the four scalable_* functions)
tbb12.dll           oneTBB itself, built from the pinned commit
```

Sequence for the `finalize` scenario:

1. `host.exe` loads `callback.dll`, `tbb12.dll` and `plugin.dll`, attaches a
   scheduler handle and runs `parallel_for`.
2. oneTBB resolves `tbbmalloc.dll` next to `tbb12.dll` (the stand-in) and keeps
   the only loader reference to it. The host never loads it, so it cannot
   account for the unload.
3. The host records `callback_fn`'s address, unloads `callback.dll`, and
   reserves the freed image range with `PAGE_NOACCESS` so the later call cannot
   land in reused memory.
4. The host calls `finalize()`, unloads `plugin.dll`, then unloads `tbb12.dll`.
5. `tbb12.dll!DllMain` reaches reference count zero and calls
   `FreeLibrary("tbbmalloc.dll")`.
6. The stand-in's detach handler sees `reserved == nullptr` and signals the
   host's trigger thread. The handler itself performs no unsafe call: the
   Windows loader unwinds (and thereby hides) access violations raised inside a
   `DllMain` invoked by `FreeLibrary()`, so the trigger runs on a separate
   thread, outside that exception frame.
7. The trigger thread calls `callback_fn`, whose module is gone. The access
   violation is unhandled, so the process terminates with `0xC0000005`.

## What is real, what is a stand-in

Real:

* the oneTBB revision, its reference counting, and the `FreeLibrary()` call from
  `DLL_PROCESS_DETACH` while the loader lock is held;
* the fact that `tbbmalloc.dll` is resolved and unloaded without the
  application's involvement;
* the access violation itself.

Stand-in plumbing, needed to make the defect observable and deterministic:

* `tbbmalloc.dll` is a fake. It serves oneTBB's allocation requests from the
  CRT and logs its own unload. It is not a malloc replacement.
* `callback.dll` and the trigger thread only exist to turn "oneTBB unloaded a
  module from `DllMain`" into a hard, reproducible fault. Without them, a stock
  `tbbmalloc.dll` often survives this path, which is why the bug went unnoticed.

In other words: the crash is deliberate, but it can only happen because oneTBB
performed a `FreeLibrary()` from its entry point at a time when a DLL's detach
code can no longer safely run.

## Results

Measured on Microsoft Validation OS (Windows 10.0.26100.9549) in a VM, built
with MSVC 19.44, Release, static CRT, `TCM_BUILD=OFF`:

| oneTBB commit | `close` | `close --exit` | `finalize` | `finalize --exit` |
|---|---|---|---|---|
| `f1e7e5c` (upstream master) | 0 | 0 | **0xC0000005** | 0 |
| `5ec58d6` (deferred unload) | 0 | 0 | **0xC0000005** | 0 |
| `30337af` (guard: no `FreeLibrary` from `DllMain`) | 0 | 0 | 0 | 0 |

The `30337af` row is the fix: `governor::release_resources()` skips
`dynamic_unlink_all()` when it is called from `DllMain`, and the process exits
cleanly.

`close` does not crash even on the affected revisions because `finalize()` is
what drops the last internal reference; without it `tbb12.dll` is not actually
unloaded. `--exit` skips the explicit unload, so the process shuts down normally.

### Diagnostic: `--control`

`--control` disarms the stand-in's detach notification. The process then does
not run the trigger thread, but the log still shows oneTBB unloading
`tbbmalloc.dll` from a detach handler. Use it to see the defect without the
deliberate crash:

```bat
build\dist\host.exe finalize --control
```

On the tested build this variant also died with `0xC0000005` later during
process teardown (the unload is unsafe regardless of the stand-in), but that
teardown crash is timing-dependent and is not what `run.bat` relies on.

## Pinning a different revision

```bat
cmake -S . -B build -A x64 -DONETBB_GIT_TAG=<commit>
```

PR commits are not in the upstream repository. Use the fork that contains them:

```bat
cmake -S . -B build -A x64 ^
  -DONETBB_GIT_REPOSITORY=https://github.com/frankier/oneTBB.git ^
  -DONETBB_GIT_TAG=5ec58d697b319b428c3a2cde7020d9f5150492b6
```

## Verifying on Linux with MSVC-Wine

The results above were produced this way.

```bash
export MSVC_BIN=/path/to/msvc-wine/bin/x64
export WINEPREFIX=/path/to/wineprefix
scripts/build-wine.sh
scripts/run-wine.sh
```

Notes:

* POSIX truncates process exit statuses to 8 bits, so Wine reports the
  `0xC0000005` crash as status 5. `run-wine.sh` classifies the crash from the
  log output instead.
* Wine may open its own crash dialog. `run-wine.sh` disables it with
  `WINEDLLOVERRIDES=winedbg.exe=d`; you can also set
  `HKCU\Software\Wine\WineDbg\ShowCrashDialog` to `0` in the prefix.

## Build configuration notes

* `TBB_VERIFY_DEPENDENCY_SIGNATURE=OFF` (oneTBB's default) lets the unsigned
  stand-in `tbbmalloc.dll` load. Turning it on would make oneTBB verify
  Authenticode signatures.
* `TBBMALLOC_BUILD=OFF`: the stand-in replaces the real allocator; the
  `tbbmalloc.dll` in `build\dist` is the fake.
* `TCM_BUILD=OFF`: TCM needs hwloc and is irrelevant here.
* `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`: static CRT, so `build\dist` runs
  on any Windows machine without the VC++ redistributable.
* The crash is a user-mode access violation in a short-lived child process. It
  does not damage the machine or other processes.
