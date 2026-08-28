# Ring 3 Migration — Phase 0 + Phase 1

> **Branch**: `ring3-phase1` — complements `REVIEW_AUG2026.md` (§5 P0 items and
> the 8-week plan). Goal: make the README's architecture diagram true — apps in
> Ring 3, kernel as syscall provider.

## What this branch does

### Phase 0 — Isolation foundations (security fixes)

| Fix | File | What changed |
|---|---|---|
| Kernel pages were **user-accessible** | `hal/cpu/paging.c` | Identity map (0–128MB) flags `0x7` (P\|RW\|**U**) → `0x3` (P\|RW). Previously ANY Ring 3 process could read/write all kernel memory through the shared page tables (`vmm_create_address_space` shares entries 0–31 by design — that design is now actually safe). |
| CR0.WP never set | `hal/cpu/paging.c` | `switch_page_directory()` now sets `CR0.PG\|CR0.WP` — Ring 0 honors read-only PTEs, which COW fork requires. |
| PDE flags didn't follow PTE flags | `hal/cpu/paging.c` | `paging_map_region()` derives the directory entry's RW/U bits from the mapping flags (Ring 3 needs U at BOTH levels). |
| `paging_map_region()` clobbered user CR3 | `hal/cpu/paging.c` | TLB flush now reloads the *current* CR3 instead of force-switching to `kernel_directory`. |
| **Single static sysenter kernel stack** | `hal/cpu/syscall.c`, `core/scheduler.c` | New `syscall_set_enter_stack()` rewrites `IA32_SYSENTER_ESP` on EVERY context switch to the incoming task's own kernel stack. (The MSR — not the `tss_esp0_for_sysenter` shadow variable — is what the CPU loads; the old code only wrote the variable, and even that was `next->esp`, a saved frame, instead of `kernel_stack_top`.) |
| Ring 3 tasks never released the CPU on exit | `core/scheduler.c` | `TASK_STATE_ZOMBIE` now triggers reschedule immediately. |
| Unvalidated user pointers in syscalls | `hal/cpu/syscall.c` | `SYS_PRINT`, `SYS_DRAW_TEXT`, `SYS_GET_ARGS`, `SYS_USER_WIN_CREATE`, `SYS_stat`, `SYS_gettimeofday`, `SYS_WAITPID` now validate/copy through `user_copy.h` (`validate_user_ptr` / `validate_user_string` / `copy_to_user` / bounded `KCOPY_USER_STR`). Notably `SYS_USER_WIN_CREATE` wrote `*x_out`/`*y_out` **directly through Ring 3 pointers** — a forged pointer could write arbitrary kernel memory. |
| `cld` missing on all kernel entry paths | `boot/system_entry.asm` | Added to `isr_common_stub`, `irq_common_stub`, `syscall_entry`, `sysenter_entry` — a Ring 3 process can set DF; kernel `rep movsb` would otherwise run backwards. |

### Phase 1 — First real Ring 3 process

`usr/test/user_calc.c` (the `user_calc.bin` blob, loaded via `user_exec_raw()`)
now runs as a true CPL 3 process:

- Own address space (`vmm_create_address_space`, code at `0x08000000`,
  stack at `0x7FFFF000`, per-task 16KB kernel stack)
- Talks to the kernel **only** via `int 0x80` (`SYS_PRINT`, `SYS_USER_WIN_CREATE`,
  `SYS_DRAW_RECT`, `SYS_DRAW_TEXT`, `SYS_GET_TICKS`, `SYS_USER_EXIT`)
- Draws a Calculator window, holds it ~2.5 s, exits cleanly
  (`process_exit` → ZOMBIE → reschedule → desktop continues)

Two launch paths:

1. **Shell command**: type `ring3` (also in `help`).
2. **Boot smoke test**: `make RING3_BOOT_TEST=1` auto-launches it during boot.

## Verified by automated QEMU run

`make RING3_BOOT_TEST=1 system.bin`, MBR disk image, 90 s headless boot:

```
[RING3] Boot test: launching ring3-calc at CPL3...
[VMM] Created address space 0x2262ae8, dir 0x235c000 (phys 0x235c000)
[SCHED] Added task to scheduler
[USER] exec: launched 'ring3-calc' at 0x8000000
...
[calc] ring3 process entered CPL3, creating window     <- user code at CPL 3
[WS] Create: Calculator                                 <- syscall -> window server
[calc] ring3 window drawn                               <- draw syscalls
[calc] ring3 process exiting cleanly
[PROC] exit: pid, status 1 0                            <- clean ZOMBIE exit
[SPOTLIGHT] Initialized                                 <- desktop kept running
```

Counts: page faults / panics: **0**, unknown syscalls: **0**.
Default build (`make system.bin`, no flag): boots clean, zero RING3 activity.

## Known limitations (next phases)

1. **One Ring 3 process at a time**; no reaper yet — the exited task's
   kernel stack + address space are reclaimed only conceptually. Phase 2:
   reap + `waitpid` unblock path.
2. The desktop shares the **idle task slot**, so while a Ring 3 task is READY
   the GUI is not scheduled (the calc's 2.5 s hold freezes the GUI by design).
   Phase 2: move the desktop into a scheduled kernel task, or better, define
   the **window-server IPC protocol** so apps never draw direct framebuffer
   syscalls from a frozen compositor.
3. `paint_cb`-less windows are erased by the next damage-repair pass — content
   persistence needs per-window backing stores (Phase 2, review §5 P1).
4. Syscall coverage is hardened but not complete: `SYS_FS_*` paths, sockets,
   `SYS_NOTIFY_*`, signal syscalls still take raw user pointers. Phase 2:
   extend `KCOPY_USER_STR`/`validate_user_ptr` to those handlers.
5. sysenter fast path exists and is now stack-safe, but its user-space
   register convention (EBX = return address) still collides with the
   int-0x80 ABI (EBX = arg1) — unused by this test; unify before enabling.

## Files changed

- `hal/cpu/paging.c` — supervisor-only identity map, CR0.WP, PDE flags, safe TLB flush
- `hal/cpu/syscall.c` / `hal/cpu/syscall.h` — `syscall_set_enter_stack()`, user-pointer hardening
- `core/scheduler.c` — per-task sysenter stack wiring, ZOMBIE reschedule
- `boot/system_entry.asm` — `cld` on all entry paths
- `core/kernel.c` — `RING3_BOOT_TEST` boot hook
- `usr/shell.c` — `ring3` shell command + help entry
- `usr/test/user_calc.c` — observable, self-terminating Ring 3 test process
- `Makefile` — `RING3_BOOT_TEST=1` build flag
