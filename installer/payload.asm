; installer/payload.asm - Embeds binaries into installer
; This file includes the compiled system.bin, mbr.bin and app bundles as raw data
; Note: Old .cdl app files are deprecated placeholders (0 bytes).
; They will be replaced with macOS-style .app bundles in the future.

global system_bin_start
global system_bin_end
global mbr_bin_start
global mbr_bin_end
global app_terminal_start
global app_terminal_end
global app_files_start
global app_files_end
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
