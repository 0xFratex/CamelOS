// core/ipc.h - Inter-Process Communication for CamelOS
// Provides message-passing IPC, shared memory, and process isolation
#ifndef IPC_H
#define IPC_H

#include "../include/types.h"

// ============================================================================
// Error Codes
// ============================================================================
#define IPC_OK                  0
#define IPC_ERR_PORT_NOT_FOUND  -1
#define IPC_ERR_NO_MEMORY       -2
#define IPC_ERR_MSG_TOO_LARGE   -3
#define IPC_ERR_NO_MESSAGES     -4
#define IPC_ERR_INVALID_PARAM   -5
#define IPC_ERR_SERVICE_NOT_FOUND -6

// ============================================================================
// Configuration
// ============================================================================
#define IPC_MAX_PORTS           128
#define IPC_MAX_MESSAGES        1024
#define IPC_MAX_SHM_REGIONS     32
#define IPC_MAX_MSG_SIZE        4096
#define IPC_SHM_MAX_SIZE        (1024 * 1024)  // 1MB

// ============================================================================
// Standard Message Types
// ============================================================================
#define IPC_MSG_NONE            0
#define IPC_MSG_REQUEST         1
#define IPC_MSG_REPLY           2
#define IPC_MSG_NOTIFY          3
#define IPC_MSG_INTERRUPT       4

// Standard service message types
#define IPC_SVC_WINDOW_CMD      0x0100  // Window server commands
#define IPC_SVC_NETWORK_CMD     0x0200  // Network service commands
#define IPC_SVC_FILESYSTEM_CMD  0x0300  // Filesystem commands
#define IPC_SVC_LAUNCHER_CMD    0x0400  // App launcher commands
#define IPC_SVC_CLIPBOARD_CMD   0x0500  // Clipboard commands
#define IPC_SVC_NOTIFICATION    0x0600  // System notifications

// ============================================================================
// IPC Message
// ============================================================================
typedef struct ipc_message {
    int msg_id;
    int src_port_id;
    int dest_port_id;
    uint32_t msg_type;
    uint32_t data_size;
    uint8_t data[IPC_MAX_MSG_SIZE];
    int in_use;
    struct ipc_message* next;
} ipc_message_t;

// ============================================================================
// IPC Port
// ============================================================================
typedef struct {
    int port_id;
    int owner_pid;
    int in_use;
    int message_count;
    ipc_message_t* head;
    ipc_message_t* tail;
    
    // Notification callback - called when a message arrives
    void (*notify_func)(int port_id, uint32_t msg_type, void* user_data);
    void* notify_data;
} ipc_port_t;

// ============================================================================
// Shared Memory Region
// ============================================================================
typedef struct {
    int shm_id;
    void* address;
    uint32_t size;
    int in_use;
    int ref_count;
} ipc_shm_region_t;

// ============================================================================
// Notification Callback Type
// ============================================================================
typedef void (*ipc_notify_func_t)(int port_id, uint32_t msg_type, void* user_data);

// ============================================================================
// API Functions
// ============================================================================

// Initialize the IPC subsystem
void ipc_init(void);

// Port management
ipc_port_t* ipc_port_create(int owner_pid);
void ipc_port_destroy(ipc_port_t* port);
ipc_port_t* ipc_port_lookup(int port_id);

// Message sending (asynchronous)
int ipc_send(int dest_port_id, int src_port_id, uint32_t msg_type,
             const void* data, uint32_t data_size);
int ipc_send_simple(int dest_port_id, uint32_t msg_type);

// Message receiving
int ipc_recv(int port_id, ipc_message_t* out_msg);
int ipc_recv_type(int port_id, uint32_t msg_type, ipc_message_t* out_msg);

// Port notification
void ipc_port_set_notify(int port_id, ipc_notify_func_t func, void* user_data);

// Shared memory
int ipc_shm_create(uint32_t size);
void* ipc_shm_map(int shm_id);
void ipc_shm_unmap(int shm_id);

// RPC (synchronous call)
int ipc_rpc_call(int dest_port_id, uint32_t method,
                 const void* args, uint32_t args_size,
                 void* reply, uint32_t* reply_size);

// Service registry
int ipc_register_service(const char* name, int port_id, int owner_pid);
int ipc_lookup_service(const char* name);
void ipc_unregister_service(const char* name);

// Debug
void ipc_print_status(void);

// Get count of active IPC ports (for process monitor)
int ipc_get_active_port_count(void);

#endif // IPC_H
