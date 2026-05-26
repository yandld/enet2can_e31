/*
 * Ethernet lwIP bring-up boundary for FRDM-MCXE31B.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ETHERNET_LWIP_H_
#define ETHERNET_LWIP_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    bool linkUp;
    bool dhcpBound;
    uint32_t ipv4Addr;
} ethernet_lwip_status_t;

bool ethernet_lwip_init(void);
void ethernet_lwip_poll(void);
ethernet_lwip_status_t ethernet_lwip_get_status(void);

#endif /* ETHERNET_LWIP_H_ */
