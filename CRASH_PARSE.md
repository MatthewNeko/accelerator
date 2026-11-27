# Crash-report parsing that cannot take the server with it

What this fork changes about the upload thread's local minidump parsing, why the
previous behaviour was a boot-time crash loop, and how the change can be checked
without waiting for a crash.

## The failure being fixed

The upload thread runs at extension load and reads every `*.dmp` in
`addons/sourcemod/data/dumps` before uploading it. Reading means running
breakpad's `MinidumpProcessor` inside `srcds` itself.

That processor is a parser: it trusts the file it is given and dereferences what
the file says. A dump it cannot survive does not fail with an error, it faults
the calling thread. On the server this showed up as:

```
开服 → 上传线程扫描 data/dumps → 解析残留 .dmp
     → MinidumpProcessor 在拼线程字符串时空指针崩 → SIGSEGV
     → 进程 1.2 秒死亡 → 自动重启 → 又解析同一份文件 → 再崩 ……
```

The loop was closed by ordering: the dump is only unlinked *after* parsing, so a
dump that kills the parser is never deleted and is parsed again on every start.
Each round also leaves one more dump behind, so the folder grows while the server
never gets past map load.

The observed crash (Accelerator 2.6.0-manual, SourceMod 1.12.0.7230, Linux x86):

| | |
|---|---|
| Signature | `SIGSEGV`, fault address `0x20` |
| Thread | `SM CompatThread` → `UploadThread::RunThread` → `PresubmitCrashDump` → `MinidumpProcessor::Process` |
| Failing statement | `minidump_processor.cc:264`, `thread_string += " id " + HexString(thread_id)` |
| Uptime at death | 1.23 s, `MapLoad Count 1` |

## What changed

Four independent layers, each of which alone would have stopped that loop.

### 1. A dump is moved before it is read

`UploadThread::RunThread` moves `data/dumps/<name>.dmp` into
`data/dumps/processing/` before anything looks inside it, and only deletes it
after the upload. If the read takes the process down, the dump is not where the
next start looks; it is in `processing/`.

On the next start the upload thread sweeps `processing/` and moves what it finds
into `data/dumps/bad/`, logging that a previous run died reading it. Those dumps
are kept for the operator and are not retried, so the second boot of an
offending dump is also its last.

### 2. The container is checked before the parser sees it

`crashparse::Validate` reads the minidump header and stream directory and
refuses anything that cannot be a minidump: wrong or absent `MDMP` signature, a
file smaller than `MINIDUMP_HEADER`, no streams, a stream directory outside the
file, any stream whose RVA and size run past EOF, no thread list or module list,
and a thread count no server could have.

A dump that fails these checks goes to `bad/` with the reason in the log instead
of into breakpad. This is the cheap filter; it is not a substitute for the next
point, because a structurally valid dump can still be one breakpad mishandles.

### 3. On Linux the parse happens in a child process

`crashparse::Parse` forks. The child runs breakpad, serialises the result (crash
signature plus module table) and writes it through a pipe; the parent reads a
bounded blob and reaps the child. A fault in the parser now costs one child
process, and the parent reports it:

```
the minidump parser crashed on signal 11 while reading this dump
```

The child resets `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE`/`SIGABRT` to their defaults
first, so a fault there cannot make breakpad's own handler write yet another
dump into the folder being worked through. There is a 30 second deadline: a
wedged child is killed and the dump is treated as unreadable rather than
blocking the upload thread forever.

Windows has no `fork`, so there the parse stays in the caller. That is safe
because of layer 1: the dump being parsed is no longer in the folder the next
start reads, so a fault cannot repeat. As a belt-and-braces measure, if the dump
could not be moved aside *and* the build parses in-process, the dump is uploaded
without a local signature rather than parsed.

### 4. The upload thread starts after the crash handler is installed

`threader->MakeThread(&uploadThread)` used to run before `new
ExceptionHandler(...)`. The consequence in the observed crash: the thread died
under the engine's own `bin/crashhandler.so`, producing a `crash_<time>_<n>.dmp`
with Valve comment streams instead of an Accelerator report with map, command
line and SourcePawn frames. Starting the thread after the handler is installed
means a fault in it is now an Accelerator report.

### Also fixed: `std::clog` was pointed at a null buffer

The old `ClogInhibitor` did `std::clog.rdbuf(nullptr)`. Inserting into a stream
whose buffer is null is undefined behaviour, and breakpad logs its progress
through `BPLOG_INFO` on `std::clog` several times per dump. It is now pointed at
a real sink buffer that discards input. This is the kind of latent fault that
would look exactly like the crash being fixed, one stack frame away.

## What did not change

The crash signature sent to the crash server is byte-for-byte what it was. The
code that builds it moved from `PresubmitCrashDump` into `crashparse::BuildSummary`
without changing the field order, the `2|` format version, the module records or
the frame records, which was confirmed by building both versions against the same
breakpad and comparing their output on a real dump (see below). Uploaded reports,
presubmit tokens, symbol and binary uploads are untouched.

The only behavioural difference on the wire: a dump whose parse dies is now sent
as metadata-only (`kPRLocalError`) and set aside, where before it killed the
server.

## Files

| File | Change |
|---|---|
| `extension/crash_parse.h`, `extension/crash_parse.cpp` | New: `Validate`, `Parse` (forked on Linux), `BuildSummary`, the result serialisation, `ParseRunsInOwnProcess`. Depends only on breakpad and the C library, not on SourceMod, so it can be compiled and tested on its own. |
| `extension/extension.cpp` | Move-then-read in `RunThread`, `processing/` and `bad/` folders, the recovery sweep, `Validate` before parse, parse via `crashparse`, upload thread moved after handler install, real `std::clog` sink, module helpers take `crashparse::ModuleInfo` instead of `google_breakpad::CodeModule`. |
| `extension/AMBuilder` | Builds `crash_parse.cpp`. |

## Directories the upload thread now uses

```
addons/sourcemod/data/dumps/            dumps waiting to be read (unchanged)
addons/sourcemod/data/dumps/processing/ the one being read right now
addons/sourcemod/data/dumps/bad/        dumps that could not be read, kept
addons/sourcemod/data/dumps/archive/    crash reports, as before
```

`processing/` is empty between runs on a healthy server. Anything in it at start
up was being read when the previous run died, and is moved to `bad/`.

## Checking it

`Validate` was exercised against a real Windows minidump and against dumps
corrupted on purpose (truncated, wrong signature, directory RVA past EOF); the
good one is accepted and each bad one is rejected with its reason. `Parse` on the
same real dump produces a signature identical to the one the unmodified
`PresubmitCrashDump` produced, checked byte for byte.

On a server, after deploying:

1. Start with a dump known to be unreadable sitting in `data/dumps`. Expect
   `Accelerator set aside a crash dump it cannot read (...)` in
   `logs/accelerator.log`, the file under `bad/`, and the server passing map
   load.
2. `data/dumps/processing/` should be empty once the upload thread reports
   `Accelerator upload thread finished. (n skipped, n uploaded, n failed)`.
3. Kill the server while it is reading a dump to confirm the next start moves it
   from `processing/` to `bad/` instead of reading it again.