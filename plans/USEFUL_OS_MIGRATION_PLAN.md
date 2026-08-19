# CamelOS: Hobby OS → Useful OS — Migration Plan

> **Status**: plan for review — no code written yet.
> **Goal**: make CamelOS a *real* operating system whose applications are
> self-contained user-mode processes that talk to the kernel through a stable
> syscall ABI and dynamically link shared libraries — and whose web browser
> actually lays out and renders pages instead of printing text lines.

---

## 0. Honest baseline (what the code actually does today)

### 0.1 Applications are not processes — they are in-kernel shared objects
- `kernel_api_t g_kernel_api` (`sys/cdl_defs.h`, `core/cdl_loader.c`) is a ~50-slot
  **struct of function pointers**. Apps receive this struct as a parameter to
  their `cdl_main(api)` entry point and call through it (`sys->print(...)`).
- `internal_load_library()` (`core/cdl_loader.c`) reads an ELF shared object,
  `kmalloc`s a buffer in **kernel heap**, manually relocates it, and calls its
  entry point directly — **in ring 0**, in the kernel address space.
- The relocation loop only handles `R_386_32`, `R_386_RELATIVE`, and a no-op
  `R_386_PC32`. There is **no GOT/PLT binding and no undefined-symbol
  resolution** — so there is no real dynamic linking against shared libraries.
- Most "apps" are not even CDL modules: `USR_SRC` in the `Makefile` compiles
  `usr/apps/*.c` straight into `system.elf`, and `kernel_launch_builtin_app()`
  (`core/kernel.c`) "launches" them by calling `init_files_app()`,
  `init_terminal_app()`, etc. A crash in any app is a kernel crash.

### 0.2 The syscall layer exists but is decorative
- `hal/cpu/syscall.c` implements `int 0x80` (`syscall_handler`) and
  `sysenter/sysexit` (`syscall_init_fast`), but **every handler just forwards to
  the same `g_kernel_api` vtable**. So the "syscalls" are the vtable with extra
  steps, and almost nothing uses them — apps call `sys->...` directly.
- Ring 3 machinery is present but dormant: `process_exec_user()`
  (`core/process.h`), `task_create_user()` (`core/task.c`), `SYS_USER_*` cases,
  user/supervisor page bits in `core/vmm.c` — but the built-in/CDL apps never
  run through it.

### 0.3 The camelplist (Info.plist) is decorative metadata
- `AppBundleInfo` (`core/app_bundle.h`) holds name/id/executable/type/icon/version.
- `app_bundle_parse_plist()` (`core/app_bundle.c`) parses XML + key=value.
- But the loader (`resolve_and_load()` in `core/cdl_loader.c`) uses the plist only
  to decide **which fallback path to take**; for `CFBundleType=builtin` it
  discards `CFBundleExecutable` and calls the **hardcoded**
  `kernel_launch_builtin_app(name)` string→function table. The plist never drives
  symbol registration, dependency loading, or service discovery.
- The same plist is hand-written in ~10 places (installer, `app_bootstrap.c`,
  `sys/api.c`, `desktop.c`, `package_manager.c`, `dmg_mount.c`,
  `app_installer.c`) with slightly different keys — a single source of truth
  is missing.

### 0.4 The browser renders text lines, not layout
- `usr/apps/browser.c` renders each element as a "line" with a type
  (`LINE_H1`…`LINE_H6`, `LINE_LI`, …) and heading sizes — no box model, no
  flex/grid, no positioning. `usr/libs/browser_dom.c` builds a DOM and
  `css_parser_v2.c` parses CSS, but the parsed styles are not fed into a real
  layout engine. `<img>` is a placeholder; form controls are plain text; video
  has no codec/audio path. This is why google.com loads but looks broken.

### 0.5 Why this matters
The single most important structural problem is **0.1**: "nothing really calls
nothing." Everything is compiled together and linked by direct function calls,
so there is no boundary where an OS normally enforces isolation, stability, and
ABI. Every other goal (safety, crash isolation, installable apps, dynamic
libraries) hangs off fixing that.

---

## 1. Target architecture

```
User space (Ring 3):
  App process  |  App process  |  App process
  (.app + camelplist)
       └────── libsystem.so (user libc + syscall wrappers)
                    │  shared libs: /usr/lib/*.so, Frameworks/
                    │  dynamic linker resolves symbols at load
Kernel space (Ring 0):
  syscall table | process mgr | VMM | scheduler | IPC/service registry
  VFS | net/TCP | gfx/window server | drivers
```

Concretely:

1. **Monolithic kernel, honest about it.** No more "microkernel" claim. One
   kernel binary, but a *clean* boundary: user code reaches the kernel only
   through numbered syscalls.
2. **Ring 3 processes.** Every app is a process with its own address space
   (VMM/COW already exist). An app crash terminates the process, not the kernel.
3. **ELF-i386 as the native format.** The toolchain is gcc/ld and the CDL apps
   are already ELF shared objects. Mach-O becomes a *read-only compatibility*
   layer, not the native format. (Decision to confirm — see §7.)
4. **Real dynamic linking.** A proper dynamic linker resolving
   `R_386_GLOB_DAT`, `R_386_JMP_SLOT`, symbol tables, and loading `/usr/lib/*.so`
   and embedded `Frameworks/`.
5. **camelplist is the source of truth**, compiled into the system at
   install/launch (§3). No more hardcoded `kernel_launch_builtin_app`.
6. **Service registry** (Mach-bootstrap-like) on top of the existing `core/ipc.c`
   ports: apps publish named services/symbols; other apps look them up.
7. **A user-space `libsystem`** that wraps the syscall ABI — this *replaces*
   `g_kernel_api` and the `sys->` vtable over time.

---

## 2. Core decisions (locked)

| # | Decision | Recommendation | Rationale |
|---|----------|----------------|-----------|
| D1 | Native executable format | **ELF-i386**, Mach-O as compat | Toolchain is ELF; `cdl` apps are ELF `.so` |
| D2 | Syscall interface | **Numbered syscalls**, register args, one table | Kills the "don't reorder the struct" ABI fragility |
| D3 | Dynamic linker | Extend the ELF loader into a real `ld.so`-like linker | `dyld.c`/`macho_loader.c` stay for Mach-O compat |
| D4 | App manifest | Extend camelplist; keep `CFBundle*` names | macOS fidelity + backward compat |
| D5 | Migration | **Incremental with a compat shim** (not big-bang) | The OS keeps booting while apps are ported |
| D6 | JS engine | **Pick one** (recommend muJS, already vendored) | Currently 4+ engines (muJS, elk, js_engine, js_engine_v2) |
| D7 | TLS | **Replace hand-rolled crypto** with mbedTLS/BearSSL | Self-rolled TLS is unsafe |

---

## 3. The camelplist → "compiled into the system" design (your specific ask)

Goal: the plist becomes the *authoritative declaration* of an app, and the
system "compiles" it — i.e. parses it, loads the binary, resolves its
dependencies and exports, registers it in the app registry + service registry,
and creates the process. The registry "header" and callable functions are then
*derived from the plist*, not from a hardcoded table.

### 3.1 Canonical camelplist schema (extend `AppBundleInfo`)

```xml
<plist>
  <dict>
    <key>CFBundleName</key>         <string>Files</string>
    <key>CFBundleIdentifier</key>   <string>com.camelos.files</string>
    <key>CFBundleExecutable</key>   <string>Files</string>
    <key>CFBundleType</key>         <string>elf</string>
    <key>CFBundleEntry</key>        <string>main</string>
    <key>CFBundleVersion</key>      <string>1.0</string>
    <key>CFBundleRequires</key>     <array><string>libcamel.so</string></array>
    <key>CFBundleExports</key>      <array><string>com.camelos.files.open</string></array>
    <key>CFBundleIconFile</key>     <string>Files.png</string>
    <key>CamelPermissions</key>     <array><string>net</string><string>disk</string></array>
  </dict>
</plist>
```

New/repurposed keys: `CFBundleEntry` (entry symbol), `CFBundleRequires`
(dependencies), `CFBundleExports` (published service/symbol names),
`CamelPermissions` (capabilities). `CFBundleType=builtin` becomes a temporary
shim during migration, then disappears.

### 3.2 New syscalls

```c
int  sys_app_register(const char* bundle_path);
int  sys_app_launch(int app_id, const char* args);
void* sys_app_get_symbol(int app_id, const char* name);
int  sys_app_unregister(int app_id);
```

`sys_app_register` is the "compile the plist" syscall. It performs, in order:

1. Parse `Info.plist` into a full `AppManifest`.
2. Resolve `CFBundleExecutable` → binary path.
3. Load the ELF image + `CFBundleRequires` libraries (dynamic linker).
4. Resolve `CFBundleEntry`, capture `CFBundleExports` symbol addresses.
5. Insert a record into the **app registry** (replacing the hardcoded table)
   *and* register `CFBundleExports` names in the **service registry** so
   `sys_app_get_symbol` / IPC lookups can find them.
6. Return `app_id`. `sys_app_launch(app_id)` then spawns the Ring 3 process.

This makes the registry "header" a *function of the plist*, and makes the app's
declared functions callable by others — the two things described.

### 3.3 Single writer for plists
Consolidate the ~10 plist writers into one `app_manifest_write()` helper so the
schema can't drift again.

---

## 4. Phased implementation plan

> Each phase ends in a bootable, testable system.

### Phase 0 — Stabilize & make the codebase honest (~2–3 days)
- Tree hygiene: `make distclean`; delete the stray `Makefile` directory, the
  ~26 orphaned `usr/apps/js*.c` muJS copies, and unused JS engines (keep one).
- Fix `CDL_CFLAGS` to drop `-I/usr/include`; add `-Wextra`, then `-Werror`.
- Add a **host-side test harness** (`tests/`, `make test`) for pure code:
  `core/string.c`, JSON/CSS/plist parsers, `pfs32` metadata math.
- Reconcile README/roadmap with reality.

### Phase 1 — One stable syscall ABI + a real user-mode hello (the Ring 3 spine)
- Freeze a **single numbered syscall table** in one header (`sys/syscalls.h`),
  register-argument convention; retire `g_kernel_api` as the *primary* path
  (keep only as a compat shim — §5).
- Finish `copy_from_user`/`copy_to_user` and validate *every* pointer at the
  boundary (partial today in `hal/cpu/syscall.c`).
- Make `process_exec_user()` actually load and run an ELF-i386 executable in
  Ring 3 with `mmap`/`brk`/argv/env, and wire `sysenter/sysexit` correctly
  (fix the TSS/ESP0 issue in `core/scheduler.c`).
- **Proof point:** port the Calculator app to a real Ring 3 process that opens
  a window via syscalls.

### Phase 2 — Self-contained apps: real dynamic linking + process lifecycle
- Upgrade the ELF loader into a proper dynamic linker: dynamic symbol table,
  `R_386_GLOB_DAT`/`R_386_JMP_SLOT`, PLT/GOT, library search paths.
- Build the existing `usr/lib/*.c` as shared libraries (`*.so`) instead of
  one-shot `.cdl` apps, and link apps against them.
- Promote `fork/exec/wait/exit/mmap/brk/signal` to the syscall ABI with real
  argv/env, exit codes, and user-space signal frames.
- **Proof point:** one CDL app (e.g. TextEdit) runs as a process linked against
  `libcamel.so`, launched/killed without touching the kernel.

### Phase 3 — camelplist becomes the runtime truth
- Implement `AppManifest` + `CFBundleEntry/Requires/Exports/Permissions`.
- Implement `sys_app_register/launch/get_symbol/unregister` (§3.2).
- Replace `kernel_launch_builtin_app()` with plist-driven registration; migrate
  built-in apps **one at a time** to processes (keep `CFBundleType=builtin` shim).
- Wire `CFBundleExports` into the service registry so apps call each other's
  published services (IPC).
- **Proof point:** the Dock/Finder launches an installed third-party `.app` that
  was *not* compiled into the kernel, purely from its plist.

### Phase 4 — Browser/Web engine rework (the "layout is terrible" problem)
Start after Phase 2 so the browser is also a real process.

- **Layout engine** (`usr/libs/layout.c`, new): box model, block/inline flow,
  flex and grid from `css_parser_v2.c`, margins/padding/borders, positioning.
  Replace the `LINE_*` renderer in `usr/apps/browser.c`.
- **Styled rendering**: feed parsed CSS into layout to produce a render tree of
  styled boxes.
- **Images**: `<img>` fetches URL and decodes via `core/png_decoder.c` +
  `include/jpeg_decoder.h`, then blits into the layout box.
- **Form controls**: render real buttons, inputs, checkboxes (reuse
  `usr/lib/ui_widgets.c`).
- **Network correctness**: proper HTTP/1.1 (redirects, chunked, content-length),
  HTTPS via a vetted TLS library (D7).
- **JavaScript**: consolidate to one engine (muJS recommended); wire DOM/CSSOM
  bindings through that engine.
- **Explicitly defer**: video/audio playback (needs codec + audio server + HDA),
  JIT, WebGL/WebAssembly.

### Phase 5 — Hardening & "useful OS" polish
- Capability/permission enforcement using `CamelPermissions`.
- ASLR via PIE executables; optional code signing for `.app` bundles.
- Package manager (`caml`) integration with the new app registry.
- Driver backlog (NVMe, VirtIO, HDA audio, GPT/extended partitions) — parallel track.

---

## 5. Migration strategy: a compat shim, not a rewrite

- Provide a **`libsystem`** user library whose API mirrors `kernel_api_t`
  (`print`, `malloc`, `fs_read`, `socket`, `draw_rect`, …) but is implemented
  with `syscallN(...)`. Existing `sys->print(...)` code ports by changing
  `sys->print(...)` → `sys_print(...)` mechanically.
- Keep `g_kernel_api` and `CFBundleType=builtin` as a **temporary shim** for
  unported apps; remove after the last built-in app is a real process.
- Add a **process watchdog**: a faulting ported app logs a clean crash report
  (`core/crash.c`) and is restarted by `launchd` instead of panicking.

---

## 6. Definition of done & measurable milestones

1. **Phase 1 done** = Calculator runs as a Ring 3 process; a deliberate
   null-pointer write prints a crash report and kills *only* that process.
2. **Phase 2 done** = a `.so` shared by ≥2 processes; dependency listing works;
   `ps` shows real processes.
3. **Phase 3 done** = installing a `.app` registers its plist, its
   `CFBundleExports` are callable via the service registry, the Dock launches it,
   and there are zero references to it in the kernel binary.
4. **Phase 4 done** = google.com (or a controlled test page) lays out with real
   boxes, images render, buttons/inputs look and act like controls.
5. **Phase 5 done** = an app without `net` permission cannot open a socket; an
   app crash never panics the kernel.

---

## 7. Decisions locked (from review)

1. **Native format (D1): ELF-i386** native; Mach-O kept as read-only compat only.
2. **Sequencing (D5): process model first** (Phases 1–3), browser after (Phase 4).
3. **App packaging (D4): keep the macOS `.app` / `Info.plist` (camelplist) aesthetic.**
4. **JS engine (D6): muJS.** Keep the vendored muJS; remove `usr/libs/js_engine.c`,
   `usr/libs/js_engine_v2.c`, `usr/libs/jscore.c`, `usr/apps/elk.c`, and the
   ~26 orphaned `usr/apps/js*.c` / `regexp.c` / `pp.c` muJS copies.
5. **TLS (D7): attempt mbedTLS/BearSSL.** If it does not work in practice, fall
   back to the existing hand-rolled TLS stack.

**Next action**: begin Phase 0 (tree hygiene + host-side test harness), then
Phase 1 (syscall ABI + ring-3 Calculator proof).
