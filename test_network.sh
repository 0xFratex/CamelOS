#!/bin/bash
# Test network with QEMU - boots from disk image

echo "Booting Camel OS with network support..."
echo "(Timeout: 60 seconds)"

# Run QEMU with RTL8139 network card
timeout 60 qemu-system-i386 \
    -m 128M \
    -drive file=disk.img,format=raw,index=0,media=disk \
    -net nic,model=rtl8139 \
    -net user \
    -display none \
    -serial stdio \
    -no-reboot \
    2>&1 || echo "QEMU terminated"

echo ""
echo "Test complete. Check output above for network status."
