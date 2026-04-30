; installer/payload.asm - Embeds binaries into installer
; This file includes the compiled system.bin, mbr.bin and CDL libraries/apps as raw data
; Updated: Old empty placeholder .cdl apps replaced with real built CDL system libraries and apps

global system_bin_start
global system_bin_end
global mbr_bin_start
global mbr_bin_end
global app_math_start
global app_math_end
global app_usr32_start
global app_usr32_end
global app_syskernel_start
global app_syskernel_end
global app_proc_start
global app_proc_end
global app_timer_start
global app_timer_end
global app_gui_start
global app_gui_end
global app_sysmon_start
global app_sysmon_end
global app_jsengine_start
global app_jsengine_end
global app_netdiag_start
global app_netdiag_end
global startup_pcm_start
global startup_pcm_end

section .rodata

system_bin_start:
    incbin "system.bin"
system_bin_end:

mbr_bin_start:
    incbin "mbr.bin"
mbr_bin_end:

app_math_start:
    incbin "math.cdl"
app_math_end:

app_usr32_start:
    incbin "usr32.cdl"
app_usr32_end:

app_syskernel_start:
    incbin "syskernel.cdl"
app_syskernel_end:

app_proc_start:
    incbin "proc.cdl"
app_proc_end:

app_timer_start:
    incbin "timer.cdl"
app_timer_end:

app_gui_start:
    incbin "gui.cdl"
app_gui_end:

app_sysmon_start:
    incbin "sysmon.cdl"
app_sysmon_end:

app_jsengine_start:
    incbin "jsengine.cdl"
app_jsengine_end:

app_netdiag_start:
    incbin "netdiag.cdl"
app_netdiag_end:

startup_pcm_start:
    incbin "assets/system_sounds/startup.pcm"
startup_pcm_end:

section .note.GNU-stack noalloc noexec nowrite progbits
