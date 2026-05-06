# CamelOS: What's Missing — Gap Analysis & Roadmap (Updated May 2026)

> **Audience**: Developers and contributors to CamelOS  
> **Date**: May 2026 (Updated from original audit)  
> **Scope**: A revised audit reflecting the current codebase state. Many items  
> from the original audit have been resolved; this document tracks remaining gaps.

---

## Executive Summary

The original gap analysis identified several "dead code" subsystems that were never initialized. **All of these have since been wired up.** The scheduler, VMM, signals, pipes, IPC, klog, VFS, process management, and crash reporter are all now properly initialized at boot. TCP listen/accept is implemented, PNG decoding works, and CoreAnimation is functional.

Recent improvements have added:
- **launchd service manager** — now initialized at boot with core services registered and proper start functions
- **launchd NULL-pointer crash fix** — services registered without `start_func` no longer cause a page fault; passive services are marked running automatically. This was the root cause of the boot-time page fault at 0x5720cf69.
- **FAT32 VFS registration + auto-mount** — FAT32 driver is registered with VFS at boot, and MBR partition table is now scanned to auto-detect and mount FAT32 partitions (type 0x0B, 0x0C)
- **Clipboard system** — full text and file copy/cut/paste support
- **TCP retransmission timer** — exponential backoff with max retry limit
- **IPC RPC completion** — synchronous RPC with reply port and timeout

**Remaining gaps** are primarily: Ring 3 user-mode isolation, TrueType fonts, JPEG decoding, USB HID, ACPI, and several macOS-faithful features.

---

## 1. Previously Dead Code — Now ACTIVE ✅

| Subsystem | Status | Notes |
|-----------|--------|-------|
| Scheduler | ✅ **Active** | `scheduler_init()` called from `kernel_init_hal()`, timer ISR wired |
| VMM | ✅ **Active** | `vmm_init()` called after `init_paging()`, page faults routed to `vmm_handle_page_fault()` |
| Signals | ✅ **Active** | `signal_init()` called from `kernel_init_hal()` |
| Pipes | ✅ **Active** | `pipe_init()` called from `kernel_init_hal()` |
| IPC | ✅ **Active** | `ipc_init()` called from `kernel_init_hal()`, RPC now functional |
| Kernel Logger | ✅ **Active** | `klog_init()` called from `kernel_init_hal()` |
| VFS | ✅ **Active** | `vfs_init()` called from `kernel_main()`, root mounted as PFS32 |
| Process Mgmt | ✅ **Active** | `process_init()` called, fork/exec/wait/exit implemented |
| Crash Reporter | ✅ **Active** | `crash_reporter_init()` called, stack unwinding + log files |
| TCP Listen/Accept | ✅ **Active** | Full server-side socket support |
| PNG Decoder | ✅ **Active** | RGB/RGBA with zlib decompression |
| CoreAnimation | ✅ **Active** | Basic animation interpolation with timing functions |
| launchd | ✅ **Active** | `launchd_init()` + `launchd_boot_start()` called, health monitoring in main loop |
| FAT32 VFS | ✅ **Active** | `fat32_register_with_vfs()` called at boot |

---

## 2. Recently Fixed Gaps

### 2.1 launchd Service Manager — NOW WIRED UP
- `launchd_init()` and `launchd_boot_start()` called from `kernel_main()`
- Core services registered: NetworkStack, WindowServer, CrashReporter, IPCService
- `launchd_check_health()` called from main event loop for crash monitoring
- Dependency graph: WindowServer depends on NetworkStack

### 2.2 FAT32 VFS — NOW REGISTERED + AUTO-MOUNT
- `fat32_register_with_vfs()` called at boot, making FAT32 a mountable VFS type
- **FAT32 auto-mount**: MBR partition table is now parsed at boot; FAT32 partitions (type 0x0B, 0x0C) are automatically detected, initialized with `fat32_init()`, and mounted at `/mnt/disk1`, `/mnt/disk2`, etc.
- FAT32 partitions can also be manually mounted via `vfs_mount("/mnt/usb", VFS_FS_FAT32, NULL)`

### 2.3 Clipboard — NOW FUNCTIONAL
- Full clipboard system with text copy/paste (4KB) and file copy/cut support
- `clipboard_copy_text()`, `clipboard_paste_text()`, `clipboard_copy_file()`
- Legacy compatibility globals preserved for existing code
- **Still TODO**: Wire into app UI (TextEdit, Terminal, Files app context menus)

### 2.4 TCP Retransmission — NOW IMPLEMENTED
- `tcp_retransmit_check()` called from main event loop
- Exponential backoff: timeout doubles on each retransmit, capped at 30s
- Max 5 retransmit attempts before RST/close
- Handles SYN retransmit, data retransmit from send buffer, FIN retransmit
- ACK reception resets retransmit count and timer

### 2.5 IPC RPC — NOW FUNCTIONAL
- `ipc_rpc_call()` creates a temporary reply port, embeds it in the request
- Polls for `IPC_MSG_REPLY` with a configurable timeout
- Properly cleans up reply port on success or timeout

### 2.6 launchd NULL-Pointer Crash — NOW FIXED
- `launchd_start_service()` and `launchd_check_health()` now check for NULL `start_func` before calling it
- Services registered without a `start_func` (passive/tracked-only) are automatically marked as RUNNING
- Actual start functions added for NetworkStack, WindowServer, CrashReporter, and IPCService that verify their corresponding subsystem is active
- **This was the root cause of the page fault at 0x5720cf69** — `launchd_boot_start()` called `start_func()` on NULL function pointers

---

## 3. Remaining Critical Gaps (P0)

### 3.1 No User/Kernel Separation — Everything Runs in Ring 0

**Current State**: GDT has User Code (0x18) and User Data (0x20) segments and a TSS descriptor, but no code transitions to Ring 3. All processes run at CPL 0.

**What's Needed**:
- Implement `sysenter`/`sysexit` or `int 0x80` privilege transition
- Create user-mode process loader that sets up Ring 3 stack + entry point
- Modify scheduler to do Ring 0→Ring 3 return on context switch
- Enforce page-table User/Supervisor bits so Ring 3 can't touch kernel pages
- Move signal delivery to use userspace signal frames

**Files to Modify**: `hal/cpu/gdt.c`, `hal/cpu/idt.c`, `core/task.c`, `core/scheduler.c`, `core/signal.c`

### 3.2 No fork() / exec() at Ring 3

**Current State**: `process_fork()` and `process_exec()` exist and work in Ring 0, but they need Ring 3 support (separate address spaces, user-mode entry) to be useful for security.

**What's Needed**: This is blocked on §3.1. Once Ring 3 works, fork/exec already have the VMM infrastructure (COW fork, per-process address spaces).

---

## 4. Remaining High-Priority Gaps (P1)

### 4.1 FAT32 Auto-Mount at Boot — NOW IMPLEMENTED ✅

**Current State**: FAT32 driver is registered with VFS, and the MBR partition table is now automatically scanned at boot. FAT32 partitions (type 0x0B, 0x0C) are detected, initialized via `fat32_init()`, and mounted at `/mnt/disk1`, `/mnt/disk2`, etc.

**Still TODO**:
- Handle extended partition tables (EBR) for partitions beyond 4
- GPT partition table support
- Mount options (read-only, no-atime, etc.)

### 4.2 No TrueType / OpenType Font Rendering

**Current State**: `common/font.c` uses a hardcoded bitmap font. `CGFont` has a TrueType stub that falls back to the bitmap font. No vector rendering.

**What's Needed**: Port `stb_truetype.h` (single-header, public domain) for TrueType glyph rasterization. Add font fallback chains.

### 4.3 No JPEG Decoding

**Current State**: `CGImage` declares `CGImageLoadJPEG` but it's a stub. PNG works, JPEG doesn't.

**What's Needed**: Add a minimal JPEG decoder (stb_image or custom baseline JPEG).

### 4.4 No USB HID Driver

**Current State**: USB xHCI controller init exists but is minimal. USB core is a mock. No HID class driver for keyboards/mice.

**What's Needed**: Proper USB enumeration, HID boot protocol driver for keyboard and mouse. This is critical for modern USB-only hardware.

### 4.5 No File Permission Enforcement

**Current State**: PFS32 has permission bits but they're never checked. `bsd_mprotect()` is a no-op. Any process can read/write any file.

**What's Needed**: Permission checks on every filesystem operation, per-process uid/gid, setuid support.

### 4.6 No Software Update System

**Current State**: No mechanism to update the OS.

**What's Needed**: HTTP-based manifest, Software Update preference pane, secure download with TLS + signature verification.

---

## 5. Remaining Medium-Priority Gaps (P2)

| Gap | Notes |
|-----|-------|
| ACPI / Power Management | No ACPI parsing, no sleep/wake, no battery |
| Intel HDA / AC97 Audio | Only SB16 supported |
| NVMe Driver | Only IDE/ATA and basic AHCI |
| VirtIO Drivers | No virtio-net/blk/gpu for QEMU |
| GPU / 2D Acceleration | All rendering is software |
| PDF Rendering | No PDF parser |
| Keychain | No encrypted credential store |
| Code Signing | No Gatekeeper equivalent |
| ASLR | All processes load at fixed addresses |
| Sandbox | Apps have full system access |
| Accessibility | No screen reader, no VoiceOver equivalent |
| i18n / Keyboard Layouts | Only US English |

---

## 6. Missing Core Applications (P1–P2)

| Application | macOS Equivalent | Status |
|-------------|-----------------|--------|
| Calculator | Calculator | ✅ Implemented |
| Console | Console | ✅ Implemented |
| Disk Utility | Disk Utility | ✅ UI implemented, backend stubs |
| Process Monitor | Activity Monitor | ✅ Implemented |
| Image Viewer | Preview | ❌ Needs JPEG/PNG viewer app |
| Media Player | QuickTime | ❌ Needs audio server |
| Archive Manager | Archive Utility | ❌ No ZIP/TAR |
| Help Viewer | Help Viewer | ❌ |
| Font Book | Font Book | ❌ |
| Keychain Access | Keychain Access | ❌ |

---

## 7. Updated Statistics

| Category | Existing | Missing | Completion |
|----------|----------|---------|------------|
| Kernel Core | 8 | 2 | 80% |
| Memory/VM | 3 | 1 | 75% |
| Process/Scheduling | 5 | 1 | 83% |
| IPC | 4 | 0 | 100% |
| Filesystem | 3 | 0 | 100% |
| Networking | 9 | 0 | 100% |
| Security | 2 | 5 | 29% |
| Drivers | 14 | 4 | 78% |
| Media/Fonts | 1 | 2 | 33% |
| macOS Frameworks | 6 | 3 | 67% |
| Core Apps | 9 | 5 | 64% |
| System Services | 5 | 3 | 63% |

**Overall System Completion: ~72%** (up from ~68% in previous audit)

The most impactful remaining work is **Ring 3 user-mode isolation** (§3.1), which unblocks true process security. The **launchd NULL-pointer crash** fix eliminates the boot-time page fault at 0x5720cf69, the **FAT32 auto-mount** enables USB drive interoperability, the **TCP retransmission** fix means networking is now reliable, and the **launchd** activation means the system has a proper service management foundation with real start functions.

---

*This analysis was updated after a thorough source-code audit and code changes in May 2026.*
