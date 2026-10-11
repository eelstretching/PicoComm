// lwIP configuration for PicoComm: UDP only, poll mode (pico_cyw43_arch_lwip_poll, all of lwIP
// runs on core 1's loop). Started from universal-strip-controller's wifi_rtc_sync settings with
// TCP's buffers shrunk; TCP stays compiled in only because DHCP and the cyw43 glue are happiest
// that way, and costs little with these sizes.
//
// A prop firmware that needs other lwIP features can put its own lwipopts.h earlier on the
// include path.
#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

#define NO_SYS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#if PICO_CYW43_ARCH_POLL
#define MEM_LIBC_MALLOC 1
#else
#define MEM_LIBC_MALLOC 0
#endif
#define MEM_ALIGNMENT 4
#define MEM_SIZE 4000
#define MEMP_NUM_UDP_PCB 4
#define MEMP_NUM_TCP_PCB 2
#define MEMP_NUM_TCP_SEG 8
#define MEMP_NUM_ARP_QUEUE 10
#define PBUF_POOL_SIZE 16
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 0
#define LWIP_IPV4 1
#define LWIP_UDP 1
#define LWIP_TCP 1
#define TCP_MSS 536
#define TCP_WND (2 * TCP_MSS)
#define TCP_SND_BUF (2 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_DHCP 1
#define LWIP_DNS 0
#define DHCP_DOES_ARP_CHECK 0
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define LWIP_CHKSUM_ALGORITHM 3
// SOF_BROADCAST on the pcb is needed to send the leader's beacons.
#define IP_SOF_BROADCAST 1
#define IP_SOF_BROADCAST_RECV 0

#define MEM_STATS 0
#define SYS_STATS 0
#define MEMP_STATS 0
#define LINK_STATS 0
#define LWIP_STATS 0

#endif
