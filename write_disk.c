// Simple tool to write MBR + kernel to disk image for testing
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main(int argc, char* argv[]) {
    if(argc != 4) {
        printf("Usage: %s <mbr.bin> <system.bin> <disk.img>\n", argv[0]);
        return 1;
    }
    
    const char* mbr_file = argv[1];
    const char* kernel_file = argv[2];
    const char* disk_file = argv[3];
    
    // Read MBR (512 bytes)
    FILE* f = fopen(mbr_file, "rb");
    if(!f) { perror("fopen mbr"); return 1; }
    uint8_t mbr[512];
    if(fread(mbr, 1, 512, f) != 512) {
        printf("MBR must be exactly 512 bytes\n");
        return 1;
    }
    fclose(f);
    
    // Read kernel
    f = fopen(kernel_file, "rb");
    if(!f) { perror("fopen kernel"); return 1; }
    fseek(f, 0, SEEK_END);
    long kernel_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* kernel = malloc(kernel_size);
    fread(kernel, 1, kernel_size, f);
    fclose(f);
    
    // Open disk
    f = fopen(disk_file, "r+b");
    if(!f) { perror("fopen disk"); return 1; }
    
    // Write MBR to sector 0
    fwrite(mbr, 1, 512, f);
    
    // Write kernel starting at sector 1
    fseek(f, 512, SEEK_SET);
    fwrite(kernel, 1, kernel_size, f);
    
    fclose(f);
    free(kernel);
    
    printf("Wrote MBR (%d bytes) and kernel (%ld bytes) to %s\n", 512, kernel_size, disk_file);
    return 0;
}
