# SourcePawn crash attribution

What this fork adds on top of upstream Accelerator, why it had to be done inside
the crashing process, and how to check it on a live server without waiting for a
crash.

## The gap being closed

Accelerator's minidump answers "which C++ module faulted". It cannot answer
"which plugin asked for that code to run", for two structural reasons:

1. Plugin code runs from JIT regions that the SourcePawn VM allocates at
   runtime. Those addresses belong to no loaded module, so breakpad's stack
   walker records them as unresolved numbers and no symbol server can name them.
2. The VM's own call frames are not machine frames. They live in the VM's
   `InvokeFrame` list, linked through `Environment::top()`. A minidump captures
   memory, but nothing in the dump says which plugin function is one step above
   the native that faulted.

Both live only inside the running process, and both are gone the moment the
process dies. So the walk happens in the crash callback, after the minidump has
already been written.

## What a report now contains

Every crash report (`<dump>.dmp.txt`) gains one section. The two blocks below are
the actual bytes printed by the crash handler while a real SourcePawn VM was
faulting, captured by `tests/run_vm_host.sh` (Linux x86, JIT on, the plugin
carrying `.dbg.lines`). The first is the same report taken one moment earlier,
when the VM was loaded but nothing was executing:

```
-------- SOURCEPAWN STACK BEGIN --------
VM-Version=standalone VM host (idle)
VM-Thread=938 Crashed-Thread=938
Frames=0
Status=vm-idle
Note=no plugin was executing, so this crash has no SourcePawn call stack to blame
Legend=native=engine/extension function, script=plugin code with file:line, internal=VM helper frame
-------- SOURCEPAWN STACK END --------

-------- SOURCEPAWN STACK BEGIN --------
VM-Version=standalone VM host
VM-Thread=938 Crashed-Thread=938
Frames=5
[0] native   "AccelProbeCrash"  called-by=accel_probe.smx
[1] script   "InnerTwo"  accel_probe.sp:12  plugin=accel_probe.smx
[2] script   "InnerOne"  accel_probe.sp:17  plugin=accel_probe.smx
[3] script   "AccelProbeMain"  accel_probe.sp:22  plugin=accel_probe.smx
[4] internal (accel_probe.smx)
Running-Plugins=1
plugin=accel_probe.smx frames=3 functions="InnerTwo <- InnerOne <- AccelProbeMain"
Legend=native=engine/extension function, script=plugin code with file:line, internal=VM helper frame
-------- SOURCEPAWN STACK END --------
```

Each `script` line points at the call site *inside* that function: line 12 is the
`AccelProbeCrash()` call in `InnerTwo`, 17 is `InnerTwo()` inside `InnerOne`, 22
is `InnerOne()` inside `AccelProbeMain`. Frame `[0]` names the native that was
executing when the fault landed, which is the half of the picture a minidump
alone cannot give you — it shows the JIT address, not the plugin that jumped to
it.

The file part of `file:line` is whatever path spcomp was given when the plugin
was compiled, not a path on the server, so it is normal for it to point at
somebody's checkout. The probe is deliberately compiled from its own directory
with a bare file name (`cd scripting && spcomp accelerator_stack_probe.sp ...`),
which is why its reports say `accelerator_stack_probe.sp` and not
`H:/code/.../scripting/accelerator_stack_probe.sp`;
`tests/check_probe.sh` fails if that regresses. The extension binary itself
still carries the build tree path in its DWARF info, which only affects
debugging that `.so` by hand and never appears in a report.

`Running-Plugins=` is the same information grouped by plugin, one line per
plugin that had a scripted frame, functions innermost first. It is not a second
walk: the VM keeps a single invoke chain for the process
(`FrameIterator::Reset()` starts at `Environment::get()->top()`) and each frame
carries its own plugin, so one walk already crosses plugin boundaries — a crash
inside plugin B called from plugin A shows both. The summary exists so that
question does not have to be answered by reading `plugin=` off every frame. The
list is capped at 24 plugins; past that it says `(+N more not listed)`.

The empty and awkward cases are written out rather than left blank, so a missing
measurement never reads like a clean one:

| Line | Meaning |
| --- | --- |
| `Frames=0` + `Status=vm-idle` | No plugin was executing. This crash genuinely has no plugin to blame. |
| `Status=no-plugin-contexts` | Accelerator had no loaded plugin context to ask the VM with. |
| `Status=iterator-unavailable` | The VM refused to hand out a frame iterator. |
| `Status=walk-faulted` (Windows) | Reading the stack raised a second exception. The minidump on disk is still valid. |
| `Frames=? (the walk could not count frames)` | The count never settled: appears with `no-plugin-contexts`, `iterator-unavailable`, or a Windows walk fault. Deliberately not the same as `Frames=0`. |
| `note=plugin compiled without debug info` | `.dbg.lines` is absent in that `.smx`, so `:0` is expected, not a lost lookup. |
| `Caveat=...different thread...` | The crash happened off the main thread; the frames are the VM's own stack, not necessarily that thread's work. |
| no `SOURCEPAWN STACK` block at all | See below: on Linux this means the walk faulted or hung, and everything else in the report survived. |

`Status=walk-faulted` can only appear on Windows, and that asymmetry is the
design rather than an oversight: Windows catches a second exception with
`__try/__except` and keeps going, so it can say "the walk faulted". On Linux the
guard hands a second fault to `SIG_DFL` and the process dies at that instant, so
there is no opportunity to write the note. What you get instead is a dump and a
report that stops after `CONSOLE HISTORY END` with no `SOURCEPAWN STACK BEGIN`
line at all (check `CrashPluginStack` first — with it set to `0` the block is
simply not attempted). On Linux that is a readable outcome, because that callback
writes with unbuffered `sys_write()` — the sections above are already on disk. On
Windows they sit in stdio's buffer until `fclose()`, so there a whole missing
block means the handler itself died rather than the walk.

Three different `Truncated` lines mean three different limits, and which one you
got changes what to do about it:

- `Truncated=true (more frames beyond 64)` — the VM stack is deeper than the
  frame cap; the frames shown are the innermost ones, which are usually the
  interesting ones.
- `Truncated=true (frame buffer full)` — 64 frames fit, but their text did not
  fit the 16 KB frame buffer; the last frames were dropped.
- `Truncated=true` immediately before the `STACK END` line — the assembled
  section did not fit the report buffer.

The last one is why the writer holds back room for its own closing lines: without
that reservation the notice would have to be written into a buffer that is by
definition already full, so it could never appear, and a report without a
`STACK END` line would be ambiguous between "the walk died" and "the tail did not
fit". With it, `STACK END` is always the final line of a section that was written
at all, and the smallest buffer that still produces a report is 55 bytes.

Where it goes:

- `addons/sourcemod/data/dumps/<dump>.dmp.txt`, uploaded with the dump as
  `upload_file_metadata`.
- `addons/sourcemod/data/dumps/archive/` keeps a copy after the upload consumes
  the originals, since this section exists nowhere else.

Console history, the section just above, is collected separately, from two
sources:

- **The extension's log listener.** At load it registers a tier0 logging
  listener (`LoggingSystem_RegisterLoggingListener`, resolved from the
  already-loaded `libtier0.so` / `tier0.dll`) and keeps the last 128 KB of
  console output in a static ring. This is the same sink SourceMod's
  `sm_conhook` uses on CS:GO, and it is chronological. The listener is removed
  on unload when the engine exports the matching unregister call; on engines
  that export none, installing is skipped so an unloaded extension cannot leave
  a pointer into unmapped code.
- **The engine's `GetSpew`.** When it returns text it is used only if the
  listener has nothing, because on CS:GO Legacy 1.38.8.1 Linux it always
  returns empty: the `csgo` gamedata section still finds the function with a
  byte signature (engine.so+0x2dcbe0), but the linked list it walks is
  initialised empty and nothing in the binary ever appends to it — the only
  writes to its head and tail indices are the constructor, and the engine's own
  `"Console History (reversed)"` literal no longer has a single reference. It
  still works on Windows/TF2-style builds, where it also covers output from
  before the extension loaded.

Only the last 500 lines are kept — the ring turns over on a busy server, and the
crash is at the end. The section says which source it came from and how much it
cut:

```
-------- CONSOLE HISTORY BEGIN --------
Source=log-listener
Lines=500/812 (tail kept)
...
-------- CONSOLE HISTORY END --------
```

| Line | Meaning |
| --- | --- |
| `Source=log-listener` | Lines came from the extension's own ring; `Lines=kept/held` counts what the ring held, not everything since startup. |
| `Source=engine-getspew` | The listener was empty, so the engine's history was used; `Lines=kept/total` counts the engine buffer. |
| `Status=console-empty` | At least one source was available and produced no lines; the `Note=` says whether it was both of them or names the missing one's reason. |
| `Status=console-unavailable` | Neither source was available; one `Note=GetSpew:` and one `Note=log listener:` line say why. The startup log carries the same warnings. |

## Configuration

Both are read from the `Core` section of `cfg/sourcemod/sourcemod.cfg` and both
default to on:

```
"Core"
{
    "CrashPluginStack"    "1"   // 0 = do not walk the VM stack in the crash handler
    "CrashReportArchive"   "1"   // 0 = do not keep local copies of reports
}
```

## Checking it without crashing

`scripting/accelerator_stack_probe.sp` adds `sm_accel_stack`, which calls the new
`Accelerator_GetPluginStack(buffer, size)` native. It runs the same walk the crash
handler uses, from inside the VM, so its output proves the frame iterator works
against the SourcePawn build actually installed on that server. Called from a
plugin command it must show at least a native frame and the command's own frame;
zero frames there means the capture is not measuring what it claims.

At startup it asks `GetFeatureStatus(FeatureType_Native, "Accelerator_GetPluginStack")`
rather than `LibraryExists(...)`. That is deliberate: the extension registers the
library name `accelerator` (`extension.cpp`, `SDK_OnAllLoaded`), and any build —
including one without this native — can register it. An earlier revision of the
probe tested `LibraryExists("accelerator.ext")`, a name nothing registers, and
refused to load on a server where everything was fine; `tests/check_probe.sh`
now pins both the source and the compiled plugin against that shape.

`sm_accel_crash confirm` then crashes the server on purpose through a native with
two scripted frames below it, which is the shape of the crashes worth attributing.

## What has been measured

The report block quoted above came out of a real SourcePawn VM that really
faulted, on Linux x86 with the JIT enabled, from inside the signal handler. It is
reproducible without a server:

```
bash tests/run_vm_host.sh          # JIT=/path/to/sourcepawn.jit.x86.so
```

That harness dlopens the VM, loads `tests/accel_probe.smx`, calls into a native
that writes to address 1, and prints the section from its own `SIGSEGV` handler
before exiting with code 80. Both halves of the quoted block come from that run:
the idle report before the fault, and the five frames after it. What it proves:

- the frame iterator returns real plugin frames against the VM build in use,
- the line numbers are the call sites in `tests/accel_probe.sp` (12, 17, 22),
- the native frame gets named, and the VM's own helper frame gets tagged
  `internal` instead of being dropped,
- `VM-Thread` and `Crashed-Thread` are the same real thread id (getting
  `-D_LINUX -DPOSIX` wrong here is what first produced `VM-Thread=0`),
- and the walk does not disturb the process on its way out: exit code 80 is the
  harness's own, from `_exit(80)` in the handler.

`bash tests/run_report_shape.sh` checks the section at every buffer size from 1
byte to full: the closing marker is always last, dropped bytes are always
announced, and what survives is always a prefix of the complete report. It also
builds a mutant with the closing-line reservation removed and requires the test
to fail against it (it does: 0 failures on the real code, 804 on the mutant).
Without that second half the test would only be an opinion.

Binaries in `deploy/addons/sourcemod/extensions/`:

| Artifact | Size | Build |
| --- | --- | --- |
| `accelerator.ext.so` (linux-x86) | 11,840,000 | AMBuild in WSL, exit 0, its build log contains no warnings |
| `accelerator.ext.dll` (windows-x86) | 2,659,840 | `build-win.ps1 -Arch x86`, 0 errors |

Each was rebuilt from these sources after the last code change, and the WSL tree
it was built in was byte-compared against this one file by file before and after
(`tests/_compare_trees.sh`) so the shipped binary cannot be a stale build of
different source. The md5 of the file in `deploy/` matches the build output.

A full Windows build emits 14 `C4996`/STL4043 deprecation warnings; every one of
them comes from MSVC's own headers through breakpad's AVRF code, none from these
sources. Both files were re-checked as containing the new native and the new
section text (`grep -a` on the binaries; `Status=walk-faulted` is present only in
the Windows one, for the reason given above).

`tests/_smxstrings.py` inflates a `.smx` container, because a plain `grep` on a
compiled plugin only sees the plaintext header and can therefore "prove" a string
is absent when it is merely compressed. Use it to confirm a plugin's
`.dbg.lines`/`.dbg.files` sections exist before blaming the walk for a `:0`.

## What still needs your server

Not yet demonstrated, because nothing on this machine could boot srcds with
SourceMod loaded:

- The whole delivery chain: breakpad's dump callback → `<dump>.dmp.txt` on disk →
  uploaded as `upload_file_metadata` → copied into `data/dumps/archive/`. The
  section is appended by the same two callbacks that write the config and console
  history, and the archiving path is new code that has never executed.
- The Windows runtime path. `__try/__except`, the VEH `IsWalking()`
  short-circuit, and `Status=walk-faulted` are compile-verified only; the harness
  above ran the Linux guard.
- `note=plugin compiled without debug info`, which spcomp 1.11 cannot produce
  (it always emits `.dbg`), so that branch only matters for `.smx` files built by
  older or third-party compilers.
- Whether the registry tracks a real plugin set through load, unload, and map
  change, and whether the different-thread caveat fires on a server that has
  threads of its own.

Why the local trees could not do it: the `csgo` tree's Metamod is
`2.0.0-dev+7`, which exports plain `CreateInterface`, while SourceMod 1.11's
`sourcemod_mm_i486.so` only exports `CreateInterface_MMS` — the engine reports
`Failed to load plugin addons/sourcemod/bin/sourcemod_mm`. Substituting a
matching Metamod made the server abort with `free(): invalid pointer` right after
`Server is hibernating`. The `raidtest` tree has the same generation problem, its
6730 VM rejects plugins compiled by spcomp 1.11.0.6970, and `srcds.exe` in both
Windows trees stops after `Console initialized` — including with the accelerator
files removed, so that failure is not caused by this work.

The checklist to run it for real, on a test instance rather than production:

1. `bash tests/mkdeploy.sh && bash tests/mkrelease.sh` assembles
   `release/Accelerator-SPStack/`, whose `install-linux.sh` / `install-win.ps1`
   move any same-named file into a timestamped rollback folder, copy the five
   files for that platform, and verify each one against `checksums.txt`. Use
   `--dry-run` first. Install the `.so` or the `.dll`, never both;
   `extensions/accelerator.autoload` is what makes SM load it at map start.
2. The probe is already in `plugins/`, which is the load directory; `scripting/`
   is source. `webternet.ext` is only needed for the upload; dumps and reports are
   written without it.
3. `sm plugins list` should show accelerator loaded, then `sm_accel_stack`. A
   correct answer has at least the native frame and the command's own scripted
   frame. `Status=no-plugin-contexts` here means the extension is not seeing the
   VM at all, and a crash test would then be pointless.
4. `sm_accel_crash confirm`, then read
   `addons/sourcemod/data/dumps/<dump>.dmp.txt` for the block and
   `data/dumps/archive/` for the retained copy.
5. Restart and load a few real plugins, crash again, and check the frames name the
   plugin that was actually running rather than the probe.

## Guards, and what they cost

The walk runs where the process is already broken, so:

- On Linux, `SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT` are set to `SIG_DFL` for the
  duration: a fault inside the walk ends the process instead of re-entering
  breakpad. `alarm(10)` with `SIGALRM` at default disposition covers the worse
  case, a hang: `CreateFrameIterator()` calls `operator new`, and malloc may be
  holding a lock if the crash happened inside the allocator.
- On Windows the walk sits in a `__try/__except`, and
  `BreakpadVectoredHandler()` returns `EXCEPTION_CONTINUE_SEARCH` while
  `spstack::IsWalking()` is set, so a fault during the walk cannot produce a
  second minidump.
- The section is assembled in one piece and appended **last**, after the config
  and console-history sections. Losing the new section never costs the data that
  used to be there. On Linux the earlier sections reach the file immediately
  (`sys_write`); on Windows they sit in stdio's buffer until the file is closed,
  which is why the Windows walk is caught rather than allowed to be fatal.
- The buffer is static storage, not a stack array: on Linux this runs on
  breakpad's alternate signal stack.
- Formatting uses its own fixed-capacity writer (no `printf`, no allocator).

Frame limit is 64; beyond that the report says
`Truncated=true (more frames beyond 64)`.

## Building

- `build-win.ps1 -Arch x86|x86_64` imports the Visual Studio developer shell,
  configures with `--sm-path` (defaults to `..\sourcemod-1.11`) and runs AMBuild.
  It picks a toolset by reading `_MSVC_STL_VERSION` out of each installed
  `VC/Tools/MSVC/*/include/yvals_core.h` and refusing the 145 generation, because
  breakpad still uses `stdext::checked_array_iterator`, which that STL removed.
- `build-linux.sh` is the recipe used here: it rsyncs the edited Windows sources
  into an ext4 tree, runs `configure.py --sm-path=... --enable-optimize
  --targets x86`, then AMBuild. It can also be built with `dockerbuild/`. Three
  things upstream does not mention:
  - `patches/*.patch` were committed with LF and expect an LF breakpad tree. A
    Windows working tree turns them into CRLF; copying such a tree onto Linux
    makes `git apply` reject every hunk. Put both sides back to LF (see the
    build script comment) and the patch step matches CI.
  - `third_party/cwd_cmd.py` joins its arguments before handing them to a shell,
    so building from a path containing a space failed on
    `git apply --reject <path with space>`. It now quotes arguments containing
    whitespace.
  - breakpad's module writer still uses `std::get_temporary_buffer`, deprecated
    in libstdc++ 12; with the project's `-Werror` that stops any modern toolchain
    cold. `AMBuildScript` passes `-Wno-deprecated-declarations`.

## Reproducing the release

In order, and each step says what it proved:

```
bash build-linux.sh                       # .so   (Linux side)
powershell -File build-win.ps1 -Arch x86  # .dll  (Windows side)
bash tests/mkdeploy.sh                    # deploy/ from the two package trees
bash tests/mkrelease.sh                   # release/ + zip, self-checked
```

Then the checks, which run against throwaway trees and never touch a server:

| Command | What it establishes |
| --- | --- |
| `JIT=... bash tests/run_vm_host.sh` | the walk works on a real VM, inside a real `SIGSEGV`; exit 80 |
| `JIT=... bash tests/_check_doc_block.sh` | the report quoted in this file is the measured bytes |
| `bash tests/run_report_shape.sh` | buffer-size invariants hold, and the mutant that breaks them is caught |
| `node tests/check_viewer.mjs` | the shipped viewer parses every shape the C++ writes, and knows when it doesn't |
| `bash tests/check_installers_linux.sh` | dry run writes nothing; install lands five files; the replaced one comes back |
| `powershell -File tests/check_installers_win.ps1` | the same on the Windows half, including refusing a corrupted payload |
| `bash tests/check_release_zip.sh` | installing from the zip gives a byte-identical binary |

`mkrelease.sh` copies this file to `release/Accelerator-SPStack/ENGINEERING-NOTES.md`
and refuses to finish unless `README.md` and those notes quote the same report
lines, so edit this file first, re-run `_check_doc_block.sh` against the harness,
then re-run `mkrelease.sh` rather than touching the release folder by hand.

## Files

| File | Change |
| --- | --- |
| `extension/sp_stack.h`, `extension/sp_stack.cpp` | New: registry, walk, formatter, platform guards. Depends only on the SourcePawn VM API, which is what lets the harness compile it without SourceMod. |
| `extension/extension.cpp` | Calls the walk from both crash callbacks, registers plugins, reads the two config keys, archives reports. |
| `extension/natives.cpp` | `Accelerator_GetPluginStack`. |
| `scripting/include/accelerator.inc` | Declaration of the new native. |
| `scripting/accelerator_stack_probe.sp` | New: verification plugin for a running server. |
| `tests/vm_crash_host.cpp`, `tests/run_vm_host.sh`, `tests/accel_probe.sp` | New: the real-VM crash harness described above. |
| `tests/report_shape.cpp`, `tests/run_report_shape.sh` | New: buffer-size invariants plus the mutant that proves them. |
| `tests/_smxstrings.py` | New: inflates a `.smx` so section presence and plugin strings can be checked honestly. |
| `tests/_check_doc_block.sh` | New: diffs the report quoted in this file against a live harness run. |
| `tests/_compare_trees.sh`, `tests/_buildlogs.sh`, `tests/_probe_so.sh` | New: build-provenance checks (source trees identical, log warnings, strings present in the shipped binary). Paths inside them are the local WSL ones. |
| `tests/mkdeploy.sh` | New: rebuilds `deploy/` from the two package trees. |
| `tests/mkrelease.sh` | New: builds `release/Accelerator-SPStack/` and self-checks it (checksums, measured-text quotes, no leaked build paths). |
| `packaging/README.md`, `packaging/install-linux.sh`, `packaging/install-win.ps1` | New: the operator-facing half of the release. |
| `packaging/viewer.html`, `tests/check_viewer.mjs` | New: offline report viewer (single file, no network) and the parser checks that keep it honest. |
| `tests/check_installers_linux.sh`, `tests/check_installers_win.ps1`, `tests/check_tamper.sh` | New: installers run against throwaway trees; dry run must write nothing, a replaced file must be recoverable, a corrupted payload must be refused. |
| `tests/check_release_zip.sh` | New: installs from the extracted zip and compares the result byte for byte with the build output. |
| `extension/AMBuilder` | Builds `sp_stack.cpp`. |
| `AMBuildScript`, `third_party/cwd_cmd.py`, `build-win.ps1`, `build-linux.sh` | Build fixes and the two platform recipes described above. |
| `deploy/` | Assembled binaries and plugin for one-copy installation; git-ignored, rebuilt from these sources. |
