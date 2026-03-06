# 🐪 Camel OS

**A Nostalgic Yet Modern Operating System**

*Vibe coded with passion over 2 months*

---

## 🌟 About Camel OS

Camel OS is a 32-bit x86 operating system that brings the nostalgic aesthetic of classic macOS X versions while embracing a modern, independent approach to system design. This project represents a labor of love, developed through intensive "vibe coding" sessions over the course of two months, resulting in a fully functional operating system with networking, GUI, and a suite of built-in applications.

### The Vibe Coding Philosophy

This project was built through "vibe coding" - a development approach where code flows naturally from inspiration and passion rather than rigid planning. Over 60 days, thousands of lines of code were written, debugged, and refined to create something that feels both familiar and fresh. Every component, from the bootloader to the desktop environment, was crafted with attention to detail and a love for elegant systems.

---

## ✨ Features

### 🎨 User Interface
- **macOS X-Inspired Design**: Beautiful Aqua-style interface with gradient backgrounds, rounded corners, and smooth animations
- **Desktop Environment**: Complete with draggable icons, context menus, and file management
- **Dock**: macOS-style dock with magnification effects and running app indicators
- **Menu Bar**: Dynamic menu system with Apple-style menus and system tray
- **Window Management**: Full window server with minimize, maximize, and close animations (Genie effect!)

### 🔐 Security & Setup
- **Screen Lock**: Password-protected lock screen with blur effects and time display
- **Welcome Setup**: First-boot configuration wizard for:
  - User account creation
  - Timezone selection (16 major timezones)
  - Theme selection (Aqua, Graphite, Sunset, Ocean, Forest)

### 💾 Installation System
- **Modern Installer**: macOS-inspired installation experience with:
  - Disk Utility for partition management
  - Disk health monitoring (SMART-like attributes)
  - Multiple filesystem support (PFS32, FAT32, NTFS, EXT4)
  - Progress tracking with detailed logs
  - System requirements checking

### 💽 Enhanced Disk Utility (v2.0)
- **Disk Benchmark**: Sequential/random read/write speed testing with SSD detection
- **Bad Sector Scan**: Quick, standard, thorough, and destructive scan modes
- **Disk Wipe**: Multi-pass secure erase (Zero fill, DoD 5220.22-M, Gutmann method)
- **Disk Clone**: Sector-by-sector or smart cloning with verification
- **Surface Scan**: Latency mapping and damaged sector detection
- **Filesystem Check**: PFS32, FAT32, NTFS, EXT4 integrity checking with repair
- **Operation Queue**: Schedule and manage multiple disk operations

### 📁 Filesystem
- **PFS32**: Custom native filesystem designed for Camel OS
- **Long filename support**: Up to 255 characters
- **Directory structure**: `/home`, `/usr`, `/etc` with proper permissions

### 🌐 Networking
- **TCP/IP Stack**: Full networking implementation
- **DHCP Client**: Automatic network configuration
- **DNS Resolution**: Domain name lookup support
- **HTTP/HTTPS**: Web protocol support with TLS
- **Network Drivers**: Support for RTL8139, RTL8169, and E1000 network cards

### 🖥️ Built-in Applications
- **Terminal**: Full command-line interface
- **Files**: File manager with desktop integration
- **Waterhole**: System monitor (activity monitor)
- **NetTools**: Network diagnostic utilities
- **TextEdit**: Text editor application
- **Browser**: Basic web browser with JavaScript engine

### 🔧 System Components
- **Kernel**: Custom microkernel with preemptive multitasking
- **Memory Management**: Virtual memory with paging
- **Process Scheduler**: Round-robin scheduling with priorities
- **Dynamic Library Loading**: CDL (Camel Dynamic Library) format
- **Hardware Abstraction Layer**: Clean driver architecture

### 🖥️ JavaScript Engine v2.0
- **ES6+ Support**: Modern JavaScript syntax and features
- **Promises**: Full async/await pattern support
- **Arrow Functions**: Concise function syntax
- **Template Literals**: String interpolation
- **Classes**: Object-oriented programming support
- **Modules**: Import/export functionality
- **Symbol Type**: Unique identifiers
- **BigInt**: Arbitrary precision integers
- **Map/Set**: Modern collection types
- **Array Methods**: map, filter, reduce, find, includes, flat, flatMap
- **Object Methods**: keys, values, entries, assign
- **JSON**: Full parse/stringify support
- **Math**: Complete math library with random, floor, ceil, round, etc.
- **Browser API**: DOM manipulation, fetch, setTimeout/setInterval

### 🎨 CSS Parser v2.0
- **Flexbox Layout**: Complete flex container and item support
  - flex-direction, flex-wrap, justify-content, align-items
  - flex-grow, flex-shrink, flex-basis, order, gap
- **CSS Grid**: Modern grid layout system
  - grid-template-columns, grid-template-rows
  - grid-gap, grid-area, justify-self, align-self
- **Modern Units**: px, em, rem, vw, vh, vmin, vmax, %, pt
- **Color Support**: Hex (#RGB, #RGBA, #RRGGBB, #RRGGBBAA), rgb(), rgba(), named colors
- **Box Model**: Complete margin, padding, border, width, height support
- **Typography**: font-size, font-family, font-weight, line-height, letter-spacing
- **Positioning**: static, relative, absolute, fixed, sticky
- **Visual Effects**: opacity, z-index, overflow, visibility
- **CSS Variables**: Custom properties with var() function
- **Transforms**: translate, rotate, scale, skew, matrix
- **Transitions & Animations**: Duration, timing-function, keyframes
- **Media Queries**: Responsive design support
- **Calc() Function**: Dynamic value calculations

---

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    User Space                           │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐      │
│  │Terminal │ │ Files   │ │Browser  │ │ SysMon  │      │
│  └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘      │
│       │           │           │           │            │
│  ┌────┴───────────┴───────────┴───────────┴────┐      │
│  │           Window Server & GUI               │      │
│  └────────────────────┬────────────────────────┘      │
├───────────────────────┼───────────────────────────────┤
│                   Kernel Space                        │
│  ┌────────────────────┴────────────────────────┐      │
│  │              System Kernel                   │      │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐    │      │
│  │  │Scheduler │ │ Memory   │ │  Net     │    │      │
│  │  │          │ │ Manager  │ │  Stack   │    │      │
│  │  └──────────┘ └──────────┘ └──────────┘    │      │
│  └────────────────────┬────────────────────────┘      │
│  ┌────────────────────┴────────────────────────┐      │
│  │        Hardware Abstraction Layer (HAL)      │      │
│  │  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐       │      │
│  │  │ CPU  │ │ VGA  │ │ ATA  │ │ NET  │       │      │
│  │  │Drivers│ │      │ │      │ │      │       │      │
│  │  └──────┘ └──────┘ └──────┘ └──────┘       │      │
│  └────────────────────┬────────────────────────┘      │
├───────────────────────┼───────────────────────────────┤
│                    Hardware                            │
│         CPU • RAM • Disk • Network • Display          │
└─────────────────────────────────────────────────────────┘
```

---

## 🚀 Getting Started

### Requirements
- QEMU or real x86 hardware
- 64MB RAM minimum (256MB recommended)
- 128MB disk space minimum

### Building from Source

```bash
# Clone the repository
git clone https://github.com/0xFratex/CamelOS.git
cd CamelOS

# Build everything
make clean && make all

# Run in QEMU
make install
```

### Creating Installation Media

```bash
# The build produces camel_install.iso
# Burn to CD/DVD or use with QEMU:
qemu-system-i386 -m 512 -cdrom camel_install.iso -drive file=disk.img,format=raw
```

---

## 📁 Project Structure

```
CamelOS/
├── boot/               # Bootloader and early boot code
│   ├── mbr.asm        # Master Boot Record
│   ├── multiboot.asm  # Multiboot header for GRUB
│   └── system_entry.asm
├── core/               # Core kernel components
│   ├── kernel.c       # Main kernel
│   ├── memory.c       # Memory management
│   ├── scheduler.c    # Process scheduling
│   ├── net.c          # Networking core
│   └── window_server.c
├── hal/                # Hardware Abstraction Layer
│   ├── cpu/           # CPU drivers (GDT, IDT, paging)
│   ├── drivers/       # Hardware drivers
│   └── video/         # Graphics and compositing
├── fs/                 # Filesystem
│   ├── pfs32.c        # Native PFS32 filesystem
│   └── disk.c         # Disk I/O
├── usr/                # User-space components
│   ├── apps/          # Applications
│   ├── lib/           # User libraries
│   ├── libs/          # Enhanced libraries
│   │   ├── js_engine.c    # JavaScript engine (v1)
│   │   ├── js_engine.h    # JS engine header (v1)
│   │   ├── js_engine_v2.h # Modern JS engine (ES6+)
│   │   ├── js_engine_v2.c # JS engine implementation
│   │   ├── css_parser_v2.h # Modern CSS parser
│   │   └── css_parser_v2.c # CSS parser implementation
│   ├── desktop.c      # Desktop environment
│   ├── dock.c         # Dock implementation
│   ├── menubar.c      # Menu bar
│   ├── screenlock.c   # Lock screen
│   └── welcome_setup.c # First-boot setup
├── installer/          # Installation system
│   ├── installer_main.c
│   ├── disk_health.c   # SMART monitoring
│   ├── disk_tools.c    # Extended disk utilities (v2.0)
│   ├── disk_tools.h    # Disk tools header
│   ├── partition_tool.c
│   └── sys_requirements.c
├── kernel/             # Kernel assets
├── sys/                # System API
└── assets/             # Images, sounds, fonts
```

---

## 🎯 System Requirements Check

The installer includes a comprehensive system requirements checker:

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| CPU       | 32-bit x86 | Pentium II+ |
| RAM       | 64 MB | 256 MB |
| Disk      | 128 MB | 1 GB |
| Display   | 640x480 | 1024x768 |
| Input     | Keyboard | Keyboard + Mouse |

---

## 🎨 Themes

Camel OS includes 5 beautiful themes:

1. **Aqua** - Classic blue, the default experience
2. **Graphite** - Professional grey tones
3. **Sunset** - Warm orange accents
4. **Ocean** - Calm teal colors
5. **Forest** - Natural green palette

---

## 🔌 Driver Support

### Storage
- IDE/ATA (Primary and Secondary)
- AHCI (Basic SATA support)

### Network
- Realtek RTL8139
- Realtek RTL8169
- Intel E1000

### Input
- PS/2 Keyboard
- PS/2 Mouse (scroll wheel supported)

### Audio
- PC Speaker
- Sound Blaster 16

### Display
- VGA (VBE modes)
- 1024x768x32 default resolution

---

## 🛠️ Development

### Code Style
- C with minimal runtime dependencies
- NASM for assembly
- Consistent naming conventions
- Modular architecture

### Building
```bash
make            # Build all
make clean      # Clean build artifacts
make install    # Build and run in QEMU
make run        # Run installed system
```

### Debugging
```bash
# Enable QEMU debug stub
make debug

# Connect GDB
gdb system.elf
target remote :1234
```

---

## 📜 License

This project is provided for educational and personal use. Feel free to explore, learn, and build upon it.

---

## 🙏 Acknowledgments

- Inspired by the elegance of classic macOS X
- Built with love for systems programming
- Crafted through countless hours of vibe coding

---

## 📊 Project Stats

- **Development Time**: 2+ months
- **Lines of Code**: ~35,000+
- **Components**: Bootloader, Kernel, Drivers, GUI, Apps, Modern Web Engine
- **Files Created**: 120+
- **JavaScript Engine**: ES6+ with Promises, async/await, modules
- **CSS Support**: Flexbox, Grid, Modern CSS3 properties
- **Disk Tools**: 6 comprehensive utilities for disk management

---

## 🐫 Why "Camel"?

Like a camel surviving in the desert, this OS is designed to be resilient and self-sufficient. It carries everything it needs within itself - a complete operating system that doesn't depend on external foundations. Plus, camels are pretty cool.

---

*Made with 💙 by the Camel OS Team*

*"Where nostalgia meets innovation"*
