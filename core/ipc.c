// core/ipc.c - Inter-Process Communication for CamelOS
// Provides message-passing IPC, shared memory, and process isolation
//
// Architecture:
//   - Message queues per process for async IPC
//   - Shared memory regions for fast data transfer
//   - Port-based communication (like Mach ports)
//   - Notification system for event signaling
//
// Process Model:
//   - Ring 3 user-mode processes (future)
//   - Ring 0 kernel services
//   - IPC works in both ring 0 and ring 3 (when implemented)

#include "ipc.h"
#include "memory.h"
#include "string.h"
#include "../hal/drivers/serial.h"

// ============================================================================
// Global IPC State
// ============================================================================

static ipc_port_t g_ports[IPC_MAX_PORTS];
static ipc_message_t g_message_pool[IPC_MAX_MESSAGES];
static ipc_shm_region_t g_shm_regions[IPC_MAX_SHM_REGIONS];
static int g_next_port_id = 1;
static int g_next_msg_id = 1;
static int g_next_shm_id = 1;

// ============================================================================
// Initialization
// ============================================================================

void ipc_init(void) {
    memset(g_ports, 0, sizeof(g_ports));
    memset(g_message_pool, 0, sizeof(g_message_pool));
    memset(g_shm_regions, 0, sizeof(g_shm_regions));
    g_next_port_id = 1;
    g_next_msg_id = 1;
    g_next_shm_id = 1;
    
    s_printf("[IPC] Inter-Process Communication initialized\n");
}

// ============================================================================
// Port Management
// ============================================================================

ipc_port_t* ipc_port_create(int owner_pid) {
    if (owner_pid < 0) return NULL;
    
    // Find free port slot
    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        if (!g_ports[i].in_use) {
            g_ports[i].port_id = g_next_port_id++;
            g_ports[i].owner_pid = owner_pid;
            g_ports[i].in_use = 1;
            g_ports[i].message_count = 0;
            g_ports[i].head = NULL;
            g_ports[i].tail = NULL;
            g_ports[i].notify_func = NULL;
            g_ports[i].notify_data = NULL;
            return &g_ports[i];
        }
    }
    
    s_printf("[IPC] No free port slots\n");
    return NULL;
}

void ipc_port_destroy(ipc_port_t* port) {
    if (!port || !port->in_use) return;
    
    // Free all pending messages
    ipc_message_t* msg = port->head;
    while (msg) {
        ipc_message_t* next = msg->next;
        msg->in_use = 0;
        msg = next;
    }
    
    port->in_use = 0;
    port->head = NULL;
    port->tail = NULL;
    port->message_count = 0;
}

ipc_port_t* ipc_port_lookup(int port_id) {
    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        if (g_ports[i].in_use && g_ports[i].port_id == port_id) {
            return &g_ports[i];
        }
    }
    return NULL;
}

// ============================================================================
// Message Sending
// ============================================================================

int ipc_send(int dest_port_id, int src_port_id, uint32_t msg_type,
             const void* data, uint32_t data_size) {
    ipc_port_t* dest = ipc_port_lookup(dest_port_id);
    if (!dest) return IPC_ERR_PORT_NOT_FOUND;
    
    if (data_size > IPC_MAX_MSG_SIZE) return IPC_ERR_MSG_TOO_LARGE;
    
    // Allocate a message from the pool
    ipc_message_t* msg = NULL;
    for (int i = 0; i < IPC_MAX_MESSAGES; i++) {
        if (!g_message_pool[i].in_use) {
            msg = &g_message_pool[i];
            break;
        }
    }
    
    if (!msg) return IPC_ERR_NO_MEMORY;
    
    // Initialize message
    msg->msg_id = g_next_msg_id++;
    msg->src_port_id = src_port_id;
    msg->dest_port_id = dest_port_id;
    msg->msg_type = msg_type;
    msg->data_size = data_size;
    msg->in_use = 1;
    msg->next = NULL;
    
    // Copy data
    if (data && data_size > 0) {
        memcpy(msg->data, data, data_size);
    }
    
    // Enqueue to destination port
    if (dest->tail) {
        dest->tail->next = msg;
    } else {
        dest->head = msg;
    }
    dest->tail = msg;
    dest->message_count++;
    
    // Notify the port owner
    if (dest->notify_func) {
        dest->notify_func(dest->port_id, msg->msg_type, dest->notify_data);
    }
    
    return msg->msg_id;
}

int ipc_send_simple(int dest_port_id, uint32_t msg_type) {
    return ipc_send(dest_port_id, 0, msg_type, NULL, 0);
}

// ============================================================================
// Message Receiving
// ============================================================================

int ipc_recv(int port_id, ipc_message_t* out_msg) {
    ipc_port_t* port = ipc_port_lookup(port_id);
    if (!port) return IPC_ERR_PORT_NOT_FOUND;
    
    if (port->message_count == 0) return IPC_ERR_NO_MESSAGES;
    
    ipc_message_t* msg = port->head;
    if (!msg) return IPC_ERR_NO_MESSAGES;
    
    // Dequeue
    port->head = msg->next;
    if (!port->head) port->tail = NULL;
    port->message_count--;
    
    // Copy to output
    if (out_msg) {
        memcpy(out_msg, msg, sizeof(ipc_message_t));
    }
    
    // Mark message as free
    msg->in_use = 0;
    msg->next = NULL;
    
    return 0;
}

int ipc_recv_type(int port_id, uint32_t msg_type, ipc_message_t* out_msg) {
    ipc_port_t* port = ipc_port_lookup(port_id);
    if (!port) return IPC_ERR_PORT_NOT_FOUND;
    
    // Search for a message of the specified type
    ipc_message_t* prev = NULL;
    ipc_message_t* msg = port->head;
    
    while (msg) {
        if (msg->msg_type == msg_type) {
            // Found - remove from queue
            if (prev) {
                prev->next = msg->next;
            } else {
                port->head = msg->next;
            }
            if (msg == port->tail) port->tail = prev;
            port->message_count--;
            
            if (out_msg) {
                memcpy(out_msg, msg, sizeof(ipc_message_t));
            }
            
            msg->in_use = 0;
            msg->next = NULL;
            return 0;
        }
        prev = msg;
        msg = msg->next;
    }
    
    return IPC_ERR_NO_MESSAGES;
}

// ============================================================================
// Port Notification
// ============================================================================

void ipc_port_set_notify(int port_id, ipc_notify_func_t func, void* user_data) {
    ipc_port_t* port = ipc_port_lookup(port_id);
    if (!port) return;
    
    port->notify_func = func;
    port->notify_data = user_data;
}

// ============================================================================
// Shared Memory
// ============================================================================

int ipc_shm_create(uint32_t size) {
    if (size == 0 || size > IPC_SHM_MAX_SIZE) return IPC_ERR_INVALID_PARAM;
    
    // Find free slot
    for (int i = 0; i < IPC_MAX_SHM_REGIONS; i++) {
        if (!g_shm_regions[i].in_use) {
            void* addr = kmalloc(size);
            if (!addr) return IPC_ERR_NO_MEMORY;
            
            g_shm_regions[i].shm_id = g_next_shm_id++;
            g_shm_regions[i].address = addr;
            g_shm_regions[i].size = size;
            g_shm_regions[i].in_use = 1;
            g_shm_regions[i].ref_count = 1;
            
            return g_shm_regions[i].shm_id;
        }
    }
    
    return IPC_ERR_NO_MEMORY;
}

void* ipc_shm_map(int shm_id) {
    for (int i = 0; i < IPC_MAX_SHM_REGIONS; i++) {
        if (g_shm_regions[i].in_use && g_shm_regions[i].shm_id == shm_id) {
            g_shm_regions[i].ref_count++;
            return g_shm_regions[i].address;
        }
    }
    return NULL;
}

void ipc_shm_unmap(int shm_id) {
    for (int i = 0; i < IPC_MAX_SHM_REGIONS; i++) {
        if (g_shm_regions[i].in_use && g_shm_regions[i].shm_id == shm_id) {
            g_shm_regions[i].ref_count--;
            if (g_shm_regions[i].ref_count <= 0) {
                kfree(g_shm_regions[i].address);
                g_shm_regions[i].in_use = 0;
            }
            return;
        }
    }
}

// ============================================================================
// RPC (Remote Procedure Call) - Synchronous message passing
// ============================================================================

int ipc_rpc_call(int dest_port_id, uint32_t method,
                 const void* args, uint32_t args_size,
                 void* reply, uint32_t* reply_size) {
    // Send request
    int msg_id = ipc_send(dest_port_id, 0, method, args, args_size);
    if (msg_id < 0) return msg_id;
    
    // Wait for reply (polling with timeout)
    // In a real system this would block the calling thread
    // For now, we poll for a reply message
    // TODO: Implement proper blocking wait
    
    return msg_id;
}

// ============================================================================
// Service Registry
// ============================================================================

#define IPC_MAX_SERVICES 32

typedef struct {
    char name[64];
    int port_id;
    int owner_pid;
    int in_use;
} ipc_service_t;

static ipc_service_t g_services[IPC_MAX_SERVICES];

int ipc_register_service(const char* name, int port_id, int owner_pid) {
    if (!name) return IPC_ERR_INVALID_PARAM;
    
    for (int i = 0; i < IPC_MAX_SERVICES; i++) {
        if (!g_services[i].in_use) {
            strncpy(g_services[i].name, name, 63);
            g_services[i].name[63] = 0;
            g_services[i].port_id = port_id;
            g_services[i].owner_pid = owner_pid;
            g_services[i].in_use = 1;
            return 0;
        }
    }
    
    return IPC_ERR_NO_MEMORY;
}

int ipc_lookup_service(const char* name) {
    if (!name) return IPC_ERR_SERVICE_NOT_FOUND;
    
    for (int i = 0; i < IPC_MAX_SERVICES; i++) {
        if (g_services[i].in_use && strcmp(g_services[i].name, name) == 0) {
            return g_services[i].port_id;
        }
    }
    
    return IPC_ERR_SERVICE_NOT_FOUND;
}

void ipc_unregister_service(const char* name) {
    if (!name) return;
    
    for (int i = 0; i < IPC_MAX_SERVICES; i++) {
        if (g_services[i].in_use && strcmp(g_services[i].name, name) == 0) {
            g_services[i].in_use = 0;
            return;
        }
    }
}

// ============================================================================
// Debug / Status
// ============================================================================

void ipc_print_status(void) {
    int active_ports = 0, active_msgs = 0, active_shm = 0;
    
    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        if (g_ports[i].in_use) active_ports++;
    }
    for (int i = 0; i < IPC_MAX_MESSAGES; i++) {
        if (g_message_pool[i].in_use) active_msgs++;
    }
    for (int i = 0; i < IPC_MAX_SHM_REGIONS; i++) {
        if (g_shm_regions[i].in_use) active_shm++;
    }
    
    s_printf("[IPC] Status: ");
    char buf[16];
    int_to_str(active_ports, buf); s_printf(buf); s_printf(" ports, ");
    int_to_str(active_msgs, buf); s_printf(buf); s_printf(" msgs, ");
    int_to_str(active_shm, buf); s_printf(buf); s_printf(" shm regions\n");
}

int ipc_get_active_port_count(void) {
    int count = 0;
    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        if (g_ports[i].in_use) count++;
    }
    return count;
}
