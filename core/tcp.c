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
#define TCP_DEBUG_PACKETS     0    // Log packet details
#define TCP_DEBUG_ERRORS      0    // Log errors (set to 0 for production)

#define TCP_MAX_CONNECTIONS 32
#define TCP_WINDOW_SIZE 16384 // Increased to 16KB
#define TCP_MSS 1460
// TCP_RETRANSMIT_TIMEOUT is now defined in tcp.h so callers outside tcp.c
// (e.g. k_close in socket.c) can arm FIN retransmit timers.
#define TCP_MAX_RETRANSMIT     5     // Max retransmit attempts before giving up


static tcp_connection_t tcp_connections[TCP_MAX_CONNECTIONS];
static uint16_t tcp_next_port = 49152; // Start of ephemeral ports

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

// Forward declaration — tcp_send is defined below (around line 250+), but
// the OFO helpers need to send ACKs when draining queued segments.
int tcp_send(tcp_connection_t* conn, uint8_t flags, uint8_t* data, uint16_t len);

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
    if (len > TCP_OFO_SEG_MAX) len = TCP_OFO_SEG_MAX;  // truncate (shouldn't happen for in-window)

    // Check for full containment in either direction.
    for (int i = 0; i < TCP_OFO_QUEUE_SIZE; i++) {
        if (!conn->ofo_queue[i].in_use) continue;
        tcp_ofo_entry_t* e = &conn->ofo_queue[i];
        uint32_t e_end = e->seq + e->len;       // exclusive
        uint32_t n_end = seq + len;             // exclusive
        // Existing fully contains new → drop new.
        if (e->seq <= seq && e_end >= n_end) return 0;
        // New fully contains existing → mark existing free, fall through to insert.
        if (seq <= e->seq && n_end >= e_end) {
            e->in_use = 0;
            e->len = 0;
        }
    }

    int slot = tcp_ofo_find_free(conn);
    if (slot < 0) {
        s_printf("[TCP] OFO queue full — dropping segment seq=%u len=%u\n", seq, len);
        return -1;
    }
    tcp_ofo_entry_t* e = &conn->ofo_queue[slot];
    e->in_use = 1;
    e->seq = seq;
    e->len = len;
    memcpy(e->data, data, len);
    s_printf("[TCP] OFO queued: seq=%u len=%u (slot=%d)\n", seq, len, slot);
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

            // Trim any leading bytes that have already been received
            // (e.g. the live segment that just arrived filled in earlier
            // bytes that this queued segment also covers).
            if (e->seq < conn->rcv_nxt) {
                uint32_t overlap = conn->rcv_nxt - e->seq;
                if (overlap >= e->len) {
                    // Entirely old — discard.
                    e->in_use = 0;
                    e->len = 0;
                    continue;
                }
                // Shift the data buffer in place.
                uint16_t keep = e->len - (uint16_t)overlap;
                memmove(e->data, e->data + overlap, keep);
                e->len = keep;
                e->seq = conn->rcv_nxt;
            }

            if (e->seq == conn->rcv_nxt) {
                s_printf("[TCP] OFO delivering: seq=%u len=%u\n", e->seq, e->len);
                if (conn->on_data) {
                    conn->on_data(e->data, e->len, conn->callback_user_data);
                } else if (!conn->user_closing) {
                    // No callback but app still wants the data — flat buffer.
                    if (conn->recv_tail + e->len <= sizeof(conn->recv_buffer)) {
                        memcpy(conn->recv_buffer + conn->recv_tail, e->data, e->len);
                        conn->recv_tail += e->len;
                    }
                }
                // If user_closing is set, we silently ACK and discard —
                // keeps the peer's FSM moving toward CLOSED without
                // polluting the orphaned recv_buffer.
                conn->rcv_nxt += e->len;
                tcp_send(conn, TCP_ACK, NULL, 0);
                e->in_use = 0;
                e->len = 0;
                progress = 1;
            }
            // else: still a gap before this segment — leave it queued.
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
    // First pass: reuse a fully-closed slot.
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].state == TCP_CLOSED) {
            memset(&tcp_connections[i], 0, sizeof(tcp_connection_t));
            return &tcp_connections[i];
        }
    }

    // Second pass: reclaim the oldest TIME_WAIT slot. Previously this
    // returned NULL once all 32 slots were stuck in TIME_WAIT (which
    // happens after ~32 navigations, since TIME_WAIT lasts 60s and the
    // retransmit check that reaps expired entries only runs from the
    // timer IRQ — see hal/cpu/timer.c). The browser then failed every
    // subsequent navigation with "Connection Error" until reboot.
    // Forcibly closing the oldest TIME_WAIT entry is safe: by spec the
    // connection is already fully closed, TIME_WAIT only exists to catch
    // stray delayed segments from the peer.
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
    tcp->data_offset = 5 << 4;  // 5 * 4 = 20 bytes header
    tcp->flags = flags;
    tcp->window = htons(TCP_WINDOW_SIZE);
    tcp->urgent_ptr = 0;

    uint16_t header_len = 20;  // Base header length in bytes
    
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
    s_printf("[TCP] send: %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d flags=0x%02X len=%d result=%d\n",
             (conn->local_ip >> 24) & 0xFF, (conn->local_ip >> 16) & 0xFF, (conn->local_ip >> 8) & 0xFF, conn->local_ip & 0xFF, conn->local_port,
             (conn->remote_ip >> 24) & 0xFF, (conn->remote_ip >> 16) & 0xFF, (conn->remote_ip >> 8) & 0xFF, conn->remote_ip & 0xFF, conn->remote_port,
             flags, tcp_len, result);

    // For SYN packets, dump the full packet hex so we can verify the
    // checksum, sequence number, MSS option, etc. are correct.
    if (flags & TCP_SYN) {
        s_printf("[TCP] SYN packet hex (%d bytes):\n", tcp_len);
        for (int i = 0; i < tcp_len; i++) {
            s_printf("%02X ", packet[i]);
            if ((i + 1) % 16 == 0) s_printf("\n");
        }
        s_printf("\n");
        s_printf("[TCP] SYN: seq=%u ack=%u wnd=%u checksum=0x%04X\n",
                 conn->snd_nxt, conn->rcv_nxt, TCP_WINDOW_SIZE, tcp->checksum);
    }
    return result;
}

// TCP connection establishment
int tcp_connect(uint32_t remote_ip, uint16_t remote_port) {
    // FIX: Reject connection attempts before the network interface is configured.
    // Previously, calling tcp_connect before DHCP/static config would set
    // local_ip = 0, causing the SYN to be silently dropped by net_send_raw_ip.
    // The connection would then sit in SYN_SENT for ~30 seconds before timing out.
    if (net_get_ip() == 0) {
        return -1;
    }

    // Find unused local port with collision check
    uint16_t start_port = tcp_next_port;
    do {
        if (tcp_find_connection(net_get_ip(), tcp_next_port, remote_ip, remote_port) == NULL) break;
        tcp_next_port++;
        if (tcp_next_port > 65535) tcp_next_port = 49152;
    } while (tcp_next_port != start_port);
    uint16_t local_port = tcp_next_port++;
    if (tcp_next_port > 65535) tcp_next_port = 49152;

    // Allocate connection
    tcp_connection_t* conn = tcp_alloc_connection();
    if (!conn) {
        return -1;
    }

    conn->state = TCP_SYN_SENT;
    conn->local_ip = net_get_ip();
    conn->remote_ip = remote_ip;
    conn->local_port = local_port;
    conn->remote_port = remote_port;
    conn->snd_nxt = 1;
    // CRITICAL: snd_una must track the ISN (Initial Sequence Number)
    // so that SYN retransmits send seq=ISN, not seq=ISN+1. Without
    // this, each retransmit sends a "new" SYN with an incremented seq,
    // which the server interprets as a duplicate SYN on an established
    // connection → RST. (Previously snd_una stayed 0 from memset, so
    // retransmits using snd_nxt sent seq=2, 3, 4... instead of seq=1.)
    conn->snd_una = conn->snd_nxt;  // = 1 (the ISN)
    conn->connect_time = timer_get_ticks();
    conn->retransmit_timeout = TCP_RETRANSMIT_TIMEOUT;
    conn->retransmit_count = 0;
    conn->last_ack_time = timer_get_ticks();

    // Send SYN
    tcp_send(conn, TCP_SYN, NULL, 0);
    conn->snd_nxt++;  // SYN consumes one seq number; data starts at ISN+1

    return local_port;
}

// Helper function for socket layer - returns connection pointer
tcp_connection_t* tcp_connect_with_ptr(uint32_t remote_ip, uint16_t remote_port) {
    // FIX: Reject connection attempts before the network interface is configured.
    // Same issue as tcp_connect() — local_ip would be 0, causing silent failure.
    if (net_get_ip() == 0) {
        return NULL;
    }

    // CRITICAL: Reap stale connections to the same remote host:port before
    // creating a new one. Without this, old connections in CLOSE_WAIT,
    // LAST_ACK, FIN_WAIT, or TIME_WAIT states linger in the connection
    // table. When the new SYN-ACK arrives, tcp_find_connection() can match
    // the stale connection instead of the new SYN_SENT one — the SYN-ACK
    // is delivered to the stale connection (which ignores it), and the
    // new connection times out.
    //
    // The log showed this exactly:
    //   [TCP] SYN sent, local_port=49152, waiting for SYN-ACK...
    //   [TCP] Matched conn state=7   ← CLOSE_WAIT! Stale connection!
    //   [TCP] TIMEOUT after 2001 ticks
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_connection_t* old = &tcp_connections[i];
        if (old->state == TCP_CLOSED) continue;
        if (old->remote_ip == remote_ip && old->remote_port == remote_port) {
            s_printf("[TCP] Reaping stale connection (state=%d) to %d.%d.%d.%d:%d\n",
                     old->state,
                     (remote_ip >> 24) & 0xFF, (remote_ip >> 16) & 0xFF,
                     (remote_ip >> 8) & 0xFF, remote_ip & 0xFF,
                     remote_port);
            // Force-close the stale connection and free its slot
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

    // Find unused local port with collision check
    uint16_t start_port = tcp_next_port;
    do {
        if (tcp_find_connection(net_get_ip(), tcp_next_port, remote_ip, remote_port) == NULL) break;
        tcp_next_port++;
        if (tcp_next_port > 65535) tcp_next_port = 49152;
    } while (tcp_next_port != start_port);
    uint16_t local_port = tcp_next_port++;
    if (tcp_next_port > 65535) tcp_next_port = 49152;

    tcp_connection_t* conn = tcp_alloc_connection();
    if (!conn) {
        return NULL;
    }

    conn->state = TCP_SYN_SENT;
    conn->local_ip = net_get_ip();
    conn->remote_ip = remote_ip;
    conn->local_port = local_port;
    conn->remote_port = remote_port;
    conn->snd_nxt = 1;
    // CRITICAL: snd_una must track the ISN (Initial Sequence Number)
    // so that SYN retransmits send seq=ISN, not seq=ISN+1. Without
    // this, each retransmit sends a "new" SYN with an incremented seq,
    // which the server interprets as a duplicate SYN on an established
    // connection → RST. (Previously snd_una stayed 0 from memset, so
    // retransmits using snd_nxt sent seq=2, 3, 4... instead of seq=1.)
    conn->snd_una = conn->snd_nxt;  // = 1 (the ISN)
    conn->connect_time = timer_get_ticks();
    conn->retransmit_timeout = TCP_RETRANSMIT_TIMEOUT;
    conn->retransmit_count = 0;
    conn->last_ack_time = timer_get_ticks();

    // Send SYN
    tcp_send(conn, TCP_SYN, NULL, 0);
    conn->snd_nxt++;  // SYN consumes one seq number; data starts at ISN+1

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

// Process incoming TCP packet - OPTIMIZED
void tcp_handle_packet(uint8_t* packet, uint32_t len, uint32_t src_ip, uint32_t dst_ip) {
    tcp_header_t* tcp = (tcp_header_t*)packet;
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dest_port);
    uint8_t pkt_flags = tcp->flags;

    s_printf("[TCP] recv packet: %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d flags=0x%02X len=%d\n",
             (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF, (src_ip >> 8) & 0xFF, src_ip & 0xFF, src_port,
             (dst_ip >> 24) & 0xFF, (dst_ip >> 16) & 0xFF, (dst_ip >> 8) & 0xFF, dst_ip & 0xFF, dst_port,
             pkt_flags, len);

    // Find connection
    tcp_connection_t* conn = tcp_find_connection(dst_ip, dst_port, src_ip, src_port);
    if (!conn) {
        s_printf("[TCP] No matching connection found\n");
        /* No existing connection found. Check if there's a listener on this port. */
        if (pkt_flags & TCP_SYN) {
            /* Incoming SYN to a potentially listening port */
            tcp_handle_incoming_syn(dst_port, src_ip, src_port, dst_ip, ntohl(tcp->seq_num));
        }
        /* Otherwise, silently drop the packet */
        return;
    }

    s_printf("[TCP] Matched conn state=%d\n", conn->state);

    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t flags = pkt_flags;

    // Handle RST
    if (flags & TCP_RST) {
        conn->state = TCP_CLOSED;
        tcp_ofo_flush(conn);  // discard any queued out-of-order segments
        if (conn->on_state_change) {
            conn->on_state_change(conn->state, TCP_CLOSED);
        }
        return;
    }

    // Update window
    conn->window = ntohs(tcp->window);

    // Track when we last received ANY segment from the peer. The
    // retransmit logic uses this to suppress premature retransmits
    // when the peer is actively sending us data (e.g. the server is
    // sending the TLS Certificate in multiple segments and hasn't
    // ACKed our ClientHello yet — the connection is clearly alive).
    conn->last_recv_time = timer_get_ticks();

    // Process based on current state
    switch (conn->state) {
        case TCP_SYN_RECEIVED:
            /* Server side: waiting for ACK to complete 3-way handshake.
             * Same race-free check as TCP_SYN_SENT above: use snd_una+1
             * instead of snd_nxt to avoid the retransmit race. */
            if (flags & TCP_ACK) {
                if (ack == conn->snd_una + 1) {
                    conn->snd_una = ack;
                    conn->state = TCP_ESTABLISHED;

                    if (conn->on_state_change) {
                        conn->on_state_change(TCP_SYN_RECEIVED, TCP_ESTABLISHED);
                    }
                }
            }
            break;

        case TCP_SYN_SENT:
            if (flags & TCP_SYN && flags & TCP_ACK) {
                // RFC 793: a SYN-ACK is valid when its ACK field equals
                // snd_una + 1 (i.e., it ACKs our SYN, which consumed
                // sequence number snd_una = ISN).
                //
                // CRITICAL: we must NOT use snd_nxt here. During SYN
                // retransmits, the retransmit code temporarily sets
                // snd_nxt = snd_una (= ISN) so tcp_send emits the
                // correct seq. If the NIC ISR fires during that window
                // (interrupts ARE enabled — IMR=0x0015, ISR on int 0x81),
                // this handler runs with snd_nxt = ISN instead of ISN+1,
                // causing `ack == snd_nxt` → `ISN+1 == ISN` → false.
                // The SYN-ACK is silently dropped and the connection
                // stays in SYN_SENT forever, eventually timing out.
                //
                // Using snd_una + 1 is both RFC-correct AND immune to
                // the retransmit race, because snd_una is never modified
                // by the retransmit code.
                if (ack == conn->snd_una + 1) {
                    conn->rcv_nxt = seq + 1;
                    conn->snd_una = ack;
                    conn->state = TCP_ESTABLISHED;
                    conn->retransmit_count = 0;
                    // Cancel the SYN retransmit timer. Without this, a
                    // retransmit that was already "in flight" (timer
                    // armed, about to fire) could send a SYN on the
                    // now-established connection, causing the server
                    // to RST. The ESTABLISHED state's own retransmit
                    // timer will be armed when we send data.
                    conn->retransmit_timeout = 0;

                    // Send ACK
                    tcp_send(conn, TCP_ACK, NULL, 0);

                    s_printf("[TCP] Connection ESTABLISHED (SYN-ACK ack=%u, snd_una was %u)\n",
                             ack, ack - 1);

                    if (conn->on_state_change) {
                        conn->on_state_change(TCP_SYN_SENT, TCP_ESTABLISHED);
                    }
                } else {
                    s_printf("[TCP] SYN-ACK rejected: ack=%u, expected snd_una+1=%u (snd_nxt=%u)\n",
                             ack, conn->snd_una + 1, conn->snd_nxt);
                }
            }
            break;

        case TCP_ESTABLISHED:
        case TCP_CLOSE_WAIT:
            // Handle ACK — advance send_head and clear retransmit state
            // when all outstanding data is acknowledged.
            //
            // BUG 1 FIX (per user analysis): send_head was never advanced
            // on ACK reception. This caused the retransmit path to bundle
            // old already-acknowledged data with new sequence numbers,
            // corrupting the TCP stream and causing the server to FIN.
            //
            // BUG 2 FIX (per user analysis): retransmit_timeout was never
            // cleared when all data was ACKed. This left a stale timeout
            // value that triggered instant spurious retransmits when new
            // data was sent later.
            if (flags & TCP_ACK) {
                if (ack > conn->snd_una) {
                    uint32_t acked_bytes = ack - conn->snd_una;
                    conn->snd_una = ack;

                    // Advance send_head by the number of acknowledged bytes
                    conn->send_head += acked_bytes;
                    if (conn->send_head > conn->send_tail) {
                        conn->send_head = conn->send_tail;  // Sanity protection
                    }

                    // If all outstanding data is fully ACKed, reclaim buffer
                    // space and clear retransmit state
                    if (conn->send_head == conn->send_tail) {
                        conn->send_head = 0;
                        conn->send_tail = 0;
                        // Clear retransmit timer so the next tcp_send_data
                        // call starts fresh (no stale timeout)
                        conn->retransmit_timeout = 0;
                    }

                    conn->retransmit_count = 0;
                    conn->last_ack_time = timer_get_ticks();
                }
            }

            // Handle data
            // TCP data_offset is a 4-bit field in the HIGH nibble of the byte,
            // representing the header length in 32-bit words. To get bytes:
            //   (data_offset >> 4) * 4
            //
            // BUG FIX: Previously used (data_offset >> 2) * 4, which for a
            // standard 20-byte header (data_offset byte = 0x50) computed
            // (0x50 >> 2) * 4 = 0x14 * 4 = 80 instead of 20. This caused
            // tcp_handle_packet to skip 80 bytes of packet data (eating 60
            // bytes of actual HTTP response data) before passing it to the
            // callback. The response buffer then started mid-URL, breaking
            // HTTP status parsing and redirect detection.
            uint16_t hdr_len = (tcp->data_offset >> 4) * 4;
            if (len > hdr_len) {
                uint16_t data_len = len - hdr_len;
                uint8_t* data = packet + hdr_len;

                s_printf("[TCP] data packet: seq=%u rcv_nxt=%u data_len=%u on_data=%s\n",
                         seq, conn->rcv_nxt, data_len, conn->on_data ? "SET" : "NULL");

                // Handle partially-overlapping segments.
                //
                // When the server retransmits a segment that starts at or before
                // our rcv_nxt but extends beyond it, the segment contains both
                // already-received data AND new data. We must trim the already-
                // received portion and accept only the new data.
                //
                // Without this, a retransmitted segment that partially overlaps
                // with previously-received data is dropped entirely — even though
                // it contains new bytes we haven't seen. This causes data loss,
                // retransmit storms, and eventual TX descriptor exhaustion.
                if (seq < conn->rcv_nxt) {
                    uint32_t overlap = conn->rcv_nxt - seq;
                    if (overlap >= data_len) {
                        // Entire segment is old data (already received). Just ACK.
                        s_printf("[TCP] dup segment: seq=%u rcv_nxt=%u (all %u bytes already received)\n",
                                 seq, conn->rcv_nxt, data_len);
                        tcp_send(conn, TCP_ACK, NULL, 0);
                        goto after_data;
                    }
                    // Trim the already-received portion
                    s_printf("[TCP] partial overlap: trimming %u old bytes from segment (seq=%u, rcv_nxt=%u)\n",
                             overlap, seq, conn->rcv_nxt);
                    data += overlap;
                    data_len -= overlap;
                    seq += overlap;
                }

                // Check sequence number (now seq should == rcv_nxt after trimming)
                if (seq == conn->rcv_nxt) {
                    // In-order segment. Deliver via callback if registered,
                    // else fall back to flat buffer — UNLESS the user has
                    // called k_close() and given up interest in this conn.
                    // In that case we ACK and discard, which keeps the
                    // peer's TCP FSM progressing toward CLOSED without
                    // polluting the orphaned recv_buffer (the previous
                    // behaviour produced endless "on_data=NULL" log spam
                    // and could wedge the connection in FIN_WAIT1 forever
                    // when the peer kept retransmitting late data).
                    if (conn->on_data) {
                        conn->on_data(data, data_len, conn->callback_user_data);
                    } else if (!conn->user_closing) {
                        // No callback — fall back to flat buffer for direct tcp_conn_recv() users
                        if (conn->recv_tail + data_len <= sizeof(conn->recv_buffer)) {
                            memcpy(conn->recv_buffer + conn->recv_tail, data, data_len);
                            conn->recv_tail += data_len;
                        } else if (conn->recv_tail < sizeof(conn->recv_buffer)) {
                            uint32_t remaining = sizeof(conn->recv_buffer) - conn->recv_tail;
                            memcpy(conn->recv_buffer + conn->recv_tail, data, remaining);
                            conn->recv_tail = sizeof(conn->recv_buffer);
                        }
                    }
                    // If user_closing is set, we silently drop the data.

                    conn->rcv_nxt += data_len;

                    s_printf("[TCP] data delivered: %u bytes via %s, new rcv_nxt=%u\n",
                             data_len,
                             conn->user_closing ? "drop(closing)" :
                             (conn->on_data ? "callback" : "flat_buffer"),
                             conn->rcv_nxt);

                    // Send ACK for in-order received data
                    tcp_send(conn, TCP_ACK, NULL, 0);

                    // Now that rcv_nxt has advanced, drain any previously-
                    // queued out-of-order segments that have become in-order.
                    tcp_ofo_drain(conn);
                } else {
                    // Out-of-order segment (seq > rcv_nxt).
                    // RFC 793 §3.3, §3.4: buffer the segment so the sender
                    // doesn't have to retransmit it after an earlier gap
                    // is filled. CamelOS previously DROPPED these — that
                    // worked for bulk HTTP transfers but was fatal for TLS
                    // handshakes, where the ServerHello + Certificate
                    // arrive as multiple TCP segments and a single dropped
                    // segment forced a 30-60s RTO during which the TLS
                    // handshake timer expired.
                    //
                    // We always send a dup ACK (so the sender can fast-
                    // retransmit if it detects 3 dup ACKs) AND we queue
                    // the segment for later delivery.
                    if (!conn->user_closing) {
                        tcp_ofo_enqueue(conn, seq, data, data_len);
                    }
                    s_printf("[TCP] OUT-OF-ORDER: seq=%u expected rcv_nxt=%u (queued %u bytes, queue=%d/8)\n",
                             seq, conn->rcv_nxt, data_len,
                             TCP_OFO_QUEUE_SIZE - tcp_ofo_find_free(conn));
                    tcp_send(conn, TCP_ACK, NULL, 0);
                }
            }

        after_data:;

            // Handle FIN
            if (flags & TCP_FIN) {
                if (conn->state == TCP_ESTABLISHED) {
                    conn->rcv_nxt = seq + 1;
                    conn->state = TCP_CLOSE_WAIT;
                    // Send ACK for the FIN, but don't send our own FIN yet
                    // The application will call close() when ready
                    tcp_send(conn, TCP_ACK, NULL, 0);
                }
            }
            break;

        case TCP_FIN_WAIT1:
            if (flags & TCP_FIN && flags & TCP_ACK) {
                // Simultaneous close: received FIN+ACK
                conn->rcv_nxt = seq + 1;
                tcp_send(conn, TCP_ACK, NULL, 0);
                conn->state = TCP_TIME_WAIT;
                conn->time_wait_start = timer_get_ticks();
                if (conn->on_state_change) {
                    conn->on_state_change(TCP_FIN_WAIT1, TCP_TIME_WAIT);
                }
            } else if (flags & TCP_ACK) {
                conn->state = TCP_FIN_WAIT2;
            } else if (flags & TCP_FIN) {
                // Got FIN without ACK
                conn->rcv_nxt = seq + 1;
                tcp_send(conn, TCP_ACK, NULL, 0);
                conn->state = TCP_CLOSING;
            }
            break;

        case TCP_FIN_WAIT2:
            if (flags & TCP_FIN) {
                conn->rcv_nxt = seq + 1;
                tcp_send(conn, TCP_ACK, NULL, 0);
                conn->state = TCP_TIME_WAIT;
                conn->time_wait_start = timer_get_ticks();
                if (conn->on_state_change) {
                    conn->on_state_change(TCP_FIN_WAIT2, TCP_TIME_WAIT);
                }
            }
            break;

        case TCP_CLOSING:
            if (flags & TCP_ACK) {
                conn->state = TCP_TIME_WAIT;
                conn->time_wait_start = timer_get_ticks();
                if (conn->on_state_change) {
                    conn->on_state_change(TCP_CLOSING, TCP_TIME_WAIT);
                }
            }
            break;

        case TCP_LAST_ACK:
            if (flags & TCP_ACK) {
                conn->state = TCP_CLOSED;
                if (conn->on_state_change) {
                    conn->on_state_change(TCP_LAST_ACK, TCP_CLOSED);
                }
            }
            break;
    }

    // Update last ACK time
    conn->last_ack_time = timer_get_ticks();
}

// Send data over TCP connection
int tcp_send_data(tcp_connection_t* conn, uint8_t* data, uint16_t len) {
    if (conn->state != TCP_ESTABLISHED) {
        return -1;
    }

    // Add to send buffer with bounds check against actual buffer size
    if (conn->send_tail + len <= sizeof(conn->send_buffer)) {
        memcpy(conn->send_buffer + conn->send_tail, data, len);
        conn->send_tail += len;
    } else if (conn->send_tail < sizeof(conn->send_buffer)) {
        // Partial write
        uint32_t remaining = sizeof(conn->send_buffer) - conn->send_tail;
        memcpy(conn->send_buffer + conn->send_tail, data, remaining);
        conn->send_tail = sizeof(conn->send_buffer);
    }

    // Send data (in chunks of MSS)
    uint16_t sent = 0;
    while (sent < len) {
        uint16_t chunk = len - sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;

        tcp_send(conn, TCP_ACK | TCP_PSH, data + sent, chunk);
        conn->snd_nxt += chunk;
        sent += chunk;
    }

    // Arm the retransmit timer ONLY if it's not already armed.
    // Re-arming on every send caused a TX storm: each tcp_send_data
    // call reset the timer, and combined with the retransmit logic,
    // this caused repeated ClientHello retransmits that wedged the
    // RTL8139 TX descriptors (they'd time out and never recover).
    // Now we only arm the timer if it was cleared (e.g. after an ACK
    // or after the SYN→ESTABLISHED transition). If it's already
    // armed, we leave it alone so the existing timeout continues
    // counting from when the first unacked data was sent.
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
    if (!conn || conn->state != TCP_ESTABLISHED) return -1;
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

    // Reset buffer pointers when all data consumed to reclaim space
    if (conn->recv_head == conn->recv_tail) {
        conn->recv_head = 0;
        conn->recv_tail = 0;
    }

    return to_read;
}

// Set data callback for a TCP connection
void tcp_conn_set_data_callback(void* conn_ptr, void (*callback)(uint8_t*, uint16_t, void*), void* user_data) {
    tcp_connection_t* conn = (tcp_connection_t*)conn_ptr;
    if (conn) {
        conn->on_data = callback;
        conn->callback_user_data = user_data;
    }
}

// Set state change callback for a TCP connection
void tcp_conn_set_state_callback(void* conn_ptr, void (*callback)(uint8_t, uint8_t)) {
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

    /* Check if already listening on this port */
    if (tcp_find_listener(port)) {
        s_printf("[TCP] Port %d already in listen\n", port);
        return -1;
    }

    /* Find a free listener slot */
    int slot = -1;
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (!tcp_listeners[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        s_printf("[TCP] No free listener slots\n");
        return -1;
    }

    /* Initialize the listener */
    memset(&tcp_listeners[slot], 0, sizeof(tcp_listener_t));
    tcp_listeners[slot].in_use = 1;
    tcp_listeners[slot].port = port;
    tcp_listeners[slot].bind_ip = bind_ip;
    tcp_listeners[slot].pending_count = 0;
    tcp_listeners[slot].established_count = 0;

    s_printf("[TCP] Listening on port %d\n", port);
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
    if (!listener->in_use) return -1;

    /* Check if there are any established connections */
    if (listener->established_count <= 0) {
        return -1;  /* No connections ready */
    }

    /* Dequeue the first established connection */
    tcp_connection_t* conn = listener->established[0];
    *out_conn = conn;

    /* Shift the array */
    for (int i = 1; i < listener->established_count; i++) {
        listener->established[i - 1] = listener->established[i];
    }
    listener->established_count--;
    listener->established[listener->established_count] = NULL;

    s_printf("[TCP] Accepted connection on port %d from remote\n", listener->port);
    return 0;
}

/**
 * Close a listener and free all pending connections.
 */
int tcp_close_listener(int listener_id) {
    if (listener_id < 0 || listener_id >= TCP_MAX_LISTENERS) return -1;

    tcp_listener_t* listener = &tcp_listeners[listener_id];
    if (!listener->in_use) return -1;

    /* Close all pending connections */
    for (int i = 0; i < listener->pending_count; i++) {
        if (listener->pending[i]) {
            /* Send RST and close */
            tcp_send(listener->pending[i], TCP_RST, NULL, 0);
            listener->pending[i]->state = TCP_CLOSED;
            listener->pending[i] = NULL;
        }
    }

    /* Close all established connections */
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

    s_printf("[TCP] Closed listener\n");
    return 0;
}

/**
 * Process incoming SYN packets for listeners.
 * Called from tcp_handle_packet when no existing connection is found.
 * This implements the server side of the TCP three-way handshake.
 */
static int tcp_handle_incoming_syn(uint16_t dst_port, uint32_t src_ip, uint16_t src_port,
                                    uint32_t dst_ip, uint32_t seq) {
    tcp_listener_t* listener = tcp_find_listener(dst_port);
    if (!listener) return -1;

    /* Check backlog */
    if (listener->pending_count >= TCP_LISTEN_BACKLOG) {
        s_printf("[TCP] Listen backlog full on port %d\n", dst_port);
        return -1;
    }

    /* Allocate a new connection for this incoming request */
    tcp_connection_t* conn = tcp_alloc_connection();
    if (!conn) return -1;

    /* Set up the connection as SYN_RECEIVED */
    conn->state = TCP_SYN_RECEIVED;
    conn->local_ip = dst_ip;
    conn->remote_ip = src_ip;
    conn->local_port = dst_port;
    conn->remote_port = src_port;
    conn->rcv_nxt = seq + 1;
    conn->snd_nxt = (uint32_t)(timer_get_ticks() ^ (src_port << 16));  /* ISS */
    if (conn->snd_nxt == 0) conn->snd_nxt = 1;
    conn->connect_time = timer_get_ticks();
    conn->window = TCP_WINDOW_SIZE;
    conn->mss = TCP_MSS;

    /* Send SYN-ACK */
    tcp_send(conn, TCP_SYN | TCP_ACK, NULL, 0);
    conn->snd_nxt++;

    /* Add to pending list */
    listener->pending[listener->pending_count++] = conn;

    s_printf("[TCP] SYN received on port %d from remote, sent SYN-ACK\n", dst_port);
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

        /* Check pending connections for state changes */
        for (int j = 0; j < listener->pending_count; j++) {
            tcp_connection_t* conn = listener->pending[j];
            if (!conn) continue;

            if (conn->state == TCP_ESTABLISHED) {
                /* Move to established queue */
                if (listener->established_count < TCP_LISTEN_BACKLOG) {
                    listener->established[listener->established_count++] = conn;
                    s_printf("[TCP] Connection established on port %d\n", listener->port);
                }

                /* Remove from pending */
                for (int k = j; k < listener->pending_count - 1; k++) {
                    listener->pending[k] = listener->pending[k + 1];
                }
                listener->pending_count--;
                listener->pending[listener->pending_count] = NULL;
                j--;  /* Recheck this index */
            }
            else if (conn->state == TCP_CLOSED) {
                /* Connection was reset before completing handshake */
                for (int k = j; k < listener->pending_count - 1; k++) {
                    listener->pending[k] = listener->pending[k + 1];
                }
                listener->pending_count--;
                listener->pending[listener->pending_count] = NULL;
                j--;
            }
        }
    }
}

// ============================================================================
// TCP Retransmission Timer
// ============================================================================

/**
 * tcp_retransmit_check - Check all connections for retransmission timeout.
 *
 * Should be called periodically (e.g., from the timer tick or main loop).
 * For each active connection that has unacknowledged data and whose
 * retransmit timer has expired, retransmits the last segment.
 * Implements exponential backoff: each retransmit doubles the timeout.
 *
 * After TCP_MAX_RETRANSMIT attempts, the connection is closed with a RST.
 */
void tcp_retransmit_check(void)
{
    uint32_t now = timer_get_ticks();

    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_connection_t* conn = &tcp_connections[i];

        /* Skip closed or idle connections */
        if (conn->state == TCP_CLOSED) continue;

        // Check for TIME_WAIT expiry (2MSL = ~60 seconds)
        if (conn->state == TCP_TIME_WAIT) {
            uint32_t elapsed = now - conn->time_wait_start;
            if (elapsed >= 6000) {  // Assuming 100Hz timer, 6000 ticks = 60 seconds
                conn->state = TCP_CLOSED;
                tcp_ofo_flush(conn);  // belt-and-suspenders: drain any residual queue
            }
            continue;
        }

        // Reap stale CLOSE_WAIT / LAST_ACK / FIN_WAIT connections.
        // These states mean the connection is closing but hasn't fully closed.
        // If no progress is made for 30 seconds (1500 ticks), force-close.
        // This prevents the connection table from filling up with zombie
        // connections that block new connections to the same host:port.
        if (conn->state == TCP_CLOSE_WAIT || conn->state == TCP_LAST_ACK ||
            conn->state == TCP_FIN_WAIT1 || conn->state == TCP_FIN_WAIT2 ||
            conn->state == TCP_CLOSING) {
            uint32_t idle = now - conn->last_ack_time;
            if (idle > 1500) {  // 30 seconds
                s_printf("[TCP] Reaping idle connection in state %d (idle %d ticks)\n",
                         conn->state, idle);
                conn->state = TCP_CLOSED;
                tcp_ofo_flush(conn);
                conn->retransmit_timeout = 0;
            }
            continue;
        }

        if (conn->retransmit_timeout == 0) continue;

        /* Check if the retransmit timer has expired */
        uint32_t elapsed = now - conn->last_ack_time;
        if (elapsed < conn->retransmit_timeout) continue;

        /* DON'T retransmit if we're actively receiving data from the peer.
         *
         * If the peer is sending us data (rcv_nxt is advancing), the
         * connection is clearly alive — our data just hasn't been ACKed
         * yet because the peer is busy sending. Retransmitting would
         * create a TX storm that wedges the NIC, as seen in the log:
         *   [TLS] Step 1 OK: ClientHello sent
         *   [TCP] Retransmitting data (attempt 1)   ← premature!
         *   [RTL8139] TX timeout on descriptor       ← TX wedged
         *
         * We track the last time we RECEIVED a segment in
         * last_recv_time. If we received something recently (within
         * the retransmit timeout window), reset the retransmit timer
         * instead of retransmitting.
         */
        if (conn->last_recv_time > 0) {
            uint32_t since_recv = now - conn->last_recv_time;
            if (since_recv < conn->retransmit_timeout) {
                /* Peer is still sending to us — reset timer and wait. */
                conn->last_ack_time = now;
                continue;
            }
        }

        /* Too many retransmits — give up and close the connection */
        if (conn->retransmit_count >= TCP_MAX_RETRANSMIT) {
            s_printf("[TCP] Max retransmits exceeded for port %d, closing\n",
                     conn->remote_port);
            tcp_send(conn, TCP_RST, NULL, 0);
            conn->state = TCP_CLOSED;
            tcp_ofo_flush(conn);
            if (conn->on_state_change) {
                conn->on_state_change(TCP_ESTABLISHED, TCP_CLOSED);
            }
            continue;
        }

        /* Retransmit based on current state */
        conn->retransmit_count++;

        /* Exponential backoff: double the timeout for next retransmit */
        conn->retransmit_timeout *= 2;
        /* Cap at 30 seconds (assuming ~50 ticks/sec, that's 1500 ticks) */
        if (conn->retransmit_timeout > 1500) {
            conn->retransmit_timeout = 1500;
        }

        /* Reset the timer for the next check */
        conn->last_ack_time = now;

        switch (conn->state) {
            case TCP_SYN_SENT:
                /* Retransmit SYN.
                 *
                 * CRITICAL FIX: SYN retransmits MUST use the same sequence
                 * number as the original SYN (the ISN). tcp_send() uses
                 * conn->snd_nxt as the seq, but snd_nxt was incremented
                 * to ISN+1 after the first SYN send. Without saving and
                 * restoring snd_nxt, each retransmit sends seq=ISN+1,
                 * ISN+2, etc. — which the server interprets as duplicate
                 * SYNs on an established connection, causing it to RST.
                 *
                 * The save/restore pattern mirrors the ESTABLISHED case
                 * below: temporarily set snd_nxt = snd_una (= ISN), send
                 * the SYN, then restore snd_nxt. */
                s_printf("[TCP] Retransmitting SYN to port %d (attempt %d)\n",
                         conn->remote_port, conn->retransmit_count);
                {
                    uint32_t saved_nxt = conn->snd_nxt;
                    conn->snd_nxt = conn->snd_una;  /* = ISN */
                    tcp_send(conn, TCP_SYN, NULL, 0);
                    conn->snd_nxt = saved_nxt;      /* restore to ISN+1 */
                }
                break;

            case TCP_SYN_RECEIVED:
                /* Retransmit SYN-ACK (server side). Same save/restore
                 * pattern as SYN_SENT above. */
                s_printf("[TCP] Retransmitting SYN-ACK to port %d (attempt %d)\n",
                         conn->remote_port, conn->retransmit_count);
                {
                    uint32_t saved_nxt = conn->snd_nxt;
                    conn->snd_nxt = conn->snd_una;  /* = ISN */
                    tcp_send(conn, TCP_SYN | TCP_ACK, NULL, 0);
                    conn->snd_nxt = saved_nxt;      /* restore */
                }
                break;

            case TCP_ESTABLISHED:
            case TCP_CLOSE_WAIT:
                /* Retransmit unacknowledged data from the send buffer.
                 * We resend from send_head up to send_tail in MSS-sized chunks,
                 * starting from the sequence number that hasn't been ACKed. */
                if (conn->send_tail > conn->send_head) {
                    s_printf("[TCP] Retransmitting data on port %d (attempt %d, %d bytes)\n",
                             conn->remote_port, conn->retransmit_count,
                             conn->send_tail - conn->send_head);

                    uint32_t data_len = conn->send_tail - conn->send_head;
                    uint32_t offset = 0;

                    /* Save and temporarily set snd_nxt to snd_una for retransmit */
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

                    /* Restore snd_nxt if it advanced beyond what we retransmitted */
                    if (saved_nxt > conn->snd_nxt) {
                        conn->snd_nxt = saved_nxt;
                    }
                } else {
                    /* No data in send buffer — just send a keep-alive ACK */
                    tcp_send(conn, TCP_ACK, NULL, 0);
                }
                break;

            case TCP_FIN_WAIT1:
                /* Retransmit FIN */
                s_printf("[TCP] Retransmitting FIN to port %d (attempt %d)\n",
                         conn->remote_port, conn->retransmit_count);
                tcp_send(conn, TCP_FIN | TCP_ACK, NULL, 0);
                break;

            case TCP_LAST_ACK:
                /* Retransmit FIN */
                tcp_send(conn, TCP_FIN | TCP_ACK, NULL, 0);
                break;

            default:
                /* Other states: no retransmit action needed */
                break;
        }
    }
}
