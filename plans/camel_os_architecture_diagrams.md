# Camel OS Architecture Diagrams

## Current System Architecture

```mermaid
graph TB
    subgraph User Space
        APPS[CDL Applications]
        APPS --> TERMINAL[Terminal]
        APPS --> FILES[Files]
        APPS --> BROWSER[Browser]
        APPS --> TEXTEDIT[TextEdit]
        APPS --> NETTOOLS[NetTools]
    end

    subgraph Kernel Core
        KERNEL[kernel.c]
        TASK[task.c - Cooperative]
        MEMORY[memory.c - Simple Heap]
        WINDOW[window_server.c]
        CDL[cdl_loader.c]
    end

    subgraph HAL
        CPU[CPU - GDT/IDT/Paging]
        DRIVERS[Drivers]
        DRIVERS --> VGA[VGA]
        DRIVERS --> KBD[Keyboard]
        DRIVERS --> MOUSE[Mouse]
        DRIVERS --> NET[RTL8139]
        DRIVERS --> ATA[ATA]
    end

    subgraph Filesystem
        PFS32[PFS32]
        DISK[disk.c]
    end

    subgraph Network Stack
        NET_CORE[net.c]
        TCP[tcp.c]
        HTTP[http.c]
        DNS[dns.c]
        TLS[tls.c]
    end

    APPS --> CDL
    CDL --> KERNEL
    KERNEL --> TASK
    KERNEL --> MEMORY
    KERNEL --> WINDOW
    KERNEL --> CPU
    KERNEL --> DRIVERS
    KERNEL --> PFS32
    KERNEL --> NET_CORE
    NET_CORE --> TCP
    TCP --> HTTP
    NET_CORE --> DNS
    HTTP --> TLS
```

## Proposed System Architecture

```mermaid
graph TB
    subgraph User Space Ring 3
        APPS[CDL Applications]
        APPS --> TERMINAL[Terminal]
        APPS --> FILES[Files]
        APPS --> BROWSER[Browser]
        APPS --> SETTINGS[Settings NEW]
        APPS --> CALC[Calculator NEW]
        APPS --> IMGVIEW[Image Viewer NEW]
        
        SYSTRAY[System Tray NEW]
        NOTIF[Notification Center NEW]
        
        LIBC[User Library - syscall wrappers]
    end

    subgraph System Services
        SVC_MGR[Service Manager NEW]
        AUDIO_SVC[Audio Server NEW]
        POWER_SVC[Power Manager NEW]
        LOG_SVC[Log Daemon NEW]
    end

    subgraph Kernel Core Ring 0
        SYSCALL[syscall.c - INT 0x80]
        SCHED[scheduler.c - Preemptive NEW]
        VMM[vmm.c - Virtual Memory NEW]
        IPC[ipc.c - Pipes/SHM NEW]
        SIGNAL[signal.c NEW]
        KLOG[klog.c NEW]
        
        WINDOW[window_server.c]
        CDL[cdl_loader.c]
        MEMORY[memory.c]
    end

    subgraph HAL
        CPU[CPU Layer]
        CPU --> GDT[GDT + TSS]
        CPU --> IDT[IDT]
        CPU --> PAGING[Paging]
        CPU --> SYSENTRY[syscall.asm NEW]
        CPU --> CTXSW[context_switch.asm NEW]
        
        DRIVERS[Driver Framework]
        DRIVERS --> INPUT[Input Layer]
        DRIVERS --> STORAGE[Storage Layer]
        DRIVERS --> NETWORK[Network Layer]
        DRIVERS --> AUDIO[Audio Layer NEW]
        DRIVERS --> USB[USB Stack]
        
        ACPI[ACPI Manager NEW]
    end

    subgraph Virtual Filesystem
        VFS[vfs.c - VFS Layer NEW]
        PFS32[PFS32]
        DEVFS[devfs - Device Files NEW]
        PROCFS[procfs - Process Info NEW]
    end

    subgraph Network Stack
        NET_CORE[net.c]
        SOCKET[socket.c]
        TCP[tcp.c]
        UDP[udp.c NEW]
        HTTP[http.c]
        DNS[dns.c]
        TLS[tls.c]
    end

    APPS --> LIBC
    LIBC --> SYSCALL
    SYSCALL --> SCHED
    SCHED --> VMM
    SCHED --> IPC
    SCHED --> SIGNAL
    
    SVC_MGR --> SYSCALL
    AUDIO_SVC --> SYSCALL
    POWER_SVC --> ACPI
    
    WINDOW --> APPS
    CDL --> APPS
    
    VFS --> PFS32
    VFS --> DEVFS
    VFS --> PROCFS
```

## Process Lifecycle State Machine

```mermaid
stateDiagram-v2
    [*] --> CREATED: fork
    CREATED --> READY: scheduler_add
    READY --> RUNNING: schedule
    RUNNING --> READY: preempt
    RUNNING --> BLOCKED: wait/sleep/io
    BLOCKED --> READY: wakeup
    RUNNING --> ZOMBIE: exit
    ZOMBIE --> [*]: parent_wait
    
    RUNNING --> STOPPED: SIGSTOP
    STOPPED --> READY: SIGCONT
```

## Memory Management Architecture

```mermaid
graph TB
    subgraph Virtual Memory Manager
        VMM[VMM Core]
        PD[Page Directory per Process]
        PT[Page Tables]
        PF[Page Fault Handler]
    end

    subgraph Physical Memory
        PMM[Physical Memory Manager]
        FRAMES[Frame Allocator]
        DMA[DMA Zone]
        NORMAL[Normal Zone]
    end

    subgraph Kernel Memory
        KMALLOC[kmalloc - Slab]
        VMALLOC[vmalloc - Virtual]
        KSTACK[Kernel Stacks]
    end

    subgraph Process Memory Layout
        CODE[Code Segment 0x08048000]
        DATA[Data Segment]
        HEAP[Heap - brk/sbrk]
        MMAP[Memory Mappings - mmap]
        STACK[User Stack]
        KSTACK2[Kernel Stack]
    end

    VMM --> PD
    PD --> PT
    PT --> PMM
    PF --> VMM
    
    PMM --> FRAMES
    FRAMES --> DMA
    FRAMES --> NORMAL
    
    VMM --> KMALLOC
    VMM --> VMALLOC
    
    CODE --> PD
    DATA --> PD
    HEAP --> PD
    MMAP --> PD
    STACK --> PD
```

## IPC Mechanisms

```mermaid
graph TB
    subgraph IPC Subsystem
        IPC_CORE[IPC Manager]
    end

    subgraph Data Transfer
        PIPE[Pipes]
        PIPE --> ANON_PIPE[Anonymous Pipe]
        PIPE --> NAMED_PIPE[Named Pipe FIFO]
        
        SHM[Shared Memory]
        SHM --> SHMGET[shmget]
        SHM --> SHMAT[shmat]
        SHM --> SHMDT[shmdt]
    end

    subgraph Messaging
        MSGQ[Message Queues]
        MSGQ --> MSGSND[msgsnd]
        MSGQ --> MSGRCV[msgrcv]
        
        SIGNALS[Signals]
        SIGNALS --> SIG_HANDLER[Signal Handlers]
        SIGNALS --> SIG_MASK[Signal Masks]
    end

    subgraph Synchronization
        SEM[Semaphores]
        SEM --> SEMGET[semget]
        SEM --> SEMOP[semop]
        
        MUTEX[Mutexes]
        MUTEX --> LOCK[lock]
        MUTEX --> UNLOCK[unlock]
    end

    subgraph Network IPC
        UNIX_SOCK[Unix Domain Sockets]
        UNIX_SOCK --> STREAM_SOCK[SOCK_STREAM]
        UNIX_SOCK --> DGRAM_SOCK[SOCK_DGRAM]
    end

    IPC_CORE --> PIPE
    IPC_CORE --> SHM
    IPC_CORE --> MSGQ
    IPC_CORE --> SIGNALS
    IPC_CORE --> SEM
    IPC_CORE --> MUTEX
    IPC_CORE --> UNIX_SOCK
```

## Window System Architecture

```mermaid
graph TB
    subgraph Applications
        APP1[App 1]
        APP2[App 2]
        APP3[App 3]
    end

    subgraph Window Server
        WS[window_server.c]
        WREG[Window Registry]
        ZORDER[Z-Order Manager]
        STATE[State Manager]
        EVENTS[Event Dispatcher]
    end

    subgraph Compositor
        COMP[compositor.c]
        RENDER[Renderer]
        EFFECTS[Effects Engine]
        ANIM[Animation System]
    end

    subgraph Input System
        INPUT[input.c]
        MOUSE[Mouse Handler]
        KBD[Keyboard Handler]
        FOCUS[Focus Manager]
    end

    subgraph Output
        FRAMEBUF[Framebuffer]
        BACKBUF[Back Buffer]
        GPU[GPU/GFX HAL]
    end

    APP1 --> WS
    APP2 --> WS
    APP3 --> WS
    
    WS --> WREG
    WS --> ZORDER
    WS --> STATE
    WS --> EVENTS
    
    WS --> COMP
    COMP --> RENDER
    COMP --> EFFECTS
    COMP --> ANIM
    
    INPUT --> EVENTS
    MOUSE --> INPUT
    KBD --> INPUT
    FOCUS --> INPUT
    
    RENDER --> FRAMEBUF
    FRAMEBUF --> BACKBUF
    BACKBUF --> GPU
```

## Network Stack Architecture

```mermaid
graph TB
    subgraph Application Layer
        HTTP[HTTP Client]
        BROWSER[Browser App]
    end

    subgraph Transport Layer
        TCP[TCP Module]
        UDP[UDP Module NEW]
    end

    subgraph Network Layer
        IP[IP Layer]
        ICMP[ICMP - Ping]
        ARP[ARP Module]
    end

    subgraph Link Layer
        ETH[Ethernet]
        NETIF[Network Interface]
    end

    subgraph Physical Layer
        DRV[Driver]
        RTL8139[RTL8139]
        E1000[E1000 NEW]
    end

    subgraph DNS Resolution
        DNS[DNS Resolver]
        CACHE[DNS Cache]
    end

    subgraph Security
        TLS[TLS Module]
        CERT[Certificate Store]
    end

    BROWSER --> HTTP
    HTTP --> TCP
    TCP --> IP
    UDP --> IP
    IP --> ICMP
    IP --> ARP
    IP --> ETH
    ETH --> NETIF
    NETIF --> DRV
    DRV --> RTL8139
    DRV --> E1000
    
    BROWSER --> DNS
    DNS --> UDP
    DNS --> CACHE
    
    HTTP --> TLS
    TLS --> CERT
```

## Driver Architecture

```mermaid
graph TB
    subgraph Device Manager
        DEVMGR[Device Manager NEW]
        DTREE[Device Tree]
        DRVREG[Driver Registry]
    end

    subgraph Bus Drivers
        PCI[PCI Driver]
        USB[USB Core]
        ISA[ISA/Legacy]
    end

    subgraph USB Stack
        XHCI[xHCI - USB 3.0]
        EHCI[EHCI - USB 2.0 NEW]
        UHCI[UHCI - USB 1.1 NEW]
        
        USBHID[HID Driver NEW]
        USBMS[Mass Storage NEW]
        USBAUDIO[Audio Class NEW]
    end

    subgraph Storage Class
        ATA[ATA/IDE]
        AHCI[AHCI/SATA NEW]
        NVMe[NVMe NEW]
        SDHCI[SD Card NEW]
    end

    subgraph Network Class
        RTL8139[RTL8139]
        RTL8169[RTL8169]
        E1000[Intel E1000 NEW]
        VIRTIO[VirtIO NEW]
    end

    subgraph Audio Class
        SB16[Sound Blaster 16]
        HDA[Intel HDA NEW]
        AC97[AC97 NEW]
    end

    subgraph Input Class
        KEYBOARD[Keyboard]
        MOUSE[Mouse]
        TABLET[Tablet NEW]
    end

    subgraph Display Class
        VGA[VGA]
        BOCHS[Bochs/QEMU]
        VBE[VBE LFB]
    end

    DEVMGR --> DTREE
    DEVMGR --> DRVREG
    
    DEVMGR --> PCI
    DEVMGR --> USB
    DEVMGR --> ISA
    
    USB --> XHCI
    USB --> EHCI
    USB --> UHCI
    USB --> USBHID
    USB --> USBMS
    USB --> USBAUDIO
    
    DEVMGR --> ATA
    DEVMGR --> AHCI
    DEVMGR --> NVMe
    DEVMGR --> SDHCI
    
    DEVMGR --> RTL8139
    DEVMGR --> RTL8169
    DEVMGR --> E1000
    DEVMGR --> VIRTIO
    
    DEVMGR --> SB16
    DEVMGR --> HDA
    DEVMGR --> AC97
    
    DEVMGR --> KEYBOARD
    DEVMGR --> MOUSE
    DEVMGR --> TABLET
    
    DEVMGR --> VGA
    DEVMGR --> BOCHS
    DEVMGR --> VBE
```

## Service Management Architecture

```mermaid
graph TB
    subgraph Service Manager
        SVC_MGR[Service Manager]
        CONFIG[Service Configs]
        STATE[State Machine]
        DEPS[Dependency Graph]
    end

    subgraph Service Types
        SYSTEM[System Services]
        NETWORK[Network Services]
        DAEMON[Background Daemons]
    end

    subgraph Services
        INIT[Init Service]
        NET_SVC[Network Service]
        AUDIO_SVC[Audio Service]
        POWER_SVC[Power Service]
        LOG_SVC[Logging Service]
        CRON_SVC[Cron Service NEW]
    end

    subgraph Service Lifecycle
        STOPPED[Stopped]
        STARTING[Starting]
        RUNNING[Running]
        STOPPING[Stopping]
        FAILED[Failed]
    end

    SVC_MGR --> CONFIG
    SVC_MGR --> STATE
    SVC_MGR --> DEPS
    
    CONFIG --> SYSTEM
    CONFIG --> NETWORK
    CONFIG --> DAEMON
    
    SYSTEM --> INIT
    NETWORK --> NET_SVC
    DAEMON --> AUDIO_SVC
    DAEMON --> POWER_SVC
    DAEMON --> LOG_SVC
    DAEMON --> CRON_SVC
    
    STATE --> STOPPED
    STATE --> STARTING
    STATE --> RUNNING
    STATE --> STOPPING
    STATE --> FAILED
    
    STOPPED --> STARTING: start
    STARTING --> RUNNING: success
    STARTING --> FAILED: error
    RUNNING --> STOPPING: stop
    STOPPING --> STOPPED: done
    FAILED --> STARTING: restart
```

## Boot Sequence

```mermaid
sequenceDiagram
    participant BIOS as BIOS/UEFI
    participant MBR as MBR
    participant BOOT as Bootloader
    participant KERN as Kernel
    participant HAL as HAL
    participant INIT as Init Service
    participant GUI as GUI System

    BIOS->>MBR: Load and execute MBR
    MBR->>MBR: Find active partition
    MBR->>BOOT: Load bootloader
    BOOT->>BOOT: Parse config
    BOOT->>KERN: Load kernel ELF
    KERN->>KERN: Initialize BSS
    KERN->>HAL: init_hal
    HAL->>HAL: init_gdt
    HAL->>HAL: init_idt
    HAL->>HAL: init_paging
    HAL->>HAL: init_heap
    HAL->>HAL: init_apic
    HAL->>HAL: init_timer
    KERN->>KERN: init_scheduler NEW
    KERN->>KERN: init_vmm NEW
    KERN->>KERN: init_syscall NEW
    KERN->>KERN: init_ipc NEW
    KERN->>KERN: init_filesystem
    KERN->>KERN: init_network
    KERN->>INIT: Start init service NEW
    INIT->>INIT: Load service configs
    INIT->>GUI: Start GUI service
    GUI->>GUI: Init window server
    GUI->>GUI: Start desktop
    GUI->>GUI: Show login screen
```

## System Call Flow

```mermaid
sequenceDiagram
    participant USER as User Process Ring 3
    participant LIBC as libc wrapper
    participant CPU as CPU
    participant SYSCALL as syscall_handler
    participant KERN as Kernel Function

    USER->>LIBC: Call read fd buf count
    LIBC->>LIBC: Setup registers
    Note over LIBC: eax=SYS_read<br/>ebx=fd<br/>ecx=buf<br/>edx=count
    LIBC->>CPU: int 0x80
    CPU->>CPU: Switch to Ring 0
    CPU->>CPU: Save user state
    CPU->>SYSCALL: syscall_entry
    SYSCALL->>SYSCALL: Lookup syscall table
    SYSCALL->>SYSCALL: Validate arguments
    SYSCALL->>KERN: sys_read fd buf count
    KERN->>KERN: Check file descriptor
    KERN->>KERN: Check permissions
    KERN->>KERN: Perform read
    KERN-->>SYSCALL: Return result
    SYSCALL-->>CPU: Set return value
    CPU->>CPU: Restore user state
    CPU->>CPU: Switch to Ring 3
    CPU-->>LIBC: Return from int 0x80
    LIBC-->>USER: Return result
```

## Desktop Environment Components

```mermaid
graph TB
    subgraph Desktop Environment
        DESKTOP[Desktop Manager]
        DOCK[Dock]
        SYSTRAY[System Tray NEW]
        NOTIF[Notifications NEW]
    end

    subgraph Window Management
        WSMGR[Window Manager]
        WORKSPACE[Workspaces NEW]
        SNAP[Window Snapping]
        TILE[Tiling Mode NEW]
        EXPOSE[Expose View NEW]
    end

    subgraph UI Components
        MENU[Menu Bar]
        CTX[Context Menus]
        DIALOG[Dialogs]
        PICKER[File Picker]
    end

    subgraph System UI
        LOGIN[Login Screen NEW]
        LOCK[Lock Screen NEW]
        SHUTDOWN[Shutdown Dialog]
    end

    DESKTOP --> DOCK
    DESKTOP --> SYSTRAY
    DESKTOP --> NOTIF
    
    WSMGR --> WORKSPACE
    WSMGR --> SNAP
    WSMGR --> TILE
    WSMGR --> EXPOSE
    
    DESKTOP --> MENU
    DESKTOP --> CTX
    DESKTOP --> DIALOG
    DESKTOP --> PICKER
    
    DESKTOP --> LOGIN
    DESKTOP --> LOCK
    DESKTOP --> SHUTDOWN
```

---

## Summary

These architecture diagrams illustrate the proposed improvements to Camel OS across all major subsystems:

1. **Core System**: Preemptive scheduling, virtual memory, IPC, and system calls
2. **User Space**: Ring 3 execution, user libraries, and system services
3. **Hardware**: Enhanced driver framework, USB stack, audio, and power management
4. **User Experience**: Desktop environment, notifications, and workspace management
5. **Security**: User/kernel separation, permission enforcement, and secure boot

The modular design allows incremental implementation while maintaining system stability throughout the development process.