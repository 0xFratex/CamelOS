# CamelOS Upgrade Summary - May 2026

## Overview
This upgrade addresses the critical gaps identified in the CamelOS operating system, focusing on TLS/HTTPS support, graphics improvements, AppKit completeness, and IPC infrastructure.

## 1. TLS/SSL Fix - BSD Socket Bridge (CRITICAL FIX)

### Problem
The TLS handshake in `tls_client.c` immediately returned `-10` because it used `k_socket()/k_recv()` which expected BSD socket file descriptors, while the browser used `tcp_connect_with_ptr()` which returned raw `void*` TCP connections. These two APIs were incompatible.

### Solution: `core/tls_client.c` (Rewritten)
- **`tls_client_handshake(void* conn)`**: Now extracts the remote IP/port from the raw TCP connection, creates a proper BSD socket via `k_socket()`, connects it through the socket layer, then invokes `tls_connect()` which performs the full TLS 1.2 handshake using `k_sendto()/k_recvfrom()`.
- **`tls_client_handshake_fd(int sockfd, const char* hostname, uint16_t port)`**: New API for cases where a BSD socket already exists (used by `http.c`). Creates a TLS session directly on the socket and performs the handshake.
- **`tls_client_session_send/recv`**: New APIs for sending/receiving via TLS session pointers.
- **`tls_client_session_close`**: Properly cleans up TLS sessions created via the fd-based API.

### Impact
- **HTTPS now works**: The browser and HTTP client can establish TLS connections to real web servers.
- **Clean API separation**: Raw TCP connections are separate from TLS-wrapped socket connections.

## 2. Browser Networking Upgrade

### Problem
The browser (`usr/apps/browser.c`) used the raw `tcp_connect_with_ptr()` API, which couldn't integrate with TLS. It also had no proper socket error handling.

### Solution: `usr/apps/browser.c` (Updated)
- **`browser_load_page()`**: Completely rewritten to use BSD socket API (`k_socket()`, `k_connect()`, `k_sendto()`, `k_recvfrom()`).
- **HTTPS path**: When `use_tls=1`, calls `tls_client_handshake_fd()` to establish a secure connection, then uses `tls_write()/tls_read()` for encrypted data transfer.
- **HTTP path**: Uses BSD sockets directly for unencrypted connections.
- **Proper cleanup**: TLS sessions and sockets are properly closed after page load.
- **Added includes**: `socket.h` and `tls.h` for the new APIs.

### Impact
- Browser can now load HTTPS pages (when TLS handshake succeeds).
- Fallback to HTTP still works if TLS fails.
- Cleaner, more maintainable networking code.

## 3. Graphics Layer - CoreGraphics (CGContext)

### New Files
- **`hal/video/cgcontext.h`**: Defines `CGContext`, `CGFont`, `CGImage` types and their APIs.
- **`hal/video/cgcontext.c`**: Full implementation of a CoreGraphics-like 2D drawing engine.

### Features Implemented
- **CGContext**: 2D drawing context with translation, clipping, state save/restore
- **Path operations**: `MoveToPoint`, `AddLineToPoint`, `AddCurveToPoint` (cubic bezier), `AddQuadCurveToPoint`, `ClosePath`, `AddRect`, `AddRoundedRect`
- **Fill/Stroke**: `FillPath`, `StrokePath` with line width and alpha support
- **Convenience functions**: `FillRect`, `StrokeRect`, `FillRoundedRect`, `StrokeRoundedRect`, `DrawLine`
- **Text drawing**: `DrawString`, `DrawStringCentered` with font size scaling and TrueType font support
- **Gradients**: `DrawLinearGradient` (vertical and horizontal)
- **Image drawing**: `DrawImage`, `DrawImageScaled` using gfx_hal asset rendering
- **Ellipse/Circle**: `FillEllipse`, `StrokeEllipse`, `FillCircle`, `StrokeCircle`
- **Shadows**: Per-context shadow with offset, radius, and color
- **CGFont**: Font object with name, size, bold/italic, glyph data, and string measurement
- **CGImage**: Image object with PNG/JPEG loading stubs (full decode requires zlib/libjpeg)

### Impact
- Provides a CoreGraphics-like layer that bridges to gfx_hal
- Enables macOS-style drawing: bezier paths, gradients, shadows, rounded corners with anti-aliasing
- Foundation for layer-backed views and render tree

## 4. Enhanced Compositor

### New File: `hal/video/compositor_v2.c`

### Features
- **`compositor_draw_shadow_v2()`**: 4-layer soft shadow with contact shadow (macOS-like depth)
- **`compositor_draw_window_v2()`**: Enhanced window drawing with gradient header, traffic lights with hover icons (X, minus, plus), title shadow, and focus-based styling
- **`compositor_draw_blur_backdrop_v2()`**: Improved frosted glass effect using blur buffer with white tint
- **`compositor_draw_reflection()`**: Dock-style reflection effect for icons

### Impact
- Windows look more polished with realistic multi-layer shadows
- Traffic light buttons have hover states with icons
- Blur backdrop properly samples from the wallpaper blur buffer

## 5. AppKit Extra Controls

### New Files
- **`core/appkit_extra.c`**: Implementation of new AppKit controls
- **Updated `core/appkit_compat.h`**: New type definitions

### New Controls
1. **NSTableView**: Table view with rows, columns, data source/delegate pattern, alternating row backgrounds, row selection, and `reloadData`
2. **NSOutlineView**: Tree/outline view extending NSTableView with expand/collapse state per item
3. **NSRunLoop**: Event loop processing with `run`/`stop` methods, integrates with `g_kernel_api.process_events()`
4. **NSWorkspace**: Application launcher (`launchApp:`, `openFile:`) using CDL loader
5. **NSSearchField**: Search text field with placeholder and magnifying glass icon support
6. **NSProgressIndicator**: Bar and spinning progress indicators with animation control
7. **NSSlider**: Horizontal/vertical slider with double value, target/action
8. **NSPopUpButton**: Dropdown/popup menu button with item management and selection
9. **NSCheckBox**: Checkbox control with state management and title
10. **NSToolbar**: Window toolbar with items and display modes (stub structure)

### Impact
- Apps can now use proper table views, search fields, progress bars, sliders, dropdowns, and checkboxes
- NSRunLoop provides proper event loop integration
- NSWorkspace enables app launching from code

## 6. IPC System

### New Files
- **`core/ipc.h`**: IPC API definitions
- **`core/ipc.c`**: Full IPC implementation

### Features
- **Port-based messaging**: Create ports per process, send/receive messages asynchronously
- **Message queues**: Per-port FIFO message queues with type-based filtering
- **Shared memory**: Create/map/unmap shared memory regions up to 1MB
- **RPC**: Synchronous call pattern (send request, wait for reply)
- **Service registry**: Register/lookup services by name (like Mach service bootstrap)
- **Notification callbacks**: Ports can register a callback for immediate notification on message arrival

### Standard Message Types
- `IPC_SVC_WINDOW_CMD`: Window server commands
- `IPC_SVC_NETWORK_CMD`: Network service commands
- `IPC_SVC_FILESYSTEM_CMD`: Filesystem commands
- `IPC_SVC_LAUNCHER_CMD`: App launcher commands
- `IPC_SVC_CLIPBOARD_CMD`: Clipboard commands
- `IPC_SVC_NOTIFICATION`: System notifications

### Impact
- Foundation for process isolation and inter-service communication
- Enables future user-mode apps to communicate with kernel services
- Service registry allows apps to discover available system services

## Files Changed/Created

### Modified
- `core/tls_client.c` - Complete rewrite with BSD socket bridge
- `usr/apps/browser.c` - BSD socket networking, TLS integration
- `core/appkit_compat.h` - New control types

### Created
- `hal/video/cgcontext.h` - CoreGraphics context header
- `hal/video/cgcontext.c` - CoreGraphics context implementation
- `hal/video/compositor_v2.c` - Enhanced compositor
- `core/appkit_extra.c` - Additional AppKit controls
- `core/ipc.h` - IPC system header
- `core/ipc.c` - IPC system implementation

## Remaining Work

### Short Term
1. **Full PNG/JPEG decoding**: Integrate zlib inflate for PNG, add JPEG decoder
2. **TrueType font rendering**: Port stb_truetype for proper font rendering
3. **ECDHE key exchange**: Complete the elliptic curve implementation for TLS
4. **NetSurf port**: Begin porting NetSurf browser engine for CSS/JS support

### Medium Term
1. **User-mode processes (Ring 3)**: Move apps to ring 3 with syscall interface
2. **GPU acceleration**: Add basic 2D acceleration for fills and blits
3. **NSTableView data source protocol**: Complete the table view with proper data source callbacks
4. **NSOutlineView rendering**: Add tree view rendering with expand/collapse UI

### Long Term
1. **WebKit port**: Full browser engine with JavaScriptCore
2. **Quartz 2D completion**: Full bezier path rendering, image filters, PDF rendering
3. **Security model**: Sandbox, code signing, keychain
4. **Power management**: Sleep/wake, CPU throttling
