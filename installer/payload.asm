; installer/payload.asm - Embeds binaries into installer
; This file includes the compiled system.bin, mbr.bin and .cdl apps as raw data

global system_bin_start
global system_bin_end
global mbr_bin_start
global mbr_bin_end
global app_terminal_start
global app_terminal_end
global app_files_start
global app_files_end
global lib_math_start
global lib_math_end
global lib_usr32_start
global lib_usr32_end
global lib_syskernel_start
global lib_syskernel_end
global lib_proc_start
global lib_proc_end
global lib_timer_start
global lib_timer_end
global lib_gui_start
global lib_gui_end
global lib_sysmon_start
global lib_sysmon_end
global app_waterhole_start
global app_waterhole_end
global app_nettools_start
global app_nettools_end
global app_textedit_start
global app_textedit_end
global app_browser_start
global app_browser_end
global startup_pcm_start
global startup_pcm_end

section .rodata

system_bin_start:
    incbin "system.bin"
system_bin_end:

mbr_bin_start:
    incbin "mbr.bin"
mbr_bin_end:

app_terminal_start:
    incbin "terminal.cdl"
app_terminal_end:

app_files_start:
    incbin "files.cdl"
app_files_end:

lib_math_start:
    incbin "math.cdl"
lib_math_end:

lib_usr32_start:
    incbin "usr32.cdl"
lib_usr32_end:

lib_syskernel_start:
    incbin "syskernel.cdl"
lib_syskernel_end:

lib_proc_start:
    incbin "proc.cdl"
lib_proc_end:

lib_timer_start:
    incbin "timer.cdl"
lib_timer_end:

lib_gui_start:
    incbin "gui.cdl"
lib_gui_end:

lib_sysmon_start:
    incbin "sysmon.cdl"
lib_sysmon_end:

app_waterhole_start:
    incbin "waterhole.cdl"
app_waterhole_end:

app_nettools_start:
    incbin "nettools.cdl"
app_nettools_end:

app_textedit_start:
    incbin "textedit.cdl"
app_textedit_end:

app_browser_start:
    incbin "browser.cdl"
app_browser_end:

startup_pcm_start:
    incbin "assets/system_sounds/startup.pcm"
startup_pcm_end:

section .note.GNU-stack noalloc noexec nowrite progbits
