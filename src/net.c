/*
 * ArcadeOS – minimal network stack + HTTP REST API
 *
 * Server-only by design: the console never opens outbound connections,
 * so the stack stays tiny —
 *   - ARP:  replies to "who has 10.0.2.15" (never asks; peers ARP us)
 *   - IPv4: header parse/build + checksum
 *   - ICMP: echo reply, so `ping` works
 *   - TCP:  ONE listening socket on port 80, one connection at a time,
 *           no retransmission (QEMU slirp is a local, lossless link)
 *   - HTTP: GET router serving JSON/text (the REST API)
 *
 * Endpoints:
 *   GET /            – API index
 *   GET /api/status  – uptime, tasks, memory
 *   GET /api/games   – *.ELF files on the game volume
 *   GET /api/scores  – parsed *.SAV high scores
 *   GET /api/log     – kernel log ring (text/plain)
 *
 * Everything is pumped from net_poll() in the idle task.
 */

#include "net.h"
#include "vga.h"
#include "serial.h"
#include "clock.h"
#include "klog.h"
#include "fat32.h"
#include "session.h"
#include "vfs.h"
#include "pmm.h"
#include "task.h"

/* Freestanding substring search (no libc strstr in the kernel) */
static const char* strstr_simple(const char* hay, const char* needle) {
    uint32_t nl = (uint32_t)strlen(needle);
    for (; *hay; hay++)
        if (strncmp(hay, needle, nl) == 0) return hay;
    return 0;
}

/* Our static identity (QEMU slirp defaults) */
static const uint8_t our_ip[4] = { 10, 0, 2, 15 };

#define ETH_ARP  0x0806
#define ETH_IP   0x0800
#define IP_ICMP  1
#define IP_TCP   6
#define IP_UDP   17
#define HTTP_PORT 80
#define UDP_ECHO_PORT 7

#define FRAME_MAX 1792
#define SEG_DATA  1400          /* TCP payload per segment */

/* ──────── Byte-order helpers ──────── */
static inline uint16_t htons16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t htonl32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | (v << 24);
}

/* ──────── Wire structures ──────── */
typedef struct {
    uint8_t  dst[6], src[6];
    uint16_t type;
} __attribute__((packed)) eth_hdr_t;

typedef struct {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t oper;
    uint8_t  sha[6], spa[4], tha[6], tpa[4];
} __attribute__((packed)) arp_pkt_t;

typedef struct {
    uint8_t  ver_ihl, tos;
    uint16_t total_len, id, frag;
    uint8_t  ttl, proto;
    uint16_t checksum;
    uint8_t  src[4], dst[4];
} __attribute__((packed)) ip_hdr_t;

typedef struct {
    uint8_t  type, code;
    uint16_t checksum;
    uint16_t id, seq;
} __attribute__((packed)) icmp_hdr_t;

typedef struct {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t  offset;            /* Data offset in high nibble */
    uint8_t  flags;
    uint16_t window, checksum, urgent;
} __attribute__((packed)) tcp_hdr_t;

typedef struct {
    uint16_t sport, dport, len, checksum;
} __attribute__((packed)) udp_hdr_t;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* ──────── TCP connection (single) ──────── */
typedef enum { TCP_LISTEN, TCP_SYN_RCVD, TCP_ESTAB, TCP_FIN_SENT } tcp_state_t;

static struct {
    tcp_state_t state;
    uint8_t  peer_mac[6];
    uint8_t  peer_ip[4];
    uint16_t peer_port;
    uint32_t snd_nxt, rcv_nxt;
    char     req[1024];
    uint32_t req_len;
} conn;

static uint8_t frame_out[FRAME_MAX];
static uint8_t frame_in[FRAME_MAX];

/* ──────── ARP cache (learned from every ARP we see) ──────── */
#define ARP_CACHE 4
static struct {
    int     valid;
    uint8_t ip[4];
    uint8_t mac[6];
} arp_cache[ARP_CACHE];
static int arp_next = 0;

/* ──────── UDP game socket ──────── */
#define UDP_QUEUE   8
#define UDP_MSG_MAX 512

static uint16_t udp_port = 0;             /* 0 = not bound */
static struct {
    uint16_t len;
    uint16_t sport;
    uint8_t  sip[4];
    uint8_t  data[UDP_MSG_MAX];
} udp_q[UDP_QUEUE];
static uint32_t udp_head = 0, udp_tail = 0;   /* tail=write, head=read */

/* Response body staging (the log endpoint dominates the size) */
static char body_buf[40960];
static char head_buf[256];

static int net_up = 0;

/* ──────── Live score (SYS_SCORE) ──────── */
static int      live_score = 0;
static uint32_t live_pid   = 0;
static char     live_name[32];
static int      live_valid = 0;

void score_report(int score, uint32_t task_id, const char* task_name) {
    live_score = score;
    live_pid   = task_id;
    strncpy(live_name, task_name, sizeof(live_name) - 1);
    live_name[sizeof(live_name) - 1] = '\0';
    live_valid = 1;
}

/* The reporting game must still be alive for the score to count */
static int live_score_active(void) {
    if (!live_valid) return 0;
    for (int i = 0; i < num_tasks; i++)
        if (tasks[i].id == live_pid && tasks[i].state != TASK_DEAD)
            return 1;
    return 0;
}

/* ──────── Checksums ──────── */
static uint16_t checksum16(const void* data, uint32_t len, uint32_t start) {
    uint32_t sum = start;
    const uint8_t* p = (const uint8_t*)data;
    while (len > 1) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t tcp_checksum(const ip_hdr_t* ip, const tcp_hdr_t* tcp, uint32_t tcp_len) {
    /* Pseudo header sum */
    uint32_t sum = 0;
    sum += ((uint32_t)ip->src[0] << 8) | ip->src[1];
    sum += ((uint32_t)ip->src[2] << 8) | ip->src[3];
    sum += ((uint32_t)ip->dst[0] << 8) | ip->dst[1];
    sum += ((uint32_t)ip->dst[2] << 8) | ip->dst[3];
    sum += IP_TCP;
    sum += tcp_len;
    uint16_t c = checksum16(tcp, tcp_len, sum);
    return c;
}

/* ──────── String building (freestanding kernel: no sprintf) ──────── */
static char* sappend(char* p, const char* s) {
    while (*s) *p++ = *s++;
    return p;
}
static char* sappend_u(char* p, uint32_t v) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = tmp[--n];
    return p;
}

/* ──────── Frame TX helpers ──────── */
static void send_ip(const uint8_t* dst_mac, const uint8_t* dst_ip,
                    uint8_t proto, const void* payload, uint32_t len) {
    eth_hdr_t* eth = (eth_hdr_t*)frame_out;
    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, rtl8139_mac(), 6);
    eth->type = htons16(ETH_IP);

    ip_hdr_t* ip = (ip_hdr_t*)(frame_out + sizeof(eth_hdr_t));
    ip->ver_ihl   = 0x45;
    ip->tos       = 0;
    ip->total_len = htons16((uint16_t)(20 + len));
    ip->id        = htons16((uint16_t)system_ticks);
    ip->frag      = 0;
    ip->ttl       = 64;
    ip->proto     = proto;
    ip->checksum  = 0;
    memcpy(ip->src, our_ip, 4);
    memcpy(ip->dst, dst_ip, 4);
    ip->checksum = htons16(checksum16(ip, 20, 0));

    memcpy(frame_out + sizeof(eth_hdr_t) + 20, payload, len);
    rtl8139_send(frame_out, sizeof(eth_hdr_t) + 20 + len);
}

/* Build+send one TCP segment to the current peer */
static void tcp_send(uint8_t flags, const void* data, uint32_t len) {
    static uint8_t seg[20 + SEG_DATA + 4];
    tcp_hdr_t* tcp = (tcp_hdr_t*)seg;
    uint32_t hdr_len = 20;

    tcp->sport   = htons16(HTTP_PORT);
    tcp->dport   = htons16(conn.peer_port);
    tcp->seq     = htonl32(conn.snd_nxt);
    tcp->ack     = htonl32(conn.rcv_nxt);
    tcp->flags   = flags;
    tcp->window  = htons16(8192);
    tcp->checksum = 0;
    tcp->urgent  = 0;

    if (flags & TCP_SYN) {
        /* MSS option: 1460 */
        seg[20] = 2; seg[21] = 4; seg[22] = 1460 >> 8; seg[23] = 1460 & 0xFF;
        hdr_len = 24;
    }
    tcp->offset = (uint8_t)((hdr_len / 4) << 4);

    if (data && len) memcpy(seg + hdr_len, data, len);

    /* Checksum needs the IP header context: fake one for the pseudo sum */
    ip_hdr_t pse;
    memcpy(pse.src, our_ip, 4);
    memcpy(pse.dst, conn.peer_ip, 4);
    tcp->checksum = htons16(tcp_checksum(&pse, tcp, hdr_len + len));

    send_ip(conn.peer_mac, conn.peer_ip, IP_TCP, seg, hdr_len + len);

    if (flags & (TCP_SYN | TCP_FIN)) conn.snd_nxt += 1;
    conn.snd_nxt += len;
}

/* ──────── REST API ──────── */

static char* sappend_hex8(char* p, uint8_t v) {
    static const char hx[] = "0123456789ab" "cdef";
    *p++ = hx[v >> 4];
    *p++ = hx[v & 0xF];
    return p;
}

/* Append the running game's pretty name ("/games/PONG.ELF" → "PONG");
 * appends nothing and returns 0 when no game task is alive. */
static int append_running_game(char** pp) {
    for (int i = 0; i < num_tasks; i++) {
        if (tasks[i].state == TASK_DEAD) continue;
        const char* n = tasks[i].name;
        if (strncmp(n, "/games/", 7) != 0) continue;
        if (strcmp(n, "/games/LAUNCHER.ELF") == 0) continue;

        char* p = *pp;
        char* q = p;
        p = sappend(p, n + 7);
        if (p - q > 4 && strncmp(p - 4, ".ELF", 4) == 0) p -= 4;
        *pp = p;
        return 1;
    }
    return 0;
}

static uint32_t build_status(char* p0) {
    const uint8_t* mac = rtl8139_mac();
    char* p = p0;

    /* Device identity */
    p = sappend(p, "{\"os\":\"ArcadeOS\",\"version\":\"" OS_VERSION "\",");
    p = sappend(p, "\"product\":\"ArcadeOS Console\",\"model\":\"AOS-1\",");
    /* Serial number: stable per console, derived from the NIC MAC */
    p = sappend(p, "\"serial\":\"AOS1-");
    for (int i = 3; i < 6; i++)
        p = sappend_hex8(p, mac[i]);
    p = sappend(p, "\",\"mac\":\"");
    for (int i = 0; i < 6; i++) {
        if (i) *p++ = ':';
        p = sappend_hex8(p, mac[i]);
    }
    p = sappend(p, "\",\"ip\":\"");
    for (int i = 0; i < 4; i++) {
        if (i) *p++ = '.';
        p = sappend_u(p, our_ip[i]);
    }
    p = sappend(p, "\",");

    /* Vitals */
    p = sappend(p, "\"uptime_ms\":");
    p = sappend_u(p, system_ticks);
    p = sappend(p, ",\"tasks\":");
    p = sappend_u(p, (uint32_t)num_tasks);
    p = sappend(p, ",\"free_pages\":");
    p = sappend_u(p, pmm_get_free_pages());
    p = sappend(p, ",\"free_kib\":");
    p = sappend_u(p, pmm_get_free_pages() * 4);

    /* Play status + who is playing */
    p = sappend(p, ",\"status\":\"");
    char* game_probe = p;
    (void)game_probe;
    {
        char tmp_game[32];
        char* tg = tmp_game;
        int playing;
        {
            char* saved = tg;
            playing = append_running_game(&tg);
            *tg = '\0';
            tg = saved;
        }
        p = sappend(p, playing ? "playing" : "menu");
        p = sappend(p, "\"");
        if (playing) {
            p = sappend(p, ",\"game\":\"");
            p = sappend(p, tmp_game);
            p = sappend(p, "\"");
        }
    }

    {
        char p1[SESSION_NAME], p2[SESSION_NAME];
        int count = session_players(p1, p2);
        p = sappend(p, ",\"players\":[\"");
        p = sappend(p, p1);
        p = sappend(p, "\"");
        if (count == 2) {
            p = sappend(p, ",\"");
            p = sappend(p, p2);
            p = sappend(p, "\"");
        }
        p = sappend(p, "]");
    }

    if (live_score_active()) {
        p = sappend(p, ",\"score\":");
        p = sappend_u(p, (uint32_t)(live_score < 0 ? 0 : live_score));
    }
    p = sappend(p, "}\n");
    return (uint32_t)(p - p0);
}

static int name_ends_with(const char* name, const char* suffix) {
    uint32_t n = (uint32_t)strlen(name), s = (uint32_t)strlen(suffix);
    return n > s && strcmp(name + n - s, suffix) == 0;
}

static uint32_t build_games(char* p0) {
    char* p = p0;
    p = sappend(p, "{\"games\":[");
    vfs_node_t* dir = vfs_open("/games", 0);
    int first = 1;
    if (dir) {
        for (uint32_t i = 0; ; i++) {
            vfs_dirent_t* de = vfs_readdir(dir, i);
            if (!de) break;
            if (!(de->flags & VFS_FLAG_FILE)) continue;
            if (!name_ends_with(de->name, ".ELF")) continue;
            uint32_t size = 0;
            vfs_node_t* child = vfs_finddir(dir, de->name);
            if (child) size = child->length;
            if (!first) p = sappend(p, ",");
            first = 0;
            p = sappend(p, "{\"file\":\"");
            p = sappend(p, de->name);
            p = sappend(p, "\",\"bytes\":");
            p = sappend_u(p, size);
            p = sappend(p, "}");
        }
    }
    p = sappend(p, "]}\n");
    return (uint32_t)(p - p0);
}

static uint32_t build_scores(char* p0) {
    /* Every game save is  { u32 magic; int high; }  in a *.SAV file */
    char* p = p0;
    p = sappend(p, "{\"scores\":[");
    vfs_node_t* dir = vfs_open("/games", 0);
    int first = 1;
    if (dir) {
        for (uint32_t i = 0; ; i++) {
            vfs_dirent_t* de = vfs_readdir(dir, i);
            if (!de) break;
            if (!(de->flags & VFS_FLAG_FILE)) continue;
            if (!name_ends_with(de->name, ".SAV")) continue;
            /* The launcher's last-played slot is not a score */
            if (strcmp(de->name, "LAUNCH0.SAV") == 0) continue;
            struct { uint32_t magic; int high; } sv;
            if (fat32_load(de->name, (uint8_t*)&sv, sizeof(sv)) != (int)sizeof(sv))
                continue;
            if (!first) p = sappend(p, ",");
            first = 0;
            p = sappend(p, "{\"file\":\"");
            p = sappend(p, de->name);
            p = sappend(p, "\",\"high\":");
            p = sappend_u(p, (uint32_t)(sv.high < 0 ? 0 : sv.high));
            p = sappend(p, "}");
        }
    }
    p = sappend(p, "]}\n");
    return (uint32_t)(p - p0);
}

static uint32_t build_index(char* p0) {
    char* p = p0;
    p = sappend(p, "{\"name\":\"ArcadeOS REST API\",\"endpoints\":[\"/api/status\",\"/api/games\",\"/api/scores\",\"/api/log\"]}\n");
    return (uint32_t)(p - p0);
}

/* Route a GET; fills body_buf, returns body length, sets content type */
static uint32_t http_route(const char* path, const char** ctype, int* status) {
    *ctype = "application/json";
    *status = 200;
    if (strcmp(path, "/") == 0)            return build_index(body_buf);
    if (strcmp(path, "/api/status") == 0)  return build_status(body_buf);
    if (strcmp(path, "/api/games") == 0)   return build_games(body_buf);
    if (strcmp(path, "/api/scores") == 0)  return build_scores(body_buf);
    if (strcmp(path, "/api/log") == 0) {
        *ctype = "text/plain";
        int n = klog_read(body_buf, sizeof(body_buf) - 1);
        return (uint32_t)(n < 0 ? 0 : n);
    }
    *status = 404;
    return (uint32_t)(sappend(body_buf, "{\"error\":\"not found\"}\n") - body_buf);
}

/* Parse the buffered request and stream the response + FIN */
static void http_respond(void) {
    /* Request line: METHOD SP PATH SP ... */
    char path[128];
    uint32_t n = 0;
    const char* q = conn.req;
    int is_get = (conn.req_len >= 4 && strncmp(q, "GET ", 4) == 0);
    q += 4;
    while (is_get && *q && *q != ' ' && *q != '\r' && n < sizeof(path) - 1)
        path[n++] = *q++;
    path[n] = '\0';

    const char* ctype = "application/json";
    int status = 405;
    uint32_t blen;
    if (!is_get) {
        blen = (uint32_t)(sappend(body_buf, "{\"error\":\"GET only\"}\n") - body_buf);
    } else {
        blen = http_route(path, &ctype, &status);
    }

    /* Log the request to serial + the kernel log ring (NOT the screen,
     * a game may own it). Shows up in /api/log and KERNEL.LOG. */
    {
        char line[192];
        char* l = line;
        l = sappend(l, "[HTTP] ");
        l = sappend(l, is_get ? "GET " : "(non-GET) ");
        l = sappend(l, is_get ? path : "-");
        l = sappend(l, " -> ");
        l = sappend_u(l, (uint32_t)status);
        l = sappend(l, "\n");
        *l = '\0';
        serial_write(line);
    }

    char* h = head_buf;
    h = sappend(h, status == 200 ? "HTTP/1.1 200 OK\r\n"
              : status == 404 ? "HTTP/1.1 404 Not Found\r\n"
                              : "HTTP/1.1 405 Method Not Allowed\r\n");
    h = sappend(h, "Content-Type: ");
    h = sappend(h, ctype);
    h = sappend(h, "\r\nContent-Length: ");
    h = sappend_u(h, blen);
    h = sappend(h, "\r\nConnection: close\r\nServer: ArcadeOS\r\n\r\n");
    uint32_t hlen = (uint32_t)(h - head_buf);

    /* Stream: headers, then the body in MSS-sized segments, then FIN */
    tcp_send(TCP_ACK | TCP_PSH, head_buf, hlen);
    uint32_t off = 0;
    while (off < blen) {
        uint32_t chunk = blen - off;
        if (chunk > SEG_DATA) chunk = SEG_DATA;
        tcp_send(TCP_ACK | TCP_PSH, body_buf + off, chunk);
        off += chunk;
    }
    tcp_send(TCP_ACK | TCP_FIN, 0, 0);
    conn.state = TCP_FIN_SENT;
}

/* ──────── ARP cache + resolution ──────── */

static void arp_learn(const uint8_t* ip, const uint8_t* mac) {
    for (int i = 0; i < ARP_CACHE; i++) {
        if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    arp_cache[arp_next].valid = 1;
    memcpy(arp_cache[arp_next].ip, ip, 4);
    memcpy(arp_cache[arp_next].mac, mac, 6);
    arp_next = (arp_next + 1) % ARP_CACHE;
}

static const uint8_t* arp_find(const uint8_t* ip) {
    for (int i = 0; i < ARP_CACHE; i++)
        if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0)
            return arp_cache[i].mac;
    return (const uint8_t*)0;
}

/* Broadcast a who-has request; the reply lands in the cache later */
static void arp_request(const uint8_t* ip) {
    eth_hdr_t* eth = (eth_hdr_t*)frame_out;
    memset(eth->dst, 0xFF, 6);
    memcpy(eth->src, rtl8139_mac(), 6);
    eth->type = htons16(ETH_ARP);

    arp_pkt_t* arp = (arp_pkt_t*)(frame_out + sizeof(eth_hdr_t));
    arp->htype = htons16(1);
    arp->ptype = htons16(ETH_IP);
    arp->hlen = 6; arp->plen = 4;
    arp->oper = htons16(1);
    memcpy(arp->sha, rtl8139_mac(), 6);
    memcpy(arp->spa, our_ip, 4);
    memset(arp->tha, 0, 6);
    memcpy(arp->tpa, ip, 4);

    rtl8139_send(frame_out, sizeof(eth_hdr_t) + sizeof(arp_pkt_t));
}

/* ──────── Protocol handlers ──────── */

static void handle_arp(const eth_hdr_t* eth, const arp_pkt_t* arp) {
    (void)eth;
    /* Learn the sender from every ARP we see (requests AND replies) */
    arp_learn(arp->spa, arp->sha);

    if (htons16(arp->oper) != 1) return;                 /* Requests only */
    if (memcmp(arp->tpa, our_ip, 4) != 0) return;        /* For us? */

    eth_hdr_t* reth = (eth_hdr_t*)frame_out;
    memcpy(reth->dst, eth->src, 6);
    memcpy(reth->src, rtl8139_mac(), 6);
    reth->type = htons16(ETH_ARP);

    arp_pkt_t* rarp = (arp_pkt_t*)(frame_out + sizeof(eth_hdr_t));
    rarp->htype = htons16(1);
    rarp->ptype = htons16(ETH_IP);
    rarp->hlen = 6; rarp->plen = 4;
    rarp->oper = htons16(2);                             /* Reply */
    memcpy(rarp->sha, rtl8139_mac(), 6);
    memcpy(rarp->spa, our_ip, 4);
    memcpy(rarp->tha, arp->sha, 6);
    memcpy(rarp->tpa, arp->spa, 4);

    rtl8139_send(frame_out, sizeof(eth_hdr_t) + sizeof(arp_pkt_t));
}

static void handle_icmp(const eth_hdr_t* eth, const ip_hdr_t* ip,
                        const uint8_t* payload, uint32_t len) {
    const icmp_hdr_t* icmp = (const icmp_hdr_t*)payload;
    if (icmp->type != 8) return;                         /* Echo request */

    static uint8_t reply[1500];
    if (len > sizeof(reply)) return;
    memcpy(reply, payload, len);
    icmp_hdr_t* r = (icmp_hdr_t*)reply;
    r->type = 0;                                         /* Echo reply */
    r->checksum = 0;
    r->checksum = htons16(checksum16(reply, len, 0));
    send_ip(eth->src, ip->src, IP_ICMP, reply, len);
}

/* ──────── UDP ──────── */

static void udp_tx(const uint8_t* dst_mac, const uint8_t* dst_ip,
                   uint16_t sport, uint16_t dport,
                   const void* data, uint32_t len) {
    static uint8_t dgram[sizeof(udp_hdr_t) + UDP_MSG_MAX];
    udp_hdr_t* udp = (udp_hdr_t*)dgram;
    udp->sport    = htons16(sport);
    udp->dport    = htons16(dport);
    udp->len      = htons16((uint16_t)(sizeof(udp_hdr_t) + len));
    udp->checksum = 0;                    /* Optional in IPv4 */
    memcpy(dgram + sizeof(udp_hdr_t), data, len);
    send_ip(dst_mac, dst_ip, IP_UDP, dgram, sizeof(udp_hdr_t) + len);
}

static void handle_udp(const eth_hdr_t* eth, const ip_hdr_t* ip,
                       const uint8_t* payload, uint32_t len) {
    if (len < sizeof(udp_hdr_t)) return;
    const udp_hdr_t* udp = (const udp_hdr_t*)payload;
    uint32_t dlen = htons16(udp->len);
    if (dlen < sizeof(udp_hdr_t) || dlen > len) return;
    dlen -= sizeof(udp_hdr_t);
    const uint8_t* data = payload + sizeof(udp_hdr_t);
    uint16_t dport = htons16(udp->dport);

    arp_learn(ip->src, eth->src);         /* Free reverse path */

    if (dport == UDP_ECHO_PORT) {         /* RFC 862, the self-test */
        if (dlen > UDP_MSG_MAX) dlen = UDP_MSG_MAX;
        udp_tx(eth->src, ip->src, UDP_ECHO_PORT, htons16(udp->sport),
               data, dlen);
        return;
    }

    if (udp_port == 0 || dport != udp_port) return;
    if (dlen > UDP_MSG_MAX) return;       /* Game datagrams are small */
    if (udp_tail - udp_head >= UDP_QUEUE) return;   /* Queue full: drop */

    uint32_t slot = udp_tail % UDP_QUEUE;
    udp_q[slot].len   = (uint16_t)dlen;
    udp_q[slot].sport = htons16(udp->sport);
    memcpy(udp_q[slot].sip, ip->src, 4);
    memcpy(udp_q[slot].data, data, dlen);
    udp_tail++;
}

static void handle_tcp(const eth_hdr_t* eth, const ip_hdr_t* ip,
                       const uint8_t* payload, uint32_t len) {
    const tcp_hdr_t* tcp = (const tcp_hdr_t*)payload;
    if (htons16(tcp->dport) != HTTP_PORT) return;

    uint32_t hdr_len = (uint32_t)(tcp->offset >> 4) * 4;
    if (hdr_len < 20 || hdr_len > len) return;
    const uint8_t* data = payload + hdr_len;
    uint32_t data_len = len - hdr_len;
    uint32_t seq = htonl32(tcp->seq);

    if (tcp->flags & TCP_RST) {
        conn.state = TCP_LISTEN;
        return;
    }

    /* New connection (also usurps a stale one — one slot only) */
    if (tcp->flags & TCP_SYN) {
        memcpy(conn.peer_mac, eth->src, 6);
        memcpy(conn.peer_ip, ip->src, 4);
        conn.peer_port = htons16(tcp->sport);
        conn.rcv_nxt   = seq + 1;
        conn.snd_nxt   = system_ticks * 7919 + 12345;    /* "ISN" */
        conn.req_len   = 0;
        conn.state     = TCP_SYN_RCVD;
        tcp_send(TCP_SYN | TCP_ACK, 0, 0);
        return;
    }

    /* Everything below concerns the active peer only */
    if (conn.state == TCP_LISTEN) return;
    if (memcmp(ip->src, conn.peer_ip, 4) != 0 ||
        htons16(tcp->sport) != conn.peer_port) return;

    switch (conn.state) {
        case TCP_SYN_RCVD:
            if (tcp->flags & TCP_ACK) conn.state = TCP_ESTAB;
            /* fall through: the ACK may carry data already */
            __attribute__((fallthrough));

        case TCP_ESTAB:
            if (data_len > 0 && seq == conn.rcv_nxt) {
                conn.rcv_nxt += data_len;

                uint32_t space = sizeof(conn.req) - 1 - conn.req_len;
                uint32_t take = (data_len < space) ? data_len : space;
                memcpy(conn.req + conn.req_len, data, take);
                conn.req_len += take;
                conn.req[conn.req_len] = '\0';

                if (strstr_simple(conn.req, "\r\n\r\n")) {
                    http_respond();          /* Sends data + FIN */
                } else {
                    tcp_send(TCP_ACK, 0, 0);
                }
            }
            if (tcp->flags & TCP_FIN) {      /* Peer gave up early */
                conn.rcv_nxt += 1;
                tcp_send(TCP_ACK | TCP_FIN, 0, 0);
                conn.state = TCP_FIN_SENT;
            }
            break;

        case TCP_FIN_SENT:
            if (tcp->flags & TCP_FIN) {
                conn.rcv_nxt += 1;
                tcp_send(TCP_ACK, 0, 0);
                conn.state = TCP_LISTEN;
            } else if (tcp->flags & TCP_ACK) {
                /* Pure ACK of our FIN: linger until the peer FINs */
            }
            break;

        default:
            break;
    }
}

/* ──────── Public API ──────── */

/* ──────── UDP public API (backs SYS_NET) ──────── */

uint32_t net_local_ip(void) {
    if (!net_up) return 0;
    return ((uint32_t)our_ip[0] << 24) | ((uint32_t)our_ip[1] << 16)
         | ((uint32_t)our_ip[2] << 8)  |  (uint32_t)our_ip[3];
}

int net_udp_bind(uint16_t port) {
    if (!net_up || port == 0 || port == HTTP_PORT || port == UDP_ECHO_PORT)
        return -1;
    udp_port = port;
    udp_head = udp_tail = 0;
    return 0;
}

int net_udp_send(uint32_t dst_ip, uint16_t dst_port,
                 const void* buf, uint32_t len) {
    if (!net_up || udp_port == 0 || len > UDP_MSG_MAX) return -1;

    uint8_t ip[4] = {
        (uint8_t)(dst_ip >> 24), (uint8_t)(dst_ip >> 16),
        (uint8_t)(dst_ip >> 8),  (uint8_t)dst_ip,
    };
    const uint8_t* mac = arp_find(ip);
    if (!mac) {
        arp_request(ip);        /* Resolve in the background */
        return -2;              /* Caller retries next frame */
    }
    udp_tx(mac, ip, udp_port, dst_port, buf, len);
    return 0;
}

int net_udp_recv(void* buf, uint32_t maxlen,
                 uint32_t* src_ip, uint16_t* src_port) {
    if (udp_head == udp_tail) return -1;

    uint32_t slot = udp_head % UDP_QUEUE;
    uint32_t n = udp_q[slot].len;
    if (n > maxlen) n = maxlen;
    memcpy(buf, udp_q[slot].data, n);
    if (src_ip)
        *src_ip = ((uint32_t)udp_q[slot].sip[0] << 24)
                | ((uint32_t)udp_q[slot].sip[1] << 16)
                | ((uint32_t)udp_q[slot].sip[2] << 8)
                |  (uint32_t)udp_q[slot].sip[3];
    if (src_port) *src_port = udp_q[slot].sport;
    udp_head++;
    return (int)n;
}

void net_init(void) {
    if (!rtl8139_init()) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
        terminal_writestring("[NET] No NIC - REST API disabled\n");
        return;
    }
    conn.state = TCP_LISTEN;
    net_up = 1;
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[NET] IP 10.0.2.15, REST API on port 80\n");
}

void net_poll(void) {
    if (!net_up) return;

    for (int budget = 0; budget < 8; budget++) {
        uint32_t len = rtl8139_recv(frame_in, sizeof(frame_in));
        if (len == 0) return;
        if (len < sizeof(eth_hdr_t)) continue;

        const eth_hdr_t* eth = (const eth_hdr_t*)frame_in;
        uint16_t type = htons16(eth->type);

        if (type == ETH_ARP && len >= sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) {
            handle_arp(eth, (const arp_pkt_t*)(frame_in + sizeof(eth_hdr_t)));
        } else if (type == ETH_IP && len >= sizeof(eth_hdr_t) + 20) {
            const ip_hdr_t* ip = (const ip_hdr_t*)(frame_in + sizeof(eth_hdr_t));
            if ((ip->ver_ihl >> 4) != 4) continue;
            if (memcmp(ip->dst, our_ip, 4) != 0) continue;

            uint32_t ihl = (uint32_t)(ip->ver_ihl & 0xF) * 4;
            uint32_t tot = htons16(ip->total_len);
            if (tot < ihl || sizeof(eth_hdr_t) + tot > len) continue;

            const uint8_t* payload = frame_in + sizeof(eth_hdr_t) + ihl;
            uint32_t plen = tot - ihl;

            if (ip->proto == IP_ICMP)     handle_icmp(eth, ip, payload, plen);
            else if (ip->proto == IP_TCP) handle_tcp(eth, ip, payload, plen);
            else if (ip->proto == IP_UDP) handle_udp(eth, ip, payload, plen);
        }
    }
}
