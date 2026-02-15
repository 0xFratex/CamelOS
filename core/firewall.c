// core/firewall.c - Network Firewall Implementation for CamelOS
#include "firewall.h"
#include "net.h"
#include "string.h"
#include "memory.h"
#include "../hal/drivers/serial.h"
#include "timer.h"

// External functions
extern void printk(const char* fmt, ...);

// Firewall global state
static firewall_state_t fw_state;

// Initialize firewall with secure defaults
void firewall_init(void) {
    memset(&fw_state, 0, sizeof(fw_state));
    fw_state.enabled = 0;  // Disabled by default
    fw_state.default_incoming_action = FW_ACTION_BLOCK;
    fw_state.default_outgoing_action = FW_ACTION_ALLOW;
    fw_state.log_blocked = 1;
    fw_state.log_allowed = 0;
    fw_state.rule_count = 0;
    
    s_printf("[FIREWALL] Initialized (disabled by default)\n");
}

// Enable/disable firewall
void firewall_enable(int enable) {
    fw_state.enabled = enable ? 1 : 0;
    s_printf("[FIREWALL] %s\n", enable ? "Enabled" : "Disabled");
}

int firewall_is_enabled(void) {
    return fw_state.enabled;
}

// Set default policy
void firewall_set_default_policy(int direction, int action) {
    if (direction == FW_DIR_INCOMING) {
        fw_state.default_incoming_action = action;
    } else if (direction == FW_DIR_OUTGOING) {
        fw_state.default_outgoing_action = action;
    }
}

// Find a free rule slot
static int find_free_rule_slot(void) {
    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (!fw_state.rules[i].enabled) {
            return i;
        }
    }
    return -1;
}

// Find rule by ID
static int find_rule_by_id(int id) {
    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (fw_state.rules[i].enabled && fw_state.rules[i].id == id) {
            return i;
        }
    }
    return -1;
}

// Add a new rule
int firewall_add_rule(firewall_rule_t* rule) {
    if (!rule) return -1;
    
    int slot = find_free_rule_slot();
    if (slot < 0) {
        s_printf("[FIREWALL] Error: No free rule slots\n");
        return -1;
    }
    
    // Assign ID if not set
    if (rule->id == 0) {
        static int next_id = 1;
        rule->id = next_id++;
    }
    
    // Copy rule to slot
    memcpy(&fw_state.rules[slot], rule, sizeof(firewall_rule_t));
    fw_state.rules[slot].enabled = 1;
    fw_state.rules[slot].match_count = 0;
    fw_state.rules[slot].last_match_time = 0;
    
    fw_state.rule_count++;
    
    s_printf("[FIREWALL] Added rule %d: %s\n", rule->id, rule->description);
    return rule->id;
}

// Remove a rule
int firewall_remove_rule(int rule_id) {
    int slot = find_rule_by_id(rule_id);
    if (slot < 0) {
        return -1;
    }
    
    fw_state.rules[slot].enabled = 0;
    fw_state.rule_count--;
    
    s_printf("[FIREWALL] Removed rule %d\n", rule_id);
    return 0;
}

// Update a rule
int firewall_update_rule(int rule_id, firewall_rule_t* rule) {
    int slot = find_rule_by_id(rule_id);
    if (slot < 0) {
        return -1;
    }
    
    // Preserve statistics
    uint32_t match_count = fw_state.rules[slot].match_count;
    uint32_t last_match = fw_state.rules[slot].last_match_time;
    
    // Copy updated rule
    memcpy(&fw_state.rules[slot], rule, sizeof(firewall_rule_t));
    fw_state.rules[slot].id = rule_id;
    fw_state.rules[slot].enabled = 1;
    fw_state.rules[slot].match_count = match_count;
    fw_state.rules[slot].last_match_time = last_match;
    
    return 0;
}

// Clear all rules
void firewall_clear_rules(void) {
    for (int i = 0; i < FW_MAX_RULES; i++) {
        fw_state.rules[i].enabled = 0;
    }
    fw_state.rule_count = 0;
    s_printf("[FIREWALL] All rules cleared\n");
}

// Get rule count
int firewall_get_rule_count(void) {
    return fw_state.rule_count;
}

// Get rule by index
firewall_rule_t* firewall_get_rule(int index) {
    if (index < 0 || index >= FW_MAX_RULES) return 0;
    if (!fw_state.rules[index].enabled) return 0;
    return &fw_state.rules[index];
}

// Get rule by ID
firewall_rule_t* firewall_get_rule_by_id(int id) {
    int slot = find_rule_by_id(id);
    if (slot < 0) return 0;
    return &fw_state.rules[slot];
}

// Check if IP matches rule (with subnet mask)
static int ip_matches_rule(uint32_t ip, uint32_t rule_ip, uint32_t mask) {
    // 0.0.0.0 means "any IP"
    if (rule_ip == 0) return 1;
    if (mask == 0) mask = 0xFFFFFFFF;
    return (ip & mask) == (rule_ip & mask);
}

// Check if port matches rule
static int port_matches_rule(uint16_t port, uint16_t start, uint16_t end) {
    // 0 means "any port"
    if (start == 0 && end == 0) return 1;
    if (end == 0) end = start;
    return port >= start && port <= end;
}

// Check if protocol matches
static int protocol_matches_rule(int packet_proto, int rule_proto) {
    if (rule_proto == FW_PROTO_ANY) return 1;
    return packet_proto == rule_proto;
}

// Check incoming packet against rules
int firewall_check_incoming(uint32_t src_ip, uint16_t src_port, 
                            uint32_t dst_ip, uint16_t dst_port, int protocol) {
    // If firewall is disabled, allow all
    if (!fw_state.enabled) {
        return FW_ACTION_ALLOW;
    }
    
    fw_state.total_incoming++;
    
    // Check rules in order (first match wins)
    for (int i = 0; i < FW_MAX_RULES; i++) {
        firewall_rule_t* rule = &fw_state.rules[i];
        
        if (!rule->enabled) continue;
        
        // Skip outgoing-only rules
        if (rule->direction == FW_DIR_OUTGOING) continue;
        
        // Check protocol
        if (!protocol_matches_rule(protocol, rule->protocol)) continue;
        
        // Check source IP and port
        if (!ip_matches_rule(src_ip, rule->src_ip, rule->src_mask)) continue;
        if (!port_matches_rule(src_port, rule->src_port_start, rule->src_port_end)) continue;
        
        // Check destination IP and port
        if (!ip_matches_rule(dst_ip, rule->dst_ip, rule->dst_mask)) continue;
        if (!port_matches_rule(dst_port, rule->dst_port_start, rule->dst_port_end)) continue;
        
        // Rule matches!
        rule->match_count++;
        rule->last_match_time = timer_get_ticks();
        
        if (rule->log_matches) {
            firewall_log_packet("IN", src_ip, src_port, dst_ip, dst_port, 
                               protocol, rule->action);
        }
        
        return rule->action;
    }
    
    // No rule matched, use default policy
    int action = fw_state.default_incoming_action;
    
    if (action == FW_ACTION_BLOCK && fw_state.log_blocked) {
        firewall_log_packet("IN(BLOCKED)", src_ip, src_port, dst_ip, dst_port, 
                           protocol, action);
        fw_state.total_blocked++;
    } else {
        fw_state.total_allowed++;
    }
    
    return action;
}

// Check outgoing packet against rules
int firewall_check_outgoing(uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port, int protocol) {
    // If firewall is disabled, allow all
    if (!fw_state.enabled) {
        return FW_ACTION_ALLOW;
    }
    
    fw_state.total_outgoing++;
    
    // Check rules in order (first match wins)
    for (int i = 0; i < FW_MAX_RULES; i++) {
        firewall_rule_t* rule = &fw_state.rules[i];
        
        if (!rule->enabled) continue;
        
        // Skip incoming-only rules
        if (rule->direction == FW_DIR_INCOMING) continue;
        
        // Check protocol
        if (!protocol_matches_rule(protocol, rule->protocol)) continue;
        
        // For outgoing rules, src is our IP, dst is remote
        if (!ip_matches_rule(dst_ip, rule->dst_ip, rule->dst_mask)) continue;
        if (!port_matches_rule(dst_port, rule->dst_port_start, rule->dst_port_end)) continue;
        
        // Rule matches!
        rule->match_count++;
        rule->last_match_time = timer_get_ticks();
        
        if (rule->log_matches) {
            firewall_log_packet("OUT", src_ip, src_port, dst_ip, dst_port, 
                               protocol, rule->action);
        }
        
        return rule->action;
    }
    
    // No rule matched, use default policy
    int action = fw_state.default_outgoing_action;
    
    if (action == FW_ACTION_BLOCK && fw_state.log_blocked) {
        firewall_log_packet("OUT(BLOCKED)", src_ip, src_port, dst_ip, dst_port, 
                           protocol, action);
        fw_state.total_blocked++;
    } else {
        fw_state.total_allowed++;
    }
    
    return action;
}

// Log packet
void firewall_log_packet(const char* direction, uint32_t src_ip, uint16_t src_port,
                        uint32_t dst_ip, uint16_t dst_port, int protocol, int action) {
    char src_str[16], dst_str[16];
    firewall_ip_to_str(src_ip, src_str);
    firewall_ip_to_str(dst_ip, dst_str);
    
    const char* proto_str = "ANY";
    if (protocol == FW_PROTO_TCP) proto_str = "TCP";
    else if (protocol == FW_PROTO_UDP) proto_str = "UDP";
    else if (protocol == FW_PROTO_ICMP) proto_str = "ICMP";
    
    const char* action_str = action == FW_ACTION_ALLOW ? "ALLOW" : "BLOCK";
    
    s_printf("[FIREWALL] %s %s %s:%d -> %s:%d (%s) %s\n",
             direction, action_str, src_str, src_port, dst_str, dst_port,
             proto_str, action_str);
}

// Convert IP to string
void firewall_ip_to_str(uint32_t ip, char* str) {
    int a = (ip >> 24) & 0xFF;
    int b = (ip >> 16) & 0xFF;
    int c = (ip >> 8) & 0xFF;
    int d = ip & 0xFF;
    
    char* p = str;
    int digits;
    
    digits = a / 100; if (digits) *p++ = '0' + digits;
    digits = (a / 10) % 10; if (a >= 10) *p++ = '0' + digits;
    *p++ = '0' + (a % 10);
    *p++ = '.';
    
    digits = b / 100; if (digits) *p++ = '0' + digits;
    digits = (b / 10) % 10; if (b >= 10) *p++ = '0' + digits;
    *p++ = '0' + (b % 10);
    *p++ = '.';
    
    digits = c / 100; if (digits) *p++ = '0' + digits;
    digits = (c / 10) % 10; if (c >= 10) *p++ = '0' + digits;
    *p++ = '0' + (c % 10);
    *p++ = '.';
    
    digits = d / 100; if (digits) *p++ = '0' + digits;
    digits = (d / 10) % 10; if (d >= 10) *p++ = '0' + digits;
    *p++ = '0' + (d % 10);
    *p++ = 0;
}

// Convert string to IP
uint32_t firewall_str_to_ip(const char* str) {
    uint32_t ip = 0;
    int octet = 0;
    
    while (*str) {
        if (*str >= '0' && *str <= '9') {
            octet = octet * 10 + (*str - '0');
        } else if (*str == '.') {
            ip = (ip << 8) | octet;
            octet = 0;
        }
        str++;
    }
    ip = (ip << 8) | octet;
    
    return ip;
}

// Get firewall statistics
void firewall_get_stats(uint32_t* incoming, uint32_t* outgoing, 
                        uint32_t* blocked, uint32_t* allowed) {
    if (incoming) *incoming = fw_state.total_incoming;
    if (outgoing) *outgoing = fw_state.total_outgoing;
    if (blocked) *blocked = fw_state.total_blocked;
    if (allowed) *allowed = fw_state.total_allowed;
}

// Preset: Secure (block all incoming except established connections)
void firewall_preset_secure(void) {
    firewall_clear_rules();
    
    // Allow outgoing connections
    firewall_rule_t rule1 = {
        .action = FW_ACTION_ALLOW,
        .direction = FW_DIR_OUTGOING,
        .protocol = FW_PROTO_ANY,
        .description = "Allow all outgoing"
    };
    firewall_add_rule(&rule1);
    
    // Block all incoming
    firewall_set_default_policy(FW_DIR_INCOMING, FW_ACTION_BLOCK);
    firewall_set_default_policy(FW_DIR_OUTGOING, FW_ACTION_ALLOW);
    
    s_printf("[FIREWALL] Applied secure preset\n");
}

// Preset: Permissive (allow all, log only)
void firewall_preset_permissive(void) {
    firewall_clear_rules();
    
    // Allow everything
    firewall_set_default_policy(FW_DIR_INCOMING, FW_ACTION_ALLOW);
    firewall_set_default_policy(FW_DIR_OUTGOING, FW_ACTION_ALLOW);
    
    fw_state.log_allowed = 1;
    fw_state.log_blocked = 1;
    
    s_printf("[FIREWALL] Applied permissive preset\n");
}

// Preset: Balanced (allow outgoing, block incoming except DNS/DHCP)
void firewall_preset_balanced(void) {
    firewall_clear_rules();
    
    // Allow DNS responses (UDP port 53)
    firewall_rule_t rule1 = {
        .action = FW_ACTION_ALLOW,
        .direction = FW_DIR_INCOMING,
        .protocol = FW_PROTO_UDP,
        .src_port_start = 53,
        .src_port_end = 53,
        .description = "Allow DNS responses"
    };
    firewall_add_rule(&rule1);
    
    // Allow DHCP responses
    firewall_rule_t rule2 = {
        .action = FW_ACTION_ALLOW,
        .direction = FW_DIR_INCOMING,
        .protocol = FW_PROTO_UDP,
        .src_port_start = 67,
        .src_port_end = 67,
        .dst_port_start = 68,
        .dst_port_end = 68,
        .description = "Allow DHCP responses"
    };
    firewall_add_rule(&rule2);
    
    // Allow all outgoing
    firewall_rule_t rule3 = {
        .action = FW_ACTION_ALLOW,
        .direction = FW_DIR_OUTGOING,
        .protocol = FW_PROTO_ANY,
        .description = "Allow all outgoing"
    };
    firewall_add_rule(&rule3);
    
    // Block incoming by default
    firewall_set_default_policy(FW_DIR_INCOMING, FW_ACTION_BLOCK);
    firewall_set_default_policy(FW_DIR_OUTGOING, FW_ACTION_ALLOW);
    
    s_printf("[FIREWALL] Applied balanced preset\n");
}
