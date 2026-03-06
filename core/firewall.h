// core/firewall.h - Network Firewall for CamelOS
#ifndef FIREWALL_H
#define FIREWALL_H

#include "../include/types.h"

// Firewall rule actions
#define FW_ACTION_ALLOW  0
#define FW_ACTION_BLOCK  1
#define FW_ACTION_LOG    2

// Firewall rule directions
#define FW_DIR_INCOMING  0
#define FW_DIR_OUTGOING  1
#define FW_DIR_BOTH      2

// Firewall rule protocols
#define FW_PROTO_ANY     0
#define FW_PROTO_TCP     1
#define FW_PROTO_UDP     2
#define FW_PROTO_ICMP    3

// Maximum rules
#define FW_MAX_RULES     64

// Firewall rule structure
typedef struct {
    int id;
    int enabled;
    int action;         // FW_ACTION_*
    int direction;      // FW_DIR_*
    int protocol;       // FW_PROTO_*
    
    // Source (for incoming rules)
    uint32_t src_ip;
    uint32_t src_mask;
    uint16_t src_port_start;
    uint16_t src_port_end;
    
    // Destination (for outgoing rules)
    uint32_t dst_ip;
    uint32_t dst_mask;
    uint16_t dst_port_start;
    uint16_t dst_port_end;
    
    // Logging
    int log_matches;
    char description[64];
    
    // Statistics
    uint32_t match_count;
    uint32_t last_match_time;
} firewall_rule_t;

// Firewall state
typedef struct {
    int enabled;
    int default_incoming_action;   // Default action for incoming traffic
    int default_outgoing_action;   // Default action for outgoing traffic
    int log_blocked;
    int log_allowed;
    firewall_rule_t rules[FW_MAX_RULES];
    int rule_count;
    
    // Statistics
    uint32_t total_incoming;
    uint32_t total_outgoing;
    uint32_t total_blocked;
    uint32_t total_allowed;
} firewall_state_t;

// Initialize firewall
void firewall_init(void);

// Enable/disable firewall
void firewall_enable(int enable);
int firewall_is_enabled(void);

// Set default policy
void firewall_set_default_policy(int direction, int action);

// Add/remove rules
int firewall_add_rule(firewall_rule_t* rule);
int firewall_remove_rule(int rule_id);
int firewall_update_rule(int rule_id, firewall_rule_t* rule);
void firewall_clear_rules(void);

// Get rules
int firewall_get_rule_count(void);
firewall_rule_t* firewall_get_rule(int index);
firewall_rule_t* firewall_get_rule_by_id(int id);

// Check packet against rules
int firewall_check_incoming(uint32_t src_ip, uint16_t src_port, 
                            uint32_t dst_ip, uint16_t dst_port, int protocol);
int firewall_check_outgoing(uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port, int protocol);

// Logging
void firewall_log_packet(const char* direction, uint32_t src_ip, uint16_t src_port,
                        uint32_t dst_ip, uint16_t dst_port, int protocol, int action);

// Utility functions
void firewall_ip_to_str(uint32_t ip, char* str);
uint32_t firewall_str_to_ip(const char* str);

// Get firewall statistics
void firewall_get_stats(uint32_t* incoming, uint32_t* outgoing, 
                        uint32_t* blocked, uint32_t* allowed);

// Rule presets for common configurations
void firewall_preset_secure(void);      // Block all incoming except established
void firewall_preset_permissive(void);  // Allow all, log only
void firewall_preset_balanced(void);    // Allow outgoing, block incoming except DNS/DHCP

#endif // FIREWALL_H
