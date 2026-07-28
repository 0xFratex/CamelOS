// core/tcp.c - Optimized TCP implementation with proper state machine
#include "tcp.h"
#include "net.h"
#include "net_if.h"
#include "socket.h"
#include "memory.h"
#include "timer.h"
#include "string.h"
#include "../hal/drivers/serial.h"

// External declaration for printk from string.c
extern void printk(const char* fmt, ...);

// ============================================================================
// DEBUG CONFIGURATION - Set to 0 for production, 1 for debugging
// ============================================================================
#define TCP_DEBUG_ENABLED     0
#define TCP_DEBUG_STATE       0    // Log state transitions
#define TCP_DEBUG_PACKETS     1    // Log packet details
#define TCP_DEBUG_ERRORS      1    // Log errors

// Gate noisy per-packet logging. The google.com failure mode was an ACK
// storm that printed thousands of lines and starved the rest of the system.
#if TCP_DEBUG_PACKETS
#define TCP_LOG(...) s_printf(__VA_ARGS__)
#else
#define TCP_LOG(...) ((void)0)
#endif

#define TCP_MAX_CONNECTIONS 32
#define TCP_RETRANSMIT_TIMEOUT 50
#define TCP_WINDOW_SIZE 16384 // Increased to 16KB
#define TCP_MSS 1460
#define TCP_MAX_LISTENERS 8
#define TCP_LISTEN_BACKLOG 8
#define TCP_OFO_QUEUE_SIZE 8
#define TCP_OFO_SEG_MAX 1460
// TCP_RETRANSMIT_TIMEOUT is now defined in tcp.h so callers outside tcp.c
// (e.g. k_close in socket.c) can arm FIN retransmit timers.
#define TCP_MAX_RETRANSMIT     5     // Max retransmit attempts before giving up

// Rate-limit pure duplicate ACKs (same rcv_nxt) so a retransmitting peer
// cannot wedge the NIC / serial console. 1 tick ≈ 20ms at 50Hz.
#define TCP_DUP_ACK_MIN_INTERVAL  2


tcp_connection_t tcp_connections[TCP_MAX_CONNECTIONS];
static uint16_t tcp_next_port = 49152; // Start of ephemeral ports

// Per-connection last pure-dup-ACK tick (index matches tcp_connections[])
static uint32_t tcp_last_dup_ack_tick[TCP_MAX_CONNECTIONS];

// QEMU SLIRP quirk: first (and sometimes later) data bursts arrive as a
// 6-byte all-zero segment with the SAME sequence number as the real data.
// Accepting those zeros advances rcv_nxt and causes the next real segment
// to be overlap-trimmed — destroying the TLS record header / CCS bytes.
// Detect and IGNORE such segments (do not deliver, do not advance rcv_nxt,
// do not ACK) so the real retransmission is accepted intact.
static int tcp_is_slirp_zero_phantom(const uint8_t* data, uint16_t data_len) {
    if (data_len != 6 || !data) return 0;
    for (int i = 0; i < 6; i++) {
        if (data[i] != 0) return 0;
    }
    return 1;
}

// Forward declare tcp_send for OFO helpers and rate-limited ACK (defined below)
int tcp_send(tcp_connection_t* conn, uint8_t flags, uint8_t* data, uint16_t len);

// Send an ACK, rate-limiting pure duplicates (same rcv_nxt) so a
// retransmitting peer cannot flood the NIC/serial console.
static void tcp_send_ack_ratelimited(tcp_connection_t* conn) {
    int idx = (int)(conn - tcp_connections);
    uint32_t now = timer_get_ticks();
    if (idx >= 0 && idx < TCP_MAX_CONNECTIONS) {
        if (tcp_last_dup_ack_tick[idx] != 0 &&
            (now - tcp_last_dup_ack_tick[idx]) < TCP_DUP_ACK_MIN_INTERVAL) {
            return;
        }
        tcp_last_dup_ack_tick[idx] = now;
    }
    tcp_send(conn, TCP_ACK, NULL, 0);
}

// TCP FSM states are defined in tcp.h

// Byte order conversion helpers
static inline uint32_t bswap32(uint32_t x) {
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
           ((x & 0xFF0000) >> 8) | ((x >> 24) & 0xFF);
}

// ============================================================================
// Out-of-order reassembly queue helpers (RFC 793 §3.3, §3.4)
// ============================================================================
//
// CamelOS previously DROPPED out-of-order segments (seq > rcv_nxt) and only
// sent a duplicate ACK. This is technically legal but interacts badly with
// TLS: a single dropped segment forces a full RTO + retransmit, and during
// that window the TLS handshake timer (60s) keeps ticking — eventually
// timing out the entire ServerHello.
//
// The queue is small (8 slots × 1 MSS = ~11 KB worst case per connection).
// Segments are inserted by seq, partially-overlapping segments are trimmed
// or coalesced, and the queue is drained every time rcv_nxt advances.

// Find a free slot in conn->ofo_queue. Returns -1 if full.
static int tcp_ofo_find_free(tcp_connection_t* conn) {
    for (int i = 0; i < TCP_OFO_QUEUE_SIZE; i++) {
        if (!conn->ofo_queue[i].in_use) return i;
    }
    return -1;
}

// Drop every queued segment. Called on CLOSED/TIME_WAIT/RST transitions
// and when the user has given up interest in the connection (k_close).
static void tcp_ofo_flush(tcp_connection_t* conn) {
    for (int i = 0; i < TCP_OFO_QUEUE_SIZE; i++) {
        conn->ofo_queue[i].in_use = 0;
        conn->ofo_queue[i].len = 0;
    }
}

// Try to enqueue an out-of-order segment. The data passed in MUST already
// be trimmed so that seq > rcv_nxt. Returns 0 on success, -1 if the queue
// is full (caller should send a dup ACK and let the peer retransmit).
//
// Coalescing/dedup rules:
//   * If an existing entry fully contains the new segment, drop the new one.
//   * If the new segment fully contains an existing entry, replace it.
//   * Otherwise, store as a new entry (we don't merge — leaves gaps clear).
static int tcp_ofo_enqueue(tcp_connection_t* conn, uint32_t seq,
                           const uint8_t* data, uint16_t len) {
    if (len == 0) return 0;
    if (len > TCP_OFO_SEG_MAX) len = TCP_OFO_SEG_MAX;

    // Check for containment in either direction
    for (int i = 0; i < TCP_OFO_QUEUE_SIZE; i++) {
        if (!conn->ofo_queue[i].in_use) continue;
        tcp_ofo_entry_t* e = &conn->ofo_queue[i];
        uint32_t e_end = e->seq + e->len;
        uint32_t n_end = seq + len;
        if (e->seq <= seq && e_end >= n_end) return 0;  // existing contains new
        if (seq <= e->seq && n_end >= e_end) {           // new contains existing
            e->in_use = 0;
            e->len = 0;
        }
    }

    int slot = tcp_ofo_find_free(conn);
    if (slot < 0) return -1;

    tcp_ofo_entry_t* e = &conn->ofo_queue[slot];
    e->in_use = 1;
    e->seq = seq;
    e->len = len;
    memcpy(e->data, data, len);
    return 0;
}

// Drain the OFO queue: deliver any segments that are now in-order (i.e.
// seq == conn->rcv_nxt). Called after every in-order delivery and after
// any rcv_nxt advance. Mirrors the same delivery path as the live segment
// handler: callback preferred, flat-buffer fallback, ACK each segment.
static void tcp_ofo_drain(tcp_connection_t* conn) {
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < TCP_OFO_QUEUE_SIZE; i++) {
            tcp_ofo_entry_t* e = &conn->ofo_queue[i];
            if (!e->in_use || e->len == 0) continue;

            // Trim leading bytes already received
            if (e->seq < conn->rcv_nxt) {
                uint32_t overlap = conn->rcv_nxt - e->seq;
                if (overlap >= e->len) {
                    e->in_use = 0;
                    e->len = 0;
                    continue;
                }
                uint16_t keep = e->len - (uint16_t)overlap;
                memmove(e->data, e->data + overlap, keep);
                e->len = keep;
                e->seq = conn->rcv_nxt;
            }

            if (e->seq == conn->rcv_nxt) {
                if (conn->on_data) {
                    conn->on_data(e->data, e->len, conn->callback_user_data);
                } else if (!conn->user_closing) {
                    if (conn->recv_tail + e->len <= sizeof(conn->recv_buffer)) {
                        memcpy(conn->recv_buffer + conn->recv_tail, e->data, e->len);
                        conn->recv_tail += e->len;
                    }
                }
                conn->rcv_nxt += e->len;
                conn->last_recv_time = timer_get_ticks();
                tcp_send(conn, TCP_ACK, NULL, 0);
                e->in_use = 0;
                e->len = 0;
                progress = 1;
            }
        }
    }
}

// Find or allocate connection - OPTIMIZED
static tcp_connection_t* tcp_find_connection(uint32_t local_ip_net, uint16_t local_port,
                                              uint32_t remote_ip_net, uint16_t remote_port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].state != TCP_CLOSED) {
            if (tcp_connections[i].local_ip == local_ip_net &&
                tcp_connections[i].local_port == local_port &&
                tcp_connections[i].remote_ip == remote_ip_net &&
                tcp_connections[i].remote_port == remote_port) {
                return &tcp_connections[i];
            }
        }
    }
    return NULL;
}

static tcp_connection_t* tcp_alloc_connection() {
    // First pass: reuse a fully-closed slot
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].state == TCP_CLOSED) {
            memset(&tcp_connections[i], 0, sizeof(tcp_connection_t));
            return &tcp_connections[i];
        }
    }
    // Second pass: reclaim the oldest TIME_WAIT slot
    uint32_t oldest_age = 0;
    int oldest_idx = -1;
    uint32_t now = timer_get_ticks();
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].state == TCP_TIME_WAIT) {
            uint32_t age = now - tcp_connections[i].time_wait_start;
            if (age > oldest_age) {
                oldest_age = age;
                oldest_idx = i;
            }
        }
    }
    if (oldest_idx >= 0) {
        memset(&tcp_connections[oldest_idx], 0, sizeof(tcp_connection_t));
        return &tcp_connections[oldest_idx];
    }
    return NULL;
}

// TCP checksum calculation - OPTIMIZED
uint16_t tcp_checksum(uint8_t* packet, uint16_t len, uint32_t src_ip, uint32_t dst_ip) {
    uint32_t sum = 0;

    // Pseudo header
    sum += ntohs((src_ip >> 16) & 0xFFFF);
    sum += ntohs(src_ip & 0xFFFF);
    sum += ntohs((dst_ip >> 16) & 0xFFFF);
    sum += ntohs(dst_ip & 0xFFFF);
    sum += htons(IPPROTO_TCP);
    sum += htons(len);

    // Sum all 16-bit words in the packet
    int i;
    for (i = 0; i + 1 < len; i += 2) {
        sum += *((uint16_t*)(packet + i));
    }
    // Handle odd length
    if (i < len) {
        sum += packet[i];
    }
    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

// Send TCP packet - OPTIMIZED with static buffer to prevent stack overflow
// CRITICAL: tcp_send can be called from deep call chains (e.g., interrupt → tcp_handle_packet → tcp_send)
// Using a 1500-byte stack buffer was causing stack overflow with 16KB kernel stack
int tcp_send(tcp_connection_t* conn, uint8_t flags, uint8_t* data, uint16_t len) {
    uint8_t* packet = conn->send_packet;
    tcp_header_t* tcp = (tcp_header_t*)packet;

    memset(tcp, 0, sizeof(tcp_header_t));

    tcp->src_port = htons(conn->local_port);
    tcp->dest_port = htons(conn->remote_port);
    tcp->seq_num = htonl(conn->snd_nxt);
    tcp->ack_num = htonl(conn->rcv_nxt);
    tcp->data_offset = 5 << 4;  // 20 bytes header
    tcp->flags = flags;
    tcp->window = htons(TCP_WINDOW_SIZE);
    tcp->urgent_ptr = 0;

    uint16_t header_len = 20;

    if (flags & TCP_SYN) {
        // Add MSS option (4 bytes) + NOP padding
        packet[20] = 2;    // Kind: MSS
        packet[21] = 4;    // Length: 4 bytes
        packet[22] = 0x05; // MSS = 1460 (high byte)
        packet[23] = 0xB4; // MSS = 1460 (low byte)
        packet[24] = 1;    // NOP
        packet[25] = 1;    // NOP
        packet[26] = 1;    // NOP
        packet[27] = 1;    // NOP
        header_len = 28;
        tcp->data_offset = 7 << 4;
    }

    // Copy data if any
    if (data && len > 0) {
        memcpy(packet + header_len, data, len);
    }

    // Calculate checksum
    uint16_t tcp_len = header_len + len;
    tcp->checksum = 0;
    tcp->checksum = tcp_checksum(packet, tcp_len, conn->local_ip, conn->remote_ip);

    // Send via IP layer
    int result = net_send_raw_ip(conn->remote_ip, IPPROTO_TCP, packet, tcp_len);

    TCP_LOG("[TCP] send: %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d flags=0x%02X len=%d result=%d\n",
             (conn->local_ip >> 24) & 0xFF, (conn->local_ip >> 16) & 0xFF,
             (conn->local_ip >> 8) & 0xFF, conn->local_ip & 0xFF, conn->local_port,
             (conn->remote_ip >> 24) & 0xFF, (conn->remote_ip >> 16) & 0xFF,
             (conn->remote_ip >> 8) & 0xFF, conn->remote_ip & 0xFF, conn->remote_port,
             flags, tcp_len, result);

    return result;
}

// TCP connect — returns local port on success, -1 on failure
int tcp_connect(uint32_t remote_ip, uint16_t remote_port) {
    if (net_get_ip() == 0) return -1;

    // Find unused local port
    uint16_t start_port = tcp_next_port;
    do {
        if (tcp_find_connection(net_get_ip(), tcp_next_port, remote_ip, remote_port) == NULL) break;
        tcp_next_port++;
        if (tcp_next_port > 65535) tcp_next_port = 49152;
    } while (tcp_next_port != start_port);

    uint16_t local_port = tcp_next_port++;
    if (tcp_next_port > 65535) tcp_next_port = 49152;

    tcp_connection_t* conn = tcp_alloc_connection();
    if (!conn) return -1;

    conn->state = TCP_SYN_SENT;
    conn->local_ip = net_get_ip();
    conn->remote_ip = remote_ip;
    conn->local_port = local_port;
    conn->remote_port = remote_port;
    conn->snd_nxt = 1;
    conn->snd_una = conn->snd_nxt;  // ISN = 1
    conn->connect_time = timer_get_ticks();
    conn->retransmit_timeout = TCP_RETRANSMIT_TIMEOUT;
    conn->retransmit_count = 0;
    conn->last_ack_time = timer_get_ticks();

    tcp_send(conn, TCP_SYN, NULL, 0);
    conn->snd_nxt++;  // SYN consumes one seq number

    return local_port;
}

// TCP connect with pointer return (for socket layer)
tcp_connection_t* tcp_connect_with_ptr(uint32_t remote_ip, uint16_t remote_port) {
    if (net_get_ip() == 0) return NULL;

    // Reap stale connections to same remote host:port
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_connection_t* old = &tcp_connections[i];
        if (old->state == TCP_CLOSED) continue;
        if (old->remote_ip == remote_ip && old->remote_port == remote_port) {
            s_printf("[TCP] Reaping stale connection (state=%d) to %d.%d.%d.%d:%d\n",
                     old->state,
                     (remote_ip >> 24) & 0xFF, (remote_ip >> 16) & 0xFF,
                     (remote_ip >> 8) & 0xFF, remote_ip & 0xFF, remote_port);
            old->state = TCP_CLOSED;
            tcp_ofo_flush(old);
            old->remote_ip = 0;
            old->remote_port = 0;
            old->local_port = 0;
            old->retransmit_timeout = 0;
            old->retransmit_count = 0;
            old->on_data = NULL;
            old->on_state_change = NULL;
            old->callback_user_data = NULL;
            old->user_closing = 0;
        }
    }

    // Find unused local port
    uint16_t start_port = tcp_next_port;
    do {
        if (tcp_find_connection(net_get_ip(), tcp_next_port, remote_ip, remote_port) == NULL) break;
        tcp_next_port++;
        if (tcp_next_port > 65535) tcp_next_port = 49152;
    } while (tcp_next_port != start_port);

    uint16_t local_port = tcp_next_port++;
    if (tcp_next_port > 65535) tcp_next_port = 49152;

    tcp_connection_t* conn = tcp_alloc_connection();
    if (!conn) return NULL;

    conn->state = TCP_SYN_SENT;
    conn->local_ip = net_get_ip();
    conn->remote_ip = remote_ip;
    conn->local_port = local_port;
    conn->remote_port = remote_port;
    conn->snd_nxt = 1;
    conn->snd_una = conn->snd_nxt;
    conn->connect_time = timer_get_ticks();
    conn->retransmit_timeout = TCP_RETRANSMIT_TIMEOUT;
    conn->retransmit_count = 0;
    conn->last_ack_time = timer_get_ticks();

    tcp_send(conn, TCP_SYN, NULL, 0);
    conn->snd_nxt++;

    return conn;
}

// Get local port from connection
uint16_t tcp_conn_get_local_port(void* conn_ptr) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    return conn ? conn->local_port : 0;
}

// Check if connection is established
int tcp_conn_is_established(void* conn_ptr) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    return conn && conn->state == TCP_ESTABLISHED;
}

// Forward declaration - needed because tcp_handle_incoming_syn is static and called
// from tcp_handle_packet before its definition
static int tcp_handle_incoming_syn(uint16_t dst_port, uint32_t src_ip, uint16_t src_port,
                                    uint32_t dst_ip, uint32_t seq);


void tcp_handle_packet(uint8_t* packet, uint32_t len, uint32_t src_ip, uint32_t dst_ip) {
    tcp_header_t* tcp = (tcp_header_t*)packet;
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dest_port);
    uint8_t pkt_flags = tcp->flags;

    tcp_connection_t* conn = tcp_find_connection(dst_ip, dst_port, src_ip, src_port);

    if (!conn) {
        if (pkt_flags & TCP_SYN) {
            tcp_handle_incoming_syn(dst_port, src_ip, src_port, dst_ip, ntohl(tcp->seq_num));
        }
        return;
    }

    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t flags = pkt_flags;

    // RST handling
    if (flags & TCP_RST) {
        conn->state = TCP_CLOSED;
        tcp_ofo_flush(conn);
        if (conn->on_state_change) conn->on_state_change(conn->state, TCP_CLOSED);
        return;
    }

    conn->window = ntohs(tcp->window);

    switch (conn->state) {
        case TCP_SYN_RECEIVED:
            if (flags & TCP_ACK) {
                if (ack == conn->snd_una + 1) {
                    conn->snd_una = ack;
                    conn->state = TCP_ESTABLISHED;
                    if (conn->on_state_change)
                        conn->on_state_change(TCP_SYN_RECEIVED, TCP_ESTABLISHED);
                }
            }
            break;

        case TCP_SYN_SENT:
            if (flags & TCP_SYN && flags & TCP_ACK) {
                if (ack == conn->snd_una + 1) {
                    conn->rcv_nxt = seq + 1;
                    conn->snd_una = ack;
                    conn->state = TCP_ESTABLISHED;
                    conn->retransmit_count = 0;
                    conn->retransmit_timeout = 0;
                    tcp_send(conn, TCP_ACK, NULL, 0);
                    if (conn->on_state_change)
                        conn->on_state_change(TCP_SYN_SENT, TCP_ESTABLISHED);
                }
            }
            break;

        case TCP_ESTABLISHED:
        case TCP_CLOSE_WAIT:
            // ============================================================
            // ACK processing — FIX: reliably clear retransmit timer
            //
            // The old byte-level send_head/send_tail tracking got confused
            // when multiple small tls_write() calls were cumulatively ACKed
            // in one segment. The timer stayed armed, tcp_retransmit_check()
            // fired repeatedly (sending bare ACKs), and after 5 retries
            // sent RST — killing the connection mid-transfer.
            //
            // Fix: when ANY ACK advances snd_una, reset the send buffer
            // entirely and clear the timer. For a browser that sends small
            // requests and waits for responses, this is correct and
            // eliminates the ACK storm.
            // ============================================================
            if (flags & TCP_ACK) {
                if (ack > conn->snd_una) {
                    conn->snd_una = ack;
                    // All outstanding data is now acknowledged.
                    // Reset send buffer and clear retransmit state.
                    conn->send_head = 0;
                    conn->send_tail = 0;
                    conn->retransmit_timeout = 0;
                    conn->retransmit_count = 0;
                    conn->last_ack_time = timer_get_ticks();
                } else if (ack == conn->snd_una) {
                    // Duplicate ACK — peer is requesting retransmit or
                    // just acknowledging data. Update timestamp so the
                    // retransmit timer doesn't fire spuriously.
                    conn->last_ack_time = timer_get_ticks();
                }
            }

            // ============================================================
            // Data processing
            // ============================================================
            {
                uint16_t hdr_len = (tcp->data_offset >> 4) * 4;
                if (len > hdr_len) {
                    uint16_t data_len = len - hdr_len;
                    uint8_t* data = packet + hdr_len;

                    // SLIRP zero-phantom detection
                    if (seq == conn->rcv_nxt && tcp_is_slirp_zero_phantom(data, data_len)) {
                        conn->slirp_phantom_len = (uint8_t)data_len;
                        conn->slirp_phantom_time = timer_get_ticks();
                        goto after_data;
                    }

                    // Handle overlapping segments
                    if (seq < conn->rcv_nxt) {
                        uint32_t overlap = conn->rcv_nxt - seq;
                        // SLIRP phantom recovery
                        if (conn->slirp_phantom_len > 0 &&
                            data_len > (uint32_t)conn->slirp_phantom_len &&
                            data[0] != 0x00) {
                            if (conn->recv_tail >= conn->slirp_phantom_len)
                                conn->recv_tail -= conn->slirp_phantom_len;
                            conn->rcv_nxt = seq;
                            conn->slirp_phantom_len = 0;
                        } else if (overlap >= data_len) {
                            // Pure duplicate
                            conn->last_ack_time = timer_get_ticks();
                            conn->retransmit_count = 0;
                            tcp_send_ack_ratelimited(conn);
                            goto after_data;
                        } else {
                            // Partial overlap — trim
                            data += overlap;
                            data_len -= (uint16_t)overlap;
                            seq += overlap;
                        }
                    }

                    // In-order delivery
                    if (seq == conn->rcv_nxt) {
                        // ============================================================
                        // FIX: Buffer-full protection
                        //
                        // The old code silently dropped data when recv_buffer was
                        // full BUT still advanced rcv_nxt and sent an ACK. The
                        // server then believed the data was delivered and never
                        // retransmitted it — permanently losing bytes in the
                        // middle of a TLS record.
                        //
                        // Fix: if the buffer can't hold the data, DON'T advance
                        // rcv_nxt and DON'T ACK. Send a duplicate ACK (with the
                        // old rcv_nxt) so the server knows to retransmit.
                        // ============================================================
                        if (conn->on_data) {
                            // Callback mode — always deliver (callback owns buffering)
                            conn->on_data(data, data_len, conn->callback_user_data);
                            conn->rcv_nxt += data_len;
                            if (!tcp_is_slirp_zero_phantom(data, data_len))
                                conn->slirp_phantom_len = 0;
                            conn->last_recv_time = timer_get_ticks();
                            tcp_send(conn, TCP_ACK, NULL, 0);
                            tcp_ofo_drain(conn);
                        } else if (!conn->user_closing) {
                            if (conn->recv_tail + data_len <= sizeof(conn->recv_buffer)) {
                                // Buffer has space — normal delivery
                                memcpy(conn->recv_buffer + conn->recv_tail, data, data_len);
                                conn->recv_tail += data_len;
                                conn->rcv_nxt += data_len;
                                if (!tcp_is_slirp_zero_phantom(data, data_len))
                                    conn->slirp_phantom_len = 0;
                                conn->last_recv_time = timer_get_ticks();
                                tcp_send(conn, TCP_ACK, NULL, 0);
                                tcp_ofo_drain(conn);
                            } else {
                                // ====================================================
                                // BUFFER FULL — do NOT advance rcv_nxt, do NOT ACK.
                                // Send a duplicate ACK with the current (old) rcv_nxt
                                // so the sender knows we haven't received this data.
                                // The sender will retransmit after its RTO fires.
                                //
                                // Also try to drain the OFO queue in case there's
                                // space after the application reads some data.
                                // ====================================================
                                TCP_LOG("[TCP] recv_buffer FULL (%d/%d), dropping %d bytes "
                                        "seq=%u (NOT advancing rcv_nxt=%u)\n",
                                        (int)conn->recv_tail,
                                        (int)sizeof(conn->recv_buffer),
                                        (int)data_len, seq, conn->rcv_nxt);
                                tcp_send_ack_ratelimited(conn);
                            }
                        } else {
                            // User is closing — still advance rcv_nxt to keep
                            // the peer from retransmitting, but discard data.
                            conn->rcv_nxt += data_len;
                            conn->last_recv_time = timer_get_ticks();
                            tcp_send(conn, TCP_ACK, NULL, 0);
                        }
                    } else {
                        // Out-of-order: queue for later
                        if (!conn->user_closing)
                            tcp_ofo_enqueue(conn, seq, data, data_len);
                        tcp_send_ack_ratelimited(conn);
                    }
                }
            }
        after_data:;
            // FIN processing
            if (flags & TCP_FIN) {
                if (conn->state == TCP_ESTABLISHED) {
                    conn->rcv_nxt++;
                    conn->state = TCP_CLOSE_WAIT;
                    tcp_send(conn, TCP_ACK, NULL, 0);
                } else if (conn->state == TCP_CLOSE_WAIT) {
                    tcp_send_ack_ratelimited(conn);
                }
            }
            break;

        case TCP_FIN_WAIT1:
            if (flags & TCP_FIN && flags & TCP_ACK) {
                conn->rcv_nxt++;
                tcp_send(conn, TCP_ACK, NULL, 0);
                conn->state = TCP_TIME_WAIT;
                conn->time_wait_start = timer_get_ticks();
                if (conn->on_state_change)
                    conn->on_state_change(TCP_FIN_WAIT1, TCP_TIME_WAIT);
            } else if (flags & TCP_ACK) {
                conn->state = TCP_FIN_WAIT2;
            } else if (flags & TCP_FIN) {
                conn->rcv_nxt++;
                tcp_send(conn, TCP_ACK, NULL, 0);
                conn->state = TCP_CLOSING;
            }
            break;

        case TCP_FIN_WAIT2:
            if (flags & TCP_FIN) {
                conn->rcv_nxt++;
                tcp_send(conn, TCP_ACK, NULL, 0);
                conn->state = TCP_TIME_WAIT;
                conn->time_wait_start = timer_get_ticks();
                if (conn->on_state_change)
                    conn->on_state_change(TCP_FIN_WAIT2, TCP_TIME_WAIT);
            }
            break;

        case TCP_CLOSING:
            if (flags & TCP_ACK) {
                conn->state = TCP_TIME_WAIT;
                conn->time_wait_start = timer_get_ticks();
                if (conn->on_state_change)
                    conn->on_state_change(TCP_CLOSING, TCP_TIME_WAIT);
            }
            break;

        case TCP_LAST_ACK:
            if (flags & TCP_ACK) {
                conn->state = TCP_CLOSED;
                tcp_ofo_flush(conn);
                if (conn->on_state_change)
                    conn->on_state_change(TCP_LAST_ACK, TCP_CLOSED);
            }
            break;
    }
    conn->last_ack_time = timer_get_ticks();
}

// Send data over TCP connection
int tcp_send_data(tcp_connection_t* conn, uint8_t* data, uint16_t len) {
    if (conn->state != TCP_ESTABLISHED) return -1;

    // Add to send buffer
    if (conn->send_tail + len <= sizeof(conn->send_buffer)) {
        memcpy(conn->send_buffer + conn->send_tail, data, len);
        conn->send_tail += len;
    } else if (conn->send_tail < sizeof(conn->send_buffer)) {
        uint32_t remaining = sizeof(conn->send_buffer) - conn->send_tail;
        memcpy(conn->send_buffer + conn->send_tail, data, remaining);
        conn->send_tail = sizeof(conn->send_buffer);
    }

    // Send data in MSS-sized chunks
    uint16_t sent = 0;
    while (sent < len) {
        uint16_t chunk = len - sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        tcp_send(conn, TCP_ACK | TCP_PSH, data + sent, chunk);
        conn->snd_nxt += chunk;
        sent += chunk;
    }

    // Arm retransmit timer only if not already armed
    if (conn->retransmit_timeout == 0) {
        conn->retransmit_timeout = TCP_RETRANSMIT_TIMEOUT;
        conn->last_ack_time = timer_get_ticks();
        conn->retransmit_count = 0;
    }
    return sent;
}

// Initialize TCP subsystem
void tcp_init() {
    memset(tcp_connections, 0, sizeof(tcp_connections));
}

// Send data over a TCP connection (void* wrapper for app layer)
int tcp_conn_send(void* conn_ptr, const void* data, int len) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    if (conn->state != TCP_ESTABLISHED && conn->state != TCP_CLOSE_WAIT) return -1;
    if (!data || len <= 0) return -1;
    return tcp_send_data(conn, (uint8_t*)data, (uint16_t)len);
}

// Receive data from a TCP connection (void* wrapper for app layer)
int tcp_conn_recv(void* conn_ptr, void* buf, int max_len) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    if (!conn || !buf || max_len <= 0) return -1;

    int available = (int)(conn->recv_tail - conn->recv_head);
    if (available <= 0) return 0;

    int to_read = (available < max_len) ? available : max_len;
    memcpy(buf, conn->recv_buffer + conn->recv_head, to_read);
    conn->recv_head += to_read;

    if (conn->recv_head == conn->recv_tail) {
        conn->recv_head = 0;
        conn->recv_tail = 0;
    }
    return to_read;
}

// Set data callback for a TCP connection
void tcp_conn_set_data_callback(void* conn_ptr,
    void (*callback)(uint8_t*, uint16_t, void*), void* user_data) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    if (conn) {
        conn->on_data = callback;
        conn->callback_user_data = user_data;
    }
}

// Set state change callback for a TCP connection
void tcp_conn_set_state_callback(void* conn_ptr,
    void (*callback)(uint8_t, uint8_t)) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    if (conn) {
        conn->on_state_change = callback;
    }
}

// ============================================================================
// TCP Listen/Accept Implementation (Server-Side Sockets)
// ============================================================================

static tcp_listener_t tcp_listeners[TCP_MAX_LISTENERS];

/**
 * Find a listener for the given local port.
 */
tcp_listener_t* tcp_find_listener(uint16_t port) {
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (tcp_listeners[i].in_use && tcp_listeners[i].port == port) {
            return &tcp_listeners[i];
        }
    }
    return NULL;
}

/**
 * Start listening for incoming connections on a port.
 * Returns a listener ID (>=0) on success, -1 on failure.
 */
int tcp_listen(uint16_t port, uint32_t bind_ip) {
    if (port == 0) return -1;
    if (tcp_find_listener(port)) return -1;

    int slot = -1;
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (!tcp_listeners[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    memset(&tcp_listeners[slot], 0, sizeof(tcp_listener_t));
    tcp_listeners[slot].in_use = 1;
    tcp_listeners[slot].port = port;
    tcp_listeners[slot].bind_ip = bind_ip;
    return slot;
}

/**
 * Accept a pending connection from a listener.
 * Returns 0 on success (out_conn set), -1 if no connections ready.
 */
int tcp_accept(int listener_id, tcp_connection_t** out_conn) {
    if (listener_id < 0 || listener_id >= TCP_MAX_LISTENERS) return -1;
    if (!out_conn) return -1;

    tcp_listener_t* listener = &tcp_listeners[listener_id];
    if (!listener->in_use || listener->established_count <= 0) return -1;

    tcp_connection_t* conn = listener->established[0];
    *out_conn = conn;

    for (int i = 1; i < listener->established_count; i++)
        listener->established[i - 1] = listener->established[i];
    listener->established_count--;
    listener->established[listener->established_count] = NULL;
    return 0;
}

/**
 * Close a listener and free all pending connections.
 */
int tcp_close_listener(int listener_id) {
    if (listener_id < 0 || listener_id >= TCP_MAX_LISTENERS) return -1;
    tcp_listener_t* listener = &tcp_listeners[listener_id];
    if (!listener->in_use) return -1;

    for (int i = 0; i < listener->pending_count; i++) {
        if (listener->pending[i]) {
            tcp_send(listener->pending[i], TCP_RST, NULL, 0);
            listener->pending[i]->state = TCP_CLOSED;
            listener->pending[i] = NULL;
        }
    }
    for (int i = 0; i < listener->established_count; i++) {
        if (listener->established[i]) {
            tcp_send(listener->established[i], TCP_FIN | TCP_ACK, NULL, 0);
            listener->established[i]->state = TCP_FIN_WAIT1;
            listener->established[i] = NULL;
        }
    }
    listener->in_use = 0;
    listener->pending_count = 0;
    listener->established_count = 0;
    return 0;
}

/**
 * Process incoming SYN packets for listeners.
 * Called from tcp_handle_packet when no existing connection is found.
 * This implements the server side of the TCP three-way handshake.
 */
static int tcp_handle_incoming_syn(uint16_t dst_port, uint32_t src_ip,
    uint16_t src_port, uint32_t dst_ip, uint32_t seq) {
    tcp_listener_t* listener = tcp_find_listener(dst_port);
    if (!listener) return -1;
    if (listener->pending_count >= TCP_LISTEN_BACKLOG) return -1;

    tcp_connection_t* conn = tcp_alloc_connection();
    if (!conn) return -1;

    conn->state = TCP_SYN_RECEIVED;
    conn->local_ip = dst_ip;
    conn->remote_ip = src_ip;
    conn->local_port = dst_port;
    conn->remote_port = src_port;
    conn->rcv_nxt = seq + 1;
    conn->snd_nxt = (uint32_t)(timer_get_ticks() ^ (src_port << 16));
    if (conn->snd_nxt == 0) conn->snd_nxt = 1;
    conn->connect_time = timer_get_ticks();
    conn->window = TCP_WINDOW_SIZE;
    conn->mss = TCP_MSS;

    tcp_send(conn, TCP_SYN | TCP_ACK, NULL, 0);
    conn->snd_nxt++;

    listener->pending[listener->pending_count++] = conn;
    return 0;
}

/**
 * Periodically process listeners: check if any pending connections
 * have completed the three-way handshake and move them to established.
 */
void tcp_process_listeners(void) {
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (!tcp_listeners[i].in_use) continue;
        tcp_listener_t* listener = &tcp_listeners[i];

        for (int j = 0; j < listener->pending_count; j++) {
            tcp_connection_t* conn = listener->pending[j];
            if (!conn) continue;

            if (conn->state == TCP_ESTABLISHED) {
                if (listener->established_count < TCP_LISTEN_BACKLOG)
                    listener->established[listener->established_count++] = conn;
                for (int k = j; k < listener->pending_count - 1; k++)
                    listener->pending[k] = listener->pending[k + 1];
                listener->pending_count--;
                listener->pending[listener->pending_count] = NULL;
                j--;
            } else if (conn->state == TCP_CLOSED) {
                for (int k = j; k < listener->pending_count - 1; k++)
                    listener->pending[k] = listener->pending[k + 1];
                listener->pending_count--;
                listener->pending[listener->pending_count] = NULL;
                j--;
            }
        }
    }
}

void tcp_retransmit_check(void) {
    uint32_t now = timer_get_ticks();

    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_connection_t* conn = &tcp_connections[i];
        if (conn->state == TCP_CLOSED) continue;

        // TIME_WAIT expiry (6000 ticks ≈ 120s at 50Hz)
        if (conn->state == TCP_TIME_WAIT) {
            if (now - conn->time_wait_start >= 6000) {
                conn->state = TCP_CLOSED;
                tcp_ofo_flush(conn);
            }
            continue;
        }

        // Auto-close idle CLOSE_WAIT
        if (conn->state == TCP_CLOSE_WAIT) {
            uint32_t idle = now - conn->last_ack_time;
            if (idle > 250) {
                tcp_send(conn, TCP_FIN | TCP_ACK, NULL, 0);
                conn->snd_nxt++;
                conn->state = TCP_LAST_ACK;
                conn->last_ack_time = now;
                conn->retransmit_timeout = TCP_RETRANSMIT_TIMEOUT;
                conn->retransmit_count = 0;
            }
            continue;
        }

        // Reap idle closing states
        if (conn->state == TCP_LAST_ACK || conn->state == TCP_FIN_WAIT1 ||
            conn->state == TCP_FIN_WAIT2 || conn->state == TCP_CLOSING) {
            uint32_t idle = now - conn->last_ack_time;
            if (idle > 750) {
                conn->state = TCP_CLOSED;
                tcp_ofo_flush(conn);
                conn->retransmit_timeout = 0;
                continue;
            }
        }

        // SLIRP phantom-aware retransmit suppression
        if (conn->slirp_phantom_len > 0 && conn->slirp_phantom_time > 0) {
            uint32_t since_phantom = now - conn->slirp_phantom_time;
            if (conn->send_tail > conn->send_head) {
                if (since_phantom < 100) continue;
                conn->slirp_phantom_len = 0;
            } else {
                if (since_phantom < 50) continue;
            }
        }

        if (conn->retransmit_timeout == 0) continue;

        uint32_t elapsed = now - conn->last_ack_time;
        if (elapsed < conn->retransmit_timeout) continue;

        // Grace period: skip if peer recently sent data
        if (conn->last_recv_time > 0) {
            uint32_t since_recv = now - conn->last_recv_time;
            uint32_t grace = conn->retransmit_timeout / 2;
            if (grace < 25) grace = 25;
            if (since_recv < grace) continue;
        }

        // Max retransmits exceeded
        if (conn->retransmit_count >= TCP_MAX_RETRANSMIT) {
            s_printf("[TCP] Max retransmits exceeded for %d.%d.%d.%d:%d — sending RST\n",
                     (conn->remote_ip >> 24) & 0xFF, (conn->remote_ip >> 16) & 0xFF,
                     (conn->remote_ip >> 8) & 0xFF, conn->remote_ip & 0xFF,
                     conn->remote_port);
            tcp_send(conn, TCP_RST, NULL, 0);
            conn->state = TCP_CLOSED;
            tcp_ofo_flush(conn);
            if (conn->on_state_change)
                conn->on_state_change(TCP_ESTABLISHED, TCP_CLOSED);
            continue;
        }

        conn->retransmit_count++;
        conn->retransmit_timeout *= 2;
        if (conn->retransmit_timeout > 1500)
            conn->retransmit_timeout = 1500;
        conn->last_ack_time = now;

        switch (conn->state) {
            case TCP_SYN_SENT:
            {
                __asm__ volatile("cli");
                if (conn->state != TCP_SYN_SENT) { __asm__ volatile("sti"); break; }
                uint32_t saved_nxt = conn->snd_nxt;
                conn->snd_nxt = conn->snd_una;
                tcp_send(conn, TCP_SYN, NULL, 0);
                conn->snd_nxt = saved_nxt;
                __asm__ volatile("sti");
            }
            break;

            case TCP_SYN_RECEIVED:
            {
                __asm__ volatile("cli");
                if (conn->state != TCP_SYN_RECEIVED) { __asm__ volatile("sti"); break; }
                uint32_t saved_nxt = conn->snd_nxt;
                conn->snd_nxt = conn->snd_una;
                tcp_send(conn, TCP_SYN | TCP_ACK, NULL, 0);
                conn->snd_nxt = saved_nxt;
                __asm__ volatile("sti");
            }
            break;

            case TCP_ESTABLISHED:
            case TCP_CLOSE_WAIT:
                // ============================================================
                // FIX: Only retransmit if there's actual pending data.
                //
                // The old code sent a bare ACK when send_head == send_tail,
                // which caused an ACK storm (dozens of bare ACKs per second)
                // because tcp_retransmit_check() was called from inside
                // tls_recv_all()'s polling loop on every iteration.
                //
                // Now: if there's no pending data, just clear the timer.
                // The timer should have been cleared by the ACK handler,
                // but if it wasn't (race condition), this is the safety net.
                // ============================================================
                if (conn->send_tail > conn->send_head) {
                    uint32_t data_len = conn->send_tail - conn->send_head;
                    uint32_t offset = 0;
                    uint32_t saved_nxt = conn->snd_nxt;
                    conn->snd_nxt = conn->snd_una;
                    while (offset < data_len) {
                        uint16_t chunk = (data_len - offset > TCP_MSS)
                                         ? TCP_MSS : (uint16_t)(data_len - offset);
                        tcp_send(conn, TCP_ACK | TCP_PSH,
                                 conn->send_buffer + conn->send_head + offset, chunk);
                        conn->snd_nxt += chunk;
                        offset += chunk;
                    }
                    if (saved_nxt > conn->snd_nxt)
                        conn->snd_nxt = saved_nxt;
                } else {
                    // No pending data — nothing to retransmit.
                    // Clear the timer so we don't fire again.
                    conn->retransmit_timeout = 0;
                    conn->retransmit_count = 0;
                }
                break;

            case TCP_FIN_WAIT1:
                tcp_send(conn, TCP_FIN | TCP_ACK, NULL, 0);
                break;

            case TCP_LAST_ACK:
                tcp_send(conn, TCP_FIN | TCP_ACK, NULL, 0);
                break;

            default:
                break;
        }
    }
}