/*
 * Ethernet lwIP bring-up boundary for FRDM-MCXE31B.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ETHERNET_LWIP_H_
#define ETHERNET_LWIP_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    ETHERNET_LWIP_IP_NONE = 0, /* link down, or link up but no address yet */
    ETHERNET_LWIP_IP_DHCP,     /* address leased from a DHCP server */
    ETHERNET_LWIP_IP_STATIC,   /* static fallback address (no DHCP) */
} ethernet_lwip_ip_source_t;

typedef struct
{
    bool linkUp;
    ethernet_lwip_ip_source_t ipSource;
    uint32_t ipv4Addr;
} ethernet_lwip_status_t;

bool ethernet_lwip_init(void);
void ethernet_lwip_poll(void);
ethernet_lwip_status_t ethernet_lwip_get_status(void);

#endif /* ETHERNET_LWIP_H_ */
