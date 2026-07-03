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

#endif /* NET_H */
