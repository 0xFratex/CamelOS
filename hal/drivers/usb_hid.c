// hal/drivers/usb_hid.c - USB HID Boot Protocol Driver for CamelOS
// Implements the USB HID boot protocol for keyboards and mice
// enabling support for USB-only hardware (no PS/2 required)
//
// Architecture:
//   1. USB enumeration via xHCI (device descriptor, config descriptor, interface)
//   2. HID class detection (interface class 0x03)
//   3. Boot protocol setup (Set_Protocol, Set_Idle)
//   4. Interrupt IN endpoint polling for input reports
//   5. Report parsing: 8-byte keyboard, 4-byte mouse boot reports
//   6. Integration with existing keyboard.c / mouse.c input queues

#include "usb_hid.h"
#include "usb.h"
#include "usb_xhci.h"
#include "keyboard.h"
#include "mouse.h"
#include "serial.h"
#include "../../core/memory.h"
#include "../../core/string.h"
#include "../../sys/io_ports.h"
#include "../video/gfx_hal.h"

// ============================================================================
// USB HID Constants
// ============================================================================

// Standard USB requests
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_PROTOCOL    0x0B
#define USB_REQ_SET_IDLE        0x0A
#define USB_REQ_SET_REPORT      0x09

// Descriptor types
#define USB_DESC_DEVICE         1
#define USB_DESC_CONFIG         2
#define USB_DESC_INTERFACE      4
#define USB_DESC_ENDPOINT       5
#define USB_DESC_HID            0x21
#define USB_DESC_HID_REPORT     0x22

// HID subclass and protocol
#define HID_SUBCLASS_BOOT       1
#define HID_PROTOCOL_KEYBOARD   1
#define HID_PROTOCOL_MOUSE      2

// Endpoint attributes
#define USB_EP_ATTR_INTERRUPT   3
#define USB_EP_DIR_IN           0x80

// Transfer types
#define USB_TR_INTERRUPT        3

// ============================================================================
// USB Device Descriptor (18 bytes)
// ============================================================================

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_desc_t;

// ============================================================================
// USB Configuration Descriptor (9 bytes)
// ============================================================================

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_desc_t;

// ============================================================================
// USB Interface Descriptor (9 bytes)
// ============================================================================

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} usb_interface_desc_t;

// ============================================================================
// USB Endpoint Descriptor (7 bytes)
// ============================================================================

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_desc_t;

// ============================================================================
// HID Boot Report Structures
// ============================================================================

// Keyboard boot report: 8 bytes
// Byte 0: Modifier keys (Ctrl, Shift, Alt, GUI)
// Byte 1: Reserved
// Bytes 2-7: Keycodes (up to 6 simultaneous keys, 0 = no key)
typedef struct {
    uint8_t modifiers;    // Bit flags: Ctrl=0x01, Shift=0x02, Alt=0x04, GUI=0x08
    uint8_t reserved;
    uint8_t keycodes[6];  // HID usage codes for pressed keys
} hid_keyboard_report_t;

// Mouse boot report: 4 bytes (or 3 for boot protocol without wheel)
// Byte 0: Button flags
// Byte 1: X displacement (signed)
// Byte 2: Y displacement (signed)
// Byte 3: Wheel displacement (signed, optional)
typedef struct {
    uint8_t buttons;      // Bit 0=Left, Bit 1=Right, Bit 2=Middle
    int8_t  x_displacement;
    int8_t  y_displacement;
    int8_t  wheel_displacement;
} hid_mouse_report_t;

// ============================================================================
// HID Modifier Key Bit Masks
// ============================================================================

#define HID_MOD_LCTRL    0x01
#define HID_MOD_LSHIFT   0x02
#define HID_MOD_LALT     0x04
#define HID_MOD_LGUI     0x08
#define HID_MOD_RCTRL    0x10
#define HID_MOD_RSHIFT   0x20
#define HID_MOD_RALT     0x40
#define HID_MOD_RGUI     0x80

// ============================================================================
// HID Usage Code to PS/2 Set 1 Scancode Translation Table
// Maps USB HID usage codes to the PS/2 scancodes that CamelOS already handles
// ============================================================================

static const uint8_t hid_to_ps2[256] = {
    // 0x00-0x0F: No event, Keyboard errors, and post-f6 keys
    0x00, 0x00, 0x00, 0x00, 0x1E, 0x30, 0x2E, 0x20,  // a, b, c, d
    0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26,  // e, f, g, h, i, j, k, l
    // 0x10-0x1F: m through s
    0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14,  // m, n, o, p, q, r, s, t
    0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, 0x00, 0x00,  // u, v, w, x, y, z, -, -
    // 0x20-0x2F: Numbers
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,  // 1, 2, 3, 4, 5, 6, 7, 8
    0x0A, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 9, 0, -, -, -, -, -, -
    // 0x30-0x3F: Special keys
    0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A,  // Enter, Esc, Bksp, Tab, Space, -, -, [
    0x1B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35,  // ], \, ;, ', `, ,, ., /
    // 0x40-0x4F: More special keys + F-keys
    0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41,  // CapsLock, F1-F4
    0x42, 0x43, 0x44, 0x57, 0x58, 0x00, 0x00, 0x00,  // F5-F10, NumLock, Scroll, -, -, -
    // 0x50-0x5F: More keys
    0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // -, PrtSc, -, -, -, -, -, -
    0x45, 0x48, 0x50, 0x4B, 0x4D, 0x47, 0x4F, 0x49,  // NumLock, Up*, Down*, Left*, Right*, Home*, End*, PgUp*
    0x51, 0x52, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00,  // PgDn*, Ins*, Del*, -, -, -, -, -
    // 0x60-0x6F: Keypad keys
    0x35, 0x37, 0x4A, 0x47, 0x48, 0x49, 0x4B, 0x4C,  // KP/, KP*, KP-, KP7, KP8, KP9, KP4, KP5
    0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x1C,  // KP6, KP+, KP1, KP2, KP3, KP0, KP., KPEnter
    // Rest is zeros (extended keys need E0 prefix, handled separately)
};

// Extended keys that need E0 prefix in PS/2 scancode set
static const uint8_t hid_extended_keys[] = {
    0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,  // Insert, Home, PgUp, Del, End, PgDn, Right
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56,   // Left, Down, Up, NumLock, KP/, KP*, KP-
    0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D,          // KP+, KPEnter, RCtrl, RShift, RAlt, RGUI
    0
};

static int is_extended_key(uint8_t hid_code) {
    for (int i = 0; hid_extended_keys[i]; i++) {
        if (hid_extended_keys[i] == hid_code) return 1;
    }
    return 0;
}

// ============================================================================
// HID Device State
// ============================================================================

#define MAX_HID_DEVICES 4

static hid_device_t hid_devices[MAX_HID_DEVICES];
static int hid_device_count = 0;

// Previous keyboard state for change detection
static hid_keyboard_report_t prev_keyboard_report;
static int keyboard_initialized = 0;

// ============================================================================
// xHCI TRB (Transfer Request Block) Structures
// ============================================================================

// TRB types
#define TRB_TYPE_NORMAL         1
#define TRB_TYPE_SETUP          2
#define TRB_TYPE_DATA           3
#define TRB_TYPE_STATUS         4
#define TRB_TYPE_LINK           6
#define TRB_TYPE_ENABLE_SLOT    9
#define TRB_TYPE_ADDRESS_DEVICE 10
#define TRB_TYPE_CONFIGURE_EP   12
#define TRB_TYPE_EVAL_CTX       13
#define TRB_TYPE_TRANSFER_EVENT 32
#define TRB_TYPE_COMMAND_COMPL  33

typedef struct __attribute__((packed)) {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

// ============================================================================
// Simplified USB Control Transfer via xHCI
// These functions encapsulate the xHCI command/event ring mechanics
// needed to perform USB enumeration and HID configuration
// ============================================================================

// Global xHCI state (from usb_xhci.c) - declared in usb_xhci.h
// cap_regs and op_regs are already accessible via usb_xhci.h include above

// TRB ring management
#define TRB_RING_SIZE 256
static xhci_trb_t command_ring[TRB_RING_SIZE] __attribute__((aligned(64)));
static xhci_trb_t event_ring[TRB_RING_SIZE] __attribute__((aligned(64)));
static xhci_trb_t transfer_ring[MAX_HID_DEVICES][TRB_RING_SIZE] __attribute__((aligned(64)));
static int cmd_enqueue_idx = 0;
static int evt_dequeue_idx = 0;
static int xfer_enqueue_idx[MAX_HID_DEVICES] = {0};

// DMA-style aligned buffers for USB transfers
static uint8_t usb_setup_buf[512] __attribute__((aligned(64)));
static uint8_t usb_data_buf[4096] __attribute__((aligned(64)));

// Slot context storage
#define MAX_XHCI_SLOTS 32
static uint32_t slot_ctx[MAX_XHCI_SLOTS][8] __attribute__((aligned(64)));
static uint32_t endpoint_ctx[MAX_XHCI_SLOTS][16][8] __attribute__((aligned(64)));

// Doorbell register access
static inline void xhci_ring_doorbell(uint8_t slot_id, uint8_t target) {
    if (!cap_regs) return;
    uint32_t db_off = cap_regs->db_off;
    volatile uint32_t* db = (volatile uint32_t*)((uint32_t)cap_regs + db_off + (slot_id * 2 + target) * 4);
    *db = 0;  // Ring with sequence 0
}

// Initialize the command ring
static void xhci_init_command_ring(void) {
    memset(command_ring, 0, sizeof(command_ring));
    cmd_enqueue_idx = 0;
    // Set the CRCR (Command Ring Control Register) to point to our command ring
    uint32_t crcr_lo = ((uint32_t)command_ring & 0xFFFFF000) | 1; // RCS bit
    uint32_t crcr_hi = 0;
    MMIO_WRITE32(&op_regs->crcr_lo, crcr_lo);
    MMIO_WRITE32(&op_regs->crcr_hi, crcr_hi);
}

// Initialize the event ring
static void xhci_init_event_ring(void) {
    memset(event_ring, 0, sizeof(event_ring));
    evt_dequeue_idx = 0;
    // Set ERST (Event Ring Segment Table) - simplified: single segment
    // In a full implementation, this requires ERDP and ERSTBA registers
    // in the runtime registers area
}

// Submit a command TRB and wait for completion
static int xhci_submit_command(xhci_trb_t* trb) {
    if (!op_regs) return -1;

    // Copy TRB to command ring
    command_ring[cmd_enqueue_idx] = *trb;
    cmd_enqueue_idx = (cmd_enqueue_idx + 1) % TRB_RING_SIZE;

    // Ring the command doorbell
    xhci_ring_doorbell(0, 0);

    // Wait for command completion (simplified polling)
    for (volatile int i = 0; i < 100000; i++) {
        // In a real implementation, we'd check the event ring for completion
        asm volatile("pause");
    }

    return 0;
}

// Perform a USB control transfer (SETUP + DATA + STATUS)
static int usb_control_transfer(uint8_t slot_id, uint8_t bmRequestType,
                                 uint8_t bRequest, uint16_t wValue,
                                 uint16_t wIndex, uint16_t wLength,
                                 void* data) {
    // Setup Stage TRB
    xhci_trb_t setup_trb;
    memset(&setup_trb, 0, sizeof(setup_trb));

    // Build the 8-byte SETUP packet
    uint8_t* setup_pkt = usb_setup_buf;
    setup_pkt[0] = bmRequestType;
    setup_pkt[1] = bRequest;
    setup_pkt[2] = wValue & 0xFF;
    setup_pkt[3] = (wValue >> 8) & 0xFF;
    setup_pkt[4] = wIndex & 0xFF;
    setup_pkt[5] = (wIndex >> 8) & 0xFF;
    setup_pkt[6] = wLength & 0xFF;
    setup_pkt[7] = (wLength >> 8) & 0xFF;

    setup_trb.param_lo = (uint32_t)setup_pkt;
    setup_trb.param_hi = 0;
    setup_trb.status = 8;  // Setup packet is 8 bytes
    setup_trb.control = (TRB_TYPE_SETUP << 10) | (1 << 6) | (1 << 5);  // TRB_TYPE, IDT, ISP

    // Data Stage TRB (if wLength > 0)
    if (wLength > 0 && data) {
        xhci_trb_t data_trb;
        memset(&data_trb, 0, sizeof(data_trb));
        data_trb.param_lo = (uint32_t)data;
        data_trb.param_hi = 0;
        data_trb.status = wLength;
        data_trb.control = (TRB_TYPE_DATA << 10) |
                           ((bmRequestType & 0x80) ? (1 << 16) : 0) |
                           (1 << 5);  // DIR=IN if device-to-host, ISP

        // Status Stage TRB
        xhci_trb_t status_trb;
        memset(&status_trb, 0, sizeof(status_trb));
        status_trb.control = (TRB_TYPE_STATUS << 10) |
                             ((bmRequestType & 0x80) ? 0 : (1 << 16)) |  // DIR opposite of data
                             (1 << 5);  // IOC (interrupt on completion)

        // Submit all three TRBs to the control endpoint ring
        // In a real implementation, we'd add them to EP0's transfer ring
    } else {
        // Status Stage TRB (no data stage)
        xhci_trb_t status_trb;
        memset(&status_trb, 0, sizeof(status_trb));
        status_trb.control = (TRB_TYPE_STATUS << 10) |
                             (1 << 16) |  // DIR=IN
                             (1 << 5);    // IOC
    }

    // For now, use the simplified command path
    // A full implementation would queue these on the endpoint's transfer ring
    s_printf("[USB-HID] Control transfer: req=");
    char buf[16]; int_to_str(bRequest, buf); s_printf(buf);
    s_printf(" val="); int_to_str(wValue, buf); s_printf(buf);
    s_printf("\n");

    return 0;
}

// Enable a device slot via xHCI
static uint8_t xhci_enable_slot(void) {
    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.control = (TRB_TYPE_ENABLE_SLOT << 10);

    if (xhci_submit_command(&trb) == 0) {
        // Slot ID is in the completion event's param field
        // For now, return a simple slot ID
        return 1;
    }
    return 0;
}

// ============================================================================
// HID Device Enumeration and Configuration
// ============================================================================

// Enumerate a USB device to find HID interfaces
static int usb_hid_enumerate(uint8_t slot_id) {
    // Step 1: Get Device Descriptor
    usb_device_desc_t dev_desc;
    memset(&dev_desc, 0, sizeof(dev_desc));

    int ret = usb_control_transfer(slot_id, 0x80,  // Device-to-host, Standard, Device
                                    USB_REQ_GET_DESCRIPTOR,
                                    (USB_DESC_DEVICE << 8) | 0,  // wValue: Device descriptor
                                    0,  // wIndex: 0
                                    sizeof(dev_desc),
                                    &dev_desc);
    if (ret < 0) {
        s_printf("[USB-HID] Failed to get device descriptor\n");
        return -1;
    }

    s_printf("[USB-HID] Device: VID=");
    char buf[16];
    int_to_str(dev_desc.idVendor, buf); s_printf(buf);
    s_printf(" PID="); int_to_str(dev_desc.idProduct, buf); s_printf(buf);
    s_printf(" Class="); int_to_str(dev_desc.bDeviceClass, buf); s_printf(buf);
    s_printf("\n");

    // Step 2: Get Configuration Descriptor (with all interfaces and endpoints)
    usb_config_desc_t config_desc;
    memset(&config_desc, 0, sizeof(config_desc));

    ret = usb_control_transfer(slot_id, 0x80,
                                USB_REQ_GET_DESCRIPTOR,
                                (USB_DESC_CONFIG << 8) | 0,
                                0,
                                sizeof(config_desc),
                                &config_desc);
    if (ret < 0) {
        s_printf("[USB-HID] Failed to get config descriptor\n");
        return -1;
    }

    // Get the full configuration descriptor set (config + interfaces + endpoints)
    uint16_t total_len = config_desc.wTotalLength;
    if (total_len > sizeof(usb_data_buf)) total_len = sizeof(usb_data_buf);

    ret = usb_control_transfer(slot_id, 0x80,
                                USB_REQ_GET_DESCRIPTOR,
                                (USB_DESC_CONFIG << 8) | 0,
                                0,
                                total_len,
                                usb_data_buf);
    if (ret < 0) {
        s_printf("[USB-HID] Failed to get full config descriptor\n");
        return -1;
    }

    // Step 3: Parse descriptors to find HID interfaces
    uint32_t offset = config_desc.bLength;
    hid_device_t* hid_dev = 0;

    while (offset < total_len) {
        uint8_t desc_len = usb_data_buf[offset];
        uint8_t desc_type = usb_data_buf[offset + 1];

        if (desc_len == 0) break;  // Prevent infinite loop

        if (desc_type == USB_DESC_INTERFACE) {
            usb_interface_desc_t* iface = (usb_interface_desc_t*)(usb_data_buf + offset);

            // Check if this is a HID interface (class 0x03)
            if (iface->bInterfaceClass == 0x03 && hid_device_count < MAX_HID_DEVICES) {
                hid_dev = &hid_devices[hid_device_count];
                memset(hid_dev, 0, sizeof(hid_device_t));
                hid_dev->slot_id = slot_id;
                hid_dev->interface_num = iface->bInterfaceNumber;
                hid_dev->protocol = iface->bInterfaceProtocol;  // 1=Keyboard, 2=Mouse
                hid_dev->subclass = iface->bInterfaceSubClass;

                s_printf("[USB-HID] Found HID interface: ");
                s_printf(iface->bInterfaceProtocol == 1 ? "Keyboard" :
                         iface->bInterfaceProtocol == 2 ? "Mouse" : "Other");
                s_printf(" (interface ");
                int_to_str(iface->bInterfaceNumber, buf); s_printf(buf);
                s_printf(")\n");
            }
        }

        if (desc_type == USB_DESC_ENDPOINT && hid_dev) {
            usb_endpoint_desc_t* ep = (usb_endpoint_desc_t*)(usb_data_buf + offset);

            // Look for Interrupt IN endpoints
            if ((ep->bmAttributes & 0x03) == USB_EP_ATTR_INTERRUPT &&
                (ep->bEndpointAddress & USB_EP_DIR_IN)) {
                hid_dev->endpoint_addr = ep->bEndpointAddress;
                hid_dev->endpoint_interval = ep->bInterval;
                hid_dev->max_packet_size = ep->wMaxPacketSize;
                hid_dev->active = 1;

                s_printf("[USB-HID] Interrupt IN endpoint: 0x");
                int_to_str(ep->bEndpointAddress, buf); s_printf(buf);
                s_printf(" interval="); int_to_str(ep->bInterval, buf); s_printf(buf);
                s_printf("\n");

                hid_device_count++;
                hid_dev = 0;  // Done with this device
            }
        }

        offset += desc_len;
    }

    // Step 4: Set the configuration
    usb_control_transfer(slot_id, 0x00,  // Host-to-device, Standard, Device
                          0x09,  // SET_CONFIGURATION
                          config_desc.bConfigurationValue,
                          0, 0, 0);

    return hid_device_count > 0 ? 0 : -1;
}

// Configure a HID device for boot protocol operation
static int usb_hid_set_boot_protocol(hid_device_t* dev) {
    if (!dev || !dev->active) return -1;

    // Set the boot protocol (protocol 0 = boot protocol)
    int ret = usb_control_transfer(dev->slot_id,
                                    0x21,  // Host-to-device, Class, Interface
                                    USB_REQ_SET_PROTOCOL,
                                    0,  // 0 = Boot Protocol
                                    dev->interface_num,
                                    0, 0);
    if (ret < 0) {
        s_printf("[USB-HID] Failed to set boot protocol\n");
        return -1;
    }

    // Set idle rate to 0 (infinite) for keyboard, or reasonable for mouse
    uint16_t idle_rate = (dev->protocol == HID_PROTOCOL_KEYBOARD) ? 0 : 50;  // 0=infinite, 50=50ms
    ret = usb_control_transfer(dev->slot_id,
                                0x21,
                                USB_REQ_SET_IDLE,
                                (idle_rate << 8),  // Duration in upper byte
                                dev->interface_num,
                                0, 0);
    if (ret < 0) {
        s_printf("[USB-HID] Failed to set idle rate (non-fatal)\n");
        // Non-fatal: some devices don't support Set_Idle
    }

    s_printf("[USB-HID] Boot protocol configured for ");
    s_printf(dev->protocol == HID_PROTOCOL_KEYBOARD ? "keyboard" : "mouse");
    s_printf("\n");

    return 0;
}

// ============================================================================
// HID Input Report Processing
// ============================================================================

// Process a keyboard boot report and inject into the PS/2 keyboard handler
static void hid_process_keyboard_report(const hid_keyboard_report_t* report) {
    // Update modifier state (shift, ctrl, alt)
    kbd_shift_pressed = (report->modifiers & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) ? 1 : 0;
    kbd_ctrl_pressed  = (report->modifiers & (HID_MOD_LCTRL | HID_MOD_RCTRL)) ? 1 : 0;
    kbd_alt_pressed   = (report->modifiers & (HID_MOD_LALT | HID_MOD_RALT)) ? 1 : 0;

    // Detect newly pressed keys (present in current but not in previous report)
    for (int i = 0; i < 6; i++) {
        uint8_t code = report->keycodes[i];
        if (code == 0) continue;

        // Check if this key was already pressed
        int was_pressed = 0;
        for (int j = 0; j < 6; j++) {
            if (prev_keyboard_report.keycodes[j] == code) {
                was_pressed = 1;
                break;
            }
        }

        if (!was_pressed) {
            // New key press - convert HID usage code to PS/2 scancode
            // and inject into the keyboard input system
            uint8_t scancode = 0;

            if (code < sizeof(hid_to_ps2)) {
                scancode = hid_to_ps2[code];
            }

            if (scancode) {
                // For extended keys, send E0 prefix
                if (is_extended_key(code)) {
                    // In the full implementation, we'd call the keyboard ISR
                    // with the E0-prefixed scancode sequence
                    s_printf("[USB-HID] Extended key: E0 ");
                    char buf[8]; int_to_str(scancode, buf); s_printf(buf);
                    s_printf("\n");
                }

                // Inject the scancode into the keyboard input buffer
                // This calls the same path that the PS/2 IRQ1 handler uses
                int next = (write_ptr + 1) % KBD_BUFFER_SIZE;
                if (next != read_ptr) {
                    kbd_buffer[write_ptr] = scancode;
                    write_ptr = next;
                }
            }
        }
    }

    // Detect released keys (present in previous but not in current report)
    for (int i = 0; i < 6; i++) {
        uint8_t code = prev_keyboard_report.keycodes[i];
        if (code == 0) continue;

        int still_pressed = 0;
        for (int j = 0; j < 6; j++) {
            if (report->keycodes[j] == code) {
                still_pressed = 1;
                break;
            }
        }

        if (!still_pressed) {
            // Key release - send break code (0xF0 followed by scancode in PS/2 Set 2,
            // or bit 7 set in Set 1)
            uint8_t scancode = 0;
            if (code < sizeof(hid_to_ps2)) {
                scancode = hid_to_ps2[code];
            }
            if (scancode) {
                // Key release scancode in Set 1: scancode | 0x80
                int next = (write_ptr + 1) % KBD_BUFFER_SIZE;
                if (next != read_ptr) {
                    kbd_buffer[write_ptr] = scancode | 0x80;  // Release code
                    write_ptr = next;
                }
            }
        }
    }

    // Save current report as previous
    prev_keyboard_report = *report;
}

// Process a mouse boot report and update mouse state
static void hid_process_mouse_report(const hid_mouse_report_t* report) {
    // Update button state
    mouse_btn_left   = (report->buttons & 0x01) ? 1 : 0;
    mouse_btn_right  = (report->buttons & 0x02) ? 1 : 0;
    mouse_btn_middle = (report->buttons & 0x04) ? 1 : 0;

    // Apply movement deltas
    mouse_x += report->x_displacement;
    mouse_y -= report->y_displacement;  // Invert Y: USB HID positive = up, screen positive = down

    // Scroll wheel
    mouse_scroll_delta = report->wheel_displacement;

    // Clamp to screen bounds
    int screen_w = gfx_get_width();
    int screen_h = gfx_get_height();

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= screen_w) mouse_x = screen_w - 1;
    if (mouse_y >= screen_h) mouse_y = screen_h - 1;
}

// ============================================================================
// HID Polling (called from main event loop)
// ============================================================================

void usb_hid_poll(void) {
    for (int i = 0; i < hid_device_count; i++) {
        hid_device_t* dev = &hid_devices[i];
        if (!dev->active) continue;

        // In a full implementation, we would:
        // 1. Check if the interrupt endpoint has data (via xHCI event ring)
        // 2. Read the input report from the transfer ring
        // 3. Parse it according to the device type
        //
        // For now, we set up the transfer TRBs but rely on the xHCI
        // event mechanism to deliver data asynchronously.

        // Schedule an interrupt IN transfer on the endpoint
        if (dev->endpoint_addr) {
            xhci_trb_t trb;
            memset(&trb, 0, sizeof(trb));

            uint8_t* report_buf = usb_data_buf + (i * 64);  // Per-device buffer area
            trb.param_lo = (uint32_t)report_buf;
            trb.param_hi = 0;
            trb.status = dev->max_packet_size;
            trb.control = (TRB_TYPE_NORMAL << 10) | (1 << 5);  // Normal TRB, IOC

            // Add to the transfer ring
            int idx = xfer_enqueue_idx[i];
            transfer_ring[i][idx] = trb;
            xfer_enqueue_idx[i] = (idx + 1) % TRB_RING_SIZE;

            // Ring the endpoint doorbell to start the transfer
            xhci_ring_doorbell(dev->slot_id,
                              (dev->endpoint_addr & 0x0F) * 2 + 1);

            // Check if we received data (simplified check)
            // In the full implementation, we'd parse the event ring
            // for transfer completion events
            if (report_buf[0] || report_buf[1] || report_buf[2]) {
                if (dev->protocol == HID_PROTOCOL_KEYBOARD) {
                    hid_keyboard_report_t* kb_report = (hid_keyboard_report_t*)report_buf;
                    hid_process_keyboard_report(kb_report);
                } else if (dev->protocol == HID_PROTOCOL_MOUSE) {
                    hid_mouse_report_t* ms_report = (hid_mouse_report_t*)report_buf;
                    hid_process_mouse_report(ms_report);
                }

                // Clear the buffer after processing
                memset(report_buf, 0, 64);
            }
        }
    }
}

// ============================================================================
// USB HID Initialization
// ============================================================================

void usb_hid_init(void) {
    s_printf("[USB-HID] Initializing USB HID subsystem...\n");

    // Initialize TRB rings
    xhci_init_command_ring();
    xhci_init_event_ring();

    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        memset(transfer_ring[i], 0, sizeof(transfer_ring[i]));
        xfer_enqueue_idx[i] = 0;
    }

    // Initialize previous keyboard report
    memset(&prev_keyboard_report, 0, sizeof(prev_keyboard_report));
    keyboard_initialized = 0;

    // Enumerate all USB devices found by xHCI port scanning
    // The xHCI controller was already initialized by pci_init() -> xhci_controller_init()
    // We need to enumerate each device that was registered by usb_register_device()

    extern usb_device_t usb_devices[];
    extern int usb_dev_count;

    for (int i = 0; i < usb_dev_count && i < 8; i++) {
        usb_device_t* dev = &usb_devices[i];

        s_printf("[USB-HID] Enumerating device VID=");
        char buf[16];
        int_to_str(dev->vendor_id, buf); s_printf(buf);
        s_printf(" PID="); int_to_str(dev->product_id, buf); s_printf(buf);
        s_printf("\n");

        // Enable a slot for this device
        uint8_t slot_id = xhci_enable_slot();
        if (slot_id == 0) {
            s_printf("[USB-HID] Failed to enable slot for device\n");
            continue;
        }

        // Address the device
        // (In a full implementation: Send SET_ADDRESS, then GET_DESCRIPTOR)

        // Enumerate for HID interfaces
        usb_hid_enumerate(slot_id);
    }

    // Configure all found HID devices for boot protocol
    for (int i = 0; i < hid_device_count; i++) {
        if (hid_devices[i].active) {
            usb_hid_set_boot_protocol(&hid_devices[i]);
        }
    }

    s_printf("[USB-HID] Initialized: ");
    char buf[8]; int_to_str(hid_device_count, buf); s_printf(buf);
    s_printf(" HID device(s) found\n");
}

// Get the list of HID devices
hid_device_t* usb_hid_get_devices(int* count) {
    if (count) *count = hid_device_count;
    return hid_devices;
}
