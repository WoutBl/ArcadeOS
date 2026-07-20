#ifndef NET_H
#define NET_H

#include "types.h"

/*
 * ArcadeOS – networking
 *
 * rtl8139.c – Realtek RTL8139 NIC driver, fully polled (no IRQ): the
 *             idle task pumps net_poll() which drains the RX ring.
 * net.c     – minimal server-side stack: Ethernet, ARP (replies only),
 *             IPv4, ICMP echo (ping works), and a single-connection TCP
 *             server carrying the HTTP REST API on port 80.
 *
 * With QEMU user networking the guest is 10.0.2.15 and the host reaches
 * the API via a hostfwd rule, e.g.:  curl http://localhost:8080/api/status
 */

/* ──────── NIC driver (rtl8139.c) ──────── */

int  rtl8139_init(void);                 /* 0 = no NIC found */
int  rtl8139_present(void);
const uint8_t* rtl8139_mac(void);        /* 6 bytes */
void rtl8139_send(const void* frame, uint32_t len);
/* Returns frame length (copied into buf) or 0 if the ring is empty */
uint32_t rtl8139_recv(void* buf, uint32_t maxlen);

/* ──────── Stack + REST API (net.c) ──────── */

void net_init(void);      /* Probe the NIC, set up the stack */
void net_poll(void);      /* Drain RX + run the protocol handlers */

/* Live score reporting (SYS_SCORE): the running game pushes its score,
 * /api/status serves it while the game is alive. */
void score_report(int score, uint32_t task_id, const char* task_name);

/* ──────── UDP (netplay foundation, backs SYS_NET) ────────
 *
 * One datagram socket for the running game plus a built-in echo
 * service on port 7 (the permanent self-test:
 * `echo hi | nc -u localhost 8007` with the default hostfwd rule).
 * IPs are host-order uint32 (10.0.2.2 = 0x0A000202).
 */

/* Bind the game socket (replaces any previous binding, clears queue) */
int net_udp_bind(uint16_t port);

/* Send a datagram. Returns 0, -1 on error, or -2 if the destination
 * MAC is still being ARP-resolved — retry next frame. */
int net_udp_send(uint32_t dst_ip, uint16_t dst_port,
                 const void* buf, uint32_t len);

/* Kernel-internal beam transport (src/beam.c). */
int net_beam_send(uint32_t dst_ip, const void* buf, uint32_t len);

/* Dequeue one received datagram (bound port). Returns length, or -1
 * when the queue is empty. src_ip/src_port may be NULL. */
int net_udp_recv(void* buf, uint32_t maxlen,
                 uint32_t* src_ip, uint16_t* src_port);

/* Our IPv4 address (host-order), 0 when no NIC is up */
uint32_t net_local_ip(void);

#endif /* NET_H */
