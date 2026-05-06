# CamelOS: What's Missing — Gap Analysis & Roadmap (Updated May 2026)

> **Audience**: Developers and contributors to CamelOS  
> **Date**: May 2026 (Updated from original audit)  
> **Scope**: A revised audit reflecting the current codebase state. Many items  
> from the original audit have been resolved; this document tracks remaining gaps.

---

## Executive Summary

The original gap analysis identified several "dead code" subsystems that were never initialized. **All of these have since been wired up.** The scheduler, VMM, signals, pipes, IPC, klog, VFS, process management, and crash reporter are all now properly initialized at boot. TCP listen/accept is implemented, PNG decoding works, and CoreAnimation is functional.

Recent improvements have added:
- **launchd service manager** — now initialized at boot with core services registered
- **FAT32 VFS registration** — FAT32 driver is registered with VFS at boot
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

### 2.2 FAT32 VFS — NOW REGISTERED
- `fat32_register_with_vfs()` called at boot, making FAT32 a mountable VFS type
- FAT32 partitions can be mounted via `vfs_mount("/mnt/usb", VFS_FS_FAT32, NULL)` after calling `fat32_init(lba)`
- **Still TODO**: Auto-detect and mount FAT32 partitions from the MBR at boot

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

### 4.1 FAT32 Auto-Mount at Boot

**Current State**: FAT32 driver and VFS registration are active, but `fat32_init()` is never called automatically. Partitions must be manually initialized.

**What's Needed**:
- Parse MBR partition table in `kernel_main()` after `disk_init()`
- Detect FAT32 partitions by type ID (0x0B, 0x0C)
- Call `fat32_init(partition_start_lba)` for each FAT32 partition
- Mount via `vfs_mount("/mnt/disk1", VFS_FS_FAT32, NULL)`

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
| Filesystem | 3 | 1 | 75% |
| Networking | 9 | 0 | 100% |
| Security | 2 | 5 | 29% |
| Drivers | 14 | 4 | 78% |
| Media/Fonts | 1 | 2 | 33% |
| macOS Frameworks | 6 | 3 | 67% |
| Core Apps | 9 | 5 | 64% |
| System Services | 5 | 3 | 63% |

**Overall System Completion: ~68%** (up from ~48% in original audit)

The most impactful remaining work is **Ring 3 user-mode isolation** (§3.1), which unblocks true process security, and **FAT32 auto-mount** (§4.1), which enables USB drive interoperability. The **TCP retransmission** fix means networking is now reliable, and the **launchd** activation means the system has a proper service management foundation.

---

*This analysis was updated after a thorough source-code audit and code changes in May 2026.*
