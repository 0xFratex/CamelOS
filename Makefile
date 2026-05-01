#       Makefile        -       Camel   OS      Hybrid
CC      =       gcc
AS      =       nasm
LD      =       ld

#       Kernel  Flags
CFLAGS  =       -m32    -fno-stack-protector    -fno-builtin    -nostdinc       -O2     -Iinclude       -Icore  -Ihal/drivers   -Ihal/cpu       -Icommon        -Isys   -Ifs    -Iusr   -Ikernel        -fno-pic        -fno-pie        -mno-sse        -mno-mmx        -g      -Wall   -Wno-unused-parameter   -DKERNEL_MODE
CFLAGS_INSTALLER        =       -m32    -fno-stack-protector    -fno-builtin    -nostdinc       -O2     -Iinclude       -Icore  -Ihal/drivers   -Ihal/cpu       -Icommon        -Isys   -Ifs    -Iusr   -Ikernel        -fno-pic        -fno-pie        -mno-sse        -mno-mmx        -mno-80387      -msoft-float    -g      -Wall   -Wno-unused-parameter
LDFLAGS =       -m      elf_i386        -no-pie
KERNEL_LDFLAGS  =       $(LDFLAGS) -L/usr/lib/gcc/x86_64-linux-gnu/14/32
INSTALLER_LDFLAGS       =       $(LDFLAGS) -L/usr/lib/gcc/x86_64-linux-gnu/14/32

#       CDL     Flags   (Position       Independent     Code    for     Apps)
#       FIX:    Added   -mno-sse        -mno-mmx        -msoft-float    to      prevent #UD     (Int    6)      exceptions
#       caused  by      the     compiler        generating      SSE     instructions    when    the     kernel  hasn't  enabled them.
CDL_CFLAGS      =       -m32    -fno-stack-protector    -fno-builtin    -O2     \
	-Iinclude       -Icore  -Isys   -Iusr   -Ikernel        -I/usr/include  -fPIC   -g      -Wall   -Wno-unused-parameter   \
	-ffunction-sections     -fdata-sections \
	-march=i386     -mtune=i386     \
	-mno-sse        -mno-sse2       -mno-sse3       -mno-ssse3      -mno-sse4       -mno-sse4.1     -mno-sse4.2     \
	-mno-avx        -mno-avx2       -mno-mmx        -mno-3dnow      \
	-mno-80387      -mno-fp-ret-in-387      \
	-mgeneral-regs-only     \
	-fno-tree-loop-distribute-patterns      \
	-fno-strict-aliasing    \
	-ffreestanding  \
	-fno-asynchronous-unwind-tables \
	-fno-exceptions \
	-fno-unwind-tables      \
	-fomit-frame-pointer    \
	-minline-all-stringops  \
	-fno-tree-vectorize     \
	-fno-tree-loop-vectorize        \
	-fno-tree-slp-vectorize

#       CDL     Flags
#       -shared creates a       relocatable     ELF     (like   a       DLL)
#       -Bsymbolic      ensures internal        function        calls   bind    locally
CDL_LDFLAGS     =       -m      elf_i386        -shared -Bsymbolic      -e      cdl_main        -T      linker_cdl.ld   -L/usr/lib/gcc/x86_64-linux-gnu/14/32   

COMMON_SRC      =       common/font.c

#       ---     SOURCES ---
HAL_SRC =       hal/drivers/vga.c       hal/drivers/ata.c       hal/drivers/serial.c    \
	hal/drivers/keyboard.c  hal/drivers/mouse.c     hal/drivers/sound.c     \
	hal/drivers/pci.c       hal/drivers/net_rtl8139.c       hal/drivers/net_rtl8169.c       hal/drivers/net.c       \
	hal/drivers/net_e1000.c hal/drivers/ahci.c      \
	hal/drivers/usb_xhci.c  hal/drivers/usb.c       hal/drivers/wifi_rtl.c  \
	hal/drivers/rtc.c       hal/drivers/sb16.c      \
	hal/cpu/apic.c  hal/cpu/idt.c   hal/cpu/isr.c   hal/cpu/gdt.c   hal/cpu/timer.c hal/cpu/paging.c        hal/cpu/syscall.c       \
	hal/video/gfx_hal.c     hal/video/compositor.c  hal/video/animation.c   hal/video/loading_animation.c
	
CORE_SRC        =       core/kernel.c   core/panic.c    sys/api.c       core/string.c   core/memory.c   core/task.c     core/cdl_loader.c       core/window_server.c    core/net.c      core/net_if.c   core/net_dhcp.c core/socket.c   core/tcp.c      core/http.c     core/tls.c      core/tls13.c    core/http2.c    core/tls_ca_store.c     core/app_switcher.c     core/dns.c      core/debug.c    core/arp.c      core/scheduler.c        core/firewall.c  core/sha256.c   core/objc_runtime.c    core/macho_loader.c    core/foundation_stub.c    core/app_bundle.c    core/dmg_mount.c      core/zlib_inflate.c    core/app_installer.c
ASSETS_SRC      =       kernel/assets.c
FS_SRC  =       fs/pfs32.c      fs/disk.c
USR_SRC =       usr/shell.c     usr/bubbleview.c        usr/desktop.c   usr/framework.c usr/dock.c      usr/clipboard.c usr/screenlock.c        usr/welcome_setup.c     usr/lib/camel_framework.c       usr/lib/camel_ui.c      usr/apps/files.c        usr/apps/terminal.c

#       NOTE:   files.c and terminal.c are compiled into the kernel as built-in apps
#       They are launched via the kernel_launch_builtin_app() dispatch mechanism
KERNEL_OBJ      =       system/entry.o  $(HAL_SRC:.c=.o)        $(CORE_SRC:.c=.o)       $(FS_SRC:.c=.o) $(USR_SRC:.c=.o)        $(ASSETS_SRC:.c=.o)     $(COMMON_SRC:.c=.o)

#       Installer       objects -       explicitly      list    them    to      avoid   dependency      issues
INSTALLER_OBJ   =       installer/entry.o       installer/installer_main.o      installer/panic_framework.o     sys/api_installer.o     core/string.o   core/memory.o   core/task.o     core/scheduler.o        core/panic.o    hal/drivers/ata.o       hal/drivers/vga.o       hal/video/gfx_hal.o     hal/drivers/serial.o    hal/cpu/apic.o  hal/cpu/timer.o hal/cpu/paging.o        fs/pfs32.o      fs/disk.o       hal/drivers/keyboard.o  hal/drivers/mouse.o     hal/drivers/rtc.o       installer/payload.o     common/font.o   kernel/assets.o installer/arp_stub.o    installer/disk_tools.o  installer/sys_requirements.o    installer/disk_health.o   installer/soft_div.o

#       ---     QEMU    AUDIO   CONFIG  ---
#       Try     SDL     first,  it      usually works   best    out     of      the     box
QEMU_AUDIO      =       -audiodev       sdl,id=snd0     -machine        pcspk-audiodev=snd0     -device sb16,audiodev=snd0

#       If      you     get     an      error   about   SDL,    revert  to:
#       QEMU_AUDIO      =       -audiodev       pa,id=snd0      -machine        pcspk-audiodev=snd0     -device sb16,audiodev=snd0

#       ---     BUILD   RULES   ---

all:    disk.img        camel_install.iso

mbr.bin:        boot/mbr.asm
	$(AS)   -f      bin     $<      -o      $@

system/entry.o: boot/system_entry.asm
	mkdir   -p      system
	$(AS)   -f      elf32   $<      -o      $@

system.bin:     $(KERNEL_OBJ)
	$(LD)   $(KERNEL_LDFLAGS)       -T      linker_system.ld        -o      system.elf      $(KERNEL_OBJ)   -L/usr/lib/gcc/x86_64-linux-gnu/14/32   
	objcopy -O      binary  system.elf      system.bin

#       ---     APP     COMPILATION     (Hybrid: .cdl legacy + .dmg/.app macOS)       ---
#       Legacy .cdl apps use ELF shared objects (CDL format)
#       New macOS apps use .app bundles with Info.plist
#       .dmg disk images can be mounted and apps extracted via drag-to-Applications

#       ---     LEGACY CDL LIBRARIES (still needed for system libs)     ---

#       Math    Lib
math.cdl:       usr/lib/math.c
	$(CC)   $(CDL_CFLAGS)   -c      usr/lib/math.c  -o      math.o
	$(LD)   $(CDL_LDFLAGS)  -o      math.cdl        math.o

usr32.cdl:      usr/lib/usr32.c
	$(CC)   $(CDL_CFLAGS)   -c      usr/lib/usr32.c -o      usr32.o
	$(LD)   $(CDL_LDFLAGS)  -o      usr32.cdl       usr32.o

syskernel.cdl:  usr/lib/syskernel.c
	$(CC)   $(CDL_CFLAGS)   -c      usr/lib/syskernel.c     -o      syskernel.o
	$(LD)   $(CDL_LDFLAGS)  -o      syskernel.cdl   syskernel.o

proc.cdl:       usr/lib/proc.c
	$(CC)   $(CDL_CFLAGS)   -c      usr/lib/proc.c  -o      proc.o
	$(LD)   $(CDL_LDFLAGS)  -o      proc.cdl        proc.o

timer.cdl:      usr/lib/timer.c
	$(CC)   $(CDL_CFLAGS)   -c      usr/lib/timer.c -o      timer.o
	$(LD)   $(CDL_LDFLAGS)  -o      timer.cdl       timer.o

gui.cdl:        usr/lib/gui.c
	$(CC)   $(CDL_CFLAGS)   -c      usr/lib/gui.c   -o      gui.o
	$(LD)   $(CDL_LDFLAGS)  -o      gui.cdl gui.o

sysmon.cdl:     usr/lib/sysmon.c
	$(CC)   $(CDL_CFLAGS)   -c      usr/lib/sysmon.c        -o      sysmon.o
	$(LD)   $(CDL_LDFLAGS)  -o      sysmon.cdl      sysmon.o

jsengine.cdl:   usr/libs/js_engine.c
	$(CC)   $(CDL_CFLAGS)   -c      usr/libs/js_engine.c    -o      jsengine.o
	$(LD)   $(CDL_LDFLAGS)  -o      jsengine.cdl    jsengine.o

#       ---     HYBRID APP TARGETS (legacy .cdl apps, now loaded via .app bundles)     ---
#       These create .cdl binaries that the .app bundle system can discover
#       The .app wrapper is created at runtime by the installer

#       Network Diagnostic App
netdiag.cdl:    usr/apps/netdiag.c      usr/lib/camel_framework.c
	$(CC)   $(CDL_CFLAGS)   -c      usr/apps/netdiag.c      -o      netdiag.o
	$(CC)   $(CDL_CFLAGS)   -c      usr/lib/camel_framework.c       -o      camel_framework.o
	$(LD)   $(CDL_LDFLAGS)  -o      netdiag.cdl     netdiag.o       camel_framework.o

#       ---     INSTALLER       ---

installer/entry.o:      boot/multiboot.asm
	$(AS)   -f      elf32   $<      -o      $@

installer/panic_framework.o:    installer/panic_framework.c
	$(CC)   $(CFLAGS_INSTALLER)     -c      $<      -o      $@

#       Add     ALL     CDL     files   to      payload (hybrid: legacy libs + hybrid apps)
installer/payload.o:    installer/payload.asm   system.bin      mbr.bin math.cdl        usr32.cdl       syskernel.cdl   proc.cdl        timer.cdl       gui.cdl sysmon.cdl      jsengine.cdl    netdiag.cdl
	$(AS)   -f      elf32   $<      -o      $@

installer.elf:  $(INSTALLER_OBJ)        math.cdl        usr32.cdl       syskernel.cdl   proc.cdl        timer.cdl       gui.cdl sysmon.cdl      jsengine.cdl    netdiag.cdl
	$(LD)   $(LDFLAGS)      -T      linker_installer.ld     -o      installer.elf   $(INSTALLER_OBJ)        -L/usr/lib/gcc/x86_64-linux-gnu/14/32

camel_install.iso:      installer.elf
	mkdir   -p      iso/boot/grub
	cp      installer.elf   iso/boot/installer.elf
	printf  'set    timeout=0\nmenuentry    "Camel  OS      Installer"      {\n     multiboot       /boot/installer.elf\n   boot\n}'        >       iso/boot/grub/grub.cfg
	grub-mkrescue   -o      camel_install.iso       iso
	rm      -rf     iso

disk.img:
	dd      if=/dev/zero    of=disk.img     bs=1M   count=256

#       ---     COMMANDS        ---

clean:
	rm      -f      *.bin   *.elf   *.o     *.iso   *.cdl   disk.img
	find    .       -name   "*.o"   -type   f       -delete

#       Add     this    target  or      run     it      manually
rebuild:        clean   all

install:        camel_install.iso       disk.img
	qemu-system-i386        -m      512     -cdrom  camel_install.iso       -drive  file=disk.img,format=raw,index=0,media=disk     -boot   d       -serial stdio   $(QEMU_AUDIO)

#       Enhanced        QEMU    networking
QEMU_NET        =       -netdev user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dhcpstart=10.0.2.15  \
	-device rtl8139,netdev=net0,mac=52:54:00:12:34:56

#       Enable  GDB     stub    for     debugging
QEMU_DEBUG      =       -s      -S

#       Simple  networking      for     testing
QEMU_NET_SIMPLE =       -net    nic,model=rtl8139       -net    user

run:    disk.img
	qemu-system-i386        -m      512     -drive  file=disk.img,format=raw,index=0,media=disk     \
	-vga    std     -serial stdio   $(QEMU_NET_SIMPLE)      $(QEMU_AUDIO)

#       Explicit        compilation     rules   to      handle  different       flags
sys/api.o:      sys/api.c
	$(CC)   $(CFLAGS)       -c      $<      -o      $@

sys/api_installer.o:    sys/api.c
	$(CC)   $(CFLAGS_INSTALLER)     -c      $<      -o      $@

installer/%.o:  installer/%.c
	$(CC)   $(CFLAGS_INSTALLER)     -c      $<      -o      $@

#       For     all     other   .c      files,  use     kernel  flags   by      default
%.o:    %.c
	$(CC)   $(CFLAGS)       -c      $<      -o      $@

#       Socket  compilation     rule
core/socket.o:  core/socket.c
	$(CC)   $(CFLAGS)       -c      $<      -o      $@

#       RTL8169 driver  compilation     rule
hal/drivers/net_rtl8169.o:      hal/drivers/net_rtl8169.c
	$(CC)   $(CFLAGS)       -c      $<      -o      $@

#       E1000   driver  compilation     rule
hal/drivers/net_e1000.o:        hal/drivers/net_e1000.c
	$(CC)   $(CFLAGS)       -c      $<      -o      $@

#       AHCI    driver  compilation     rule
hal/drivers/ahci.o:     hal/drivers/ahci.c
	$(CC)   $(CFLAGS)       -c      $<      -o      $@

#       TLS     CA      store   compilation     rule
core/tls_ca_store.o:    core/tls_ca_store.c
	$(CC)   $(CFLAGS)       -c      $<      -o      $@
