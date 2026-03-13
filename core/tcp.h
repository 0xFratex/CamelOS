// core/tcp.h
#ifndef TCP_H
#define TCP_H

#include "../include/types.h"

// TCP Header
typedef struct {
    uint16_t src_port;
    uint16_t dest_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t data_offset;  // Upper 4 bits only
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed)) tcp_header_t;

// TCP Flags
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

// TCP States
#define TCP_CLOSED       0
#define TCP_LISTEN       1
#define TCP_SYN_SENT     2
#define TCP_SYN_RECEIVED 3
#define TCP_ESTABLISHED  4
#define TCP_FIN_WAIT1    5
#define TCP_FIN_WAIT2    6
#define TCP_CLOSE_WAIT   7
#define TCP_CLOSING      8
#define TCP_LAST_ACK     9
#define TCP_TIME_WAIT    10

// TCP Connection
typedef struct tcp_connection {
    uint8_t state;
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;

    // Sequence numbers
    uint32_t snd_nxt;
    uint32_t snd_una;
    uint32_t rcv_nxt;

    // Buffers
    uint8_t send_buffer[4096];
    uint8_t recv_buffer[4096];
    uint16_t send_head;
    uint16_t send_tail;
    uint16_t recv_head;
    uint16_t recv_tail;

    // Timers
    uint32_t last_ack_time;
    uint32_t retransmit_timeout;
    uint8_t retransmit_count;

    // Connection info
    uint32_t connect_time;
    uint16_t window;
    uint16_t mss;

    // Callbacks
    void (*on_data)(uint8_t* data, uint16_t len, void* user_data);
    void (*on_state_change)(uint8_t old_state, uint8_t new_state);
    void* callback_user_data;
} tcp_connection_t;

// Functions
void tcp_init(void);
int tcp_connect(uint32_t remote_ip, uint16_t remote_port);
tcp_connection_t* tcp_connect_with_ptr(uint32_t remote_ip, uint16_t remote_port);
uint16_t tcp_conn_get_local_port(void* conn);
int tcp_conn_is_established(void* conn);
void tcp_conn_set_data_callback(void* conn, void (*callback)(uint8_t*, uint16_t, void*), void* user_data);
int tcp_send_data(tcp_connection_t* conn, uint8_t* data, uint16_t len);
uint16_t tcp_checksum(uint8_t* packet, uint16_t len, uint32_t src_ip, uint32_t dst_ip);
void tcp_handle_packet(uint8_t* packet, uint32_t len, uint32_t src_ip, uint32_t dst_ip);

#endif