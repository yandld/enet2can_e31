/*
 * Ethernet lwIP bring-up boundary for FRDM-MCXE31B.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ethernet_lwip.h"

#include <string.h>

#include "board.h"
#include "ethernetif.h"
#include "fsl_debug_console.h"
#include "fsl_phylan8741.h"
#include "fsl_phy.h"
#include "fsl_silicon_id.h"
#include "lwip/dhcp.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/prot/dhcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#define ETHERNET_LWIP_CLOCK_FREQ CLOCK_GetAipsPlatClkFreq()

extern phy_lan8741_resource_t g_phy_resource;

static phy_handle_t s_phyHandle;
static struct netif s_netif;
static netif_ext_callback_t s_linkStatusCallbackInfo;
static ethernet_lwip_status_t s_status;
static bool s_initialized;
static bool s_dhcpWaitingLogged;
static bool s_dhcpBoundLogged;

static void ethernet_lwip_link_callback(struct netif *netif,
                                        netif_nsc_reason_t reason,
                                        const netif_ext_callback_args_t *args)
{
    (void)netif;

    if (reason != LWIP_NSC_LINK_CHANGED)
    {
        return;
    }

    s_status.linkUp = args->link_changed.state ? true : false;
    if (s_status.linkUp)
    {
        PRINTF("Ethernet: link up\r\n");
        s_dhcpWaitingLogged = false;
    }
    else
    {
        PRINTF("Ethernet: link down\r\n");
        s_status.dhcpBound = false;
        s_status.ipv4Addr = 0U;
        s_dhcpBoundLogged = false;
    }
}

static void ethernet_lwip_update_dhcp_status(void)
{
    struct dhcp *dhcp = netif_dhcp_data(&s_netif);

    s_status.linkUp = netif_is_link_up(&s_netif) ? true : false;

    if ((dhcp != NULL) && (dhcp->state == DHCP_STATE_BOUND))
    {
        s_status.dhcpBound = true;
        s_status.ipv4Addr = netif_ip4_addr(&s_netif)->addr;

        if (!s_dhcpBoundLogged)
        {
            PRINTF("Ethernet: DHCP bound %s\r\n", ip4addr_ntoa(netif_ip4_addr(&s_netif)));
            s_dhcpBoundLogged = true;
        }
    }
    else
    {
        s_status.dhcpBound = false;
        s_status.ipv4Addr = 0U;
        s_dhcpBoundLogged = false;

        if (s_status.linkUp && !s_dhcpWaitingLogged)
        {
            PRINTF("Ethernet: DHCP waiting\r\n");
            s_dhcpWaitingLogged = true;
        }
    }
}

bool ethernet_lwip_init(void)
{
    ethernetif_config_t enetConfig;

    if (s_initialized)
    {
        return true;
    }

    memset(&s_netif, 0, sizeof(s_netif));
    memset(&s_status, 0, sizeof(s_status));
    memset(&enetConfig, 0, sizeof(enetConfig));

    enetConfig.phyHandle = &s_phyHandle;
    enetConfig.phyAddr = BOARD_EMAC_PHY_ADDRESS;
    enetConfig.phyOps = &phylan8741_ops;
    enetConfig.phyResource = &g_phy_resource;
    enetConfig.srcClockHz = ETHERNET_LWIP_CLOCK_FREQ;

    (void)SILICONID_ConvertToMacAddr(&enetConfig.macAddress);

    lwip_init();
    netif_add_ext_callback(&s_linkStatusCallbackInfo, ethernet_lwip_link_callback);

    if (netif_add(&s_netif, NULL, NULL, NULL, &enetConfig, ethernetif0_init, ethernet_input) == NULL)
    {
        PRINTF("Ethernet: netif add failed\r\n");
        return false;
    }

    netif_set_default(&s_netif);
    netif_set_up(&s_netif);

    if (dhcp_start(&s_netif) != ERR_OK)
    {
        PRINTF("Ethernet: DHCP start failed\r\n");
        return false;
    }

    s_initialized = true;
    PRINTF("Ethernet: DHCP enabled\r\n");
    ethernet_lwip_update_dhcp_status();

    return true;
}

void ethernet_lwip_poll(void)
{
    if (!s_initialized)
    {
        return;
    }

    ethernetif_input(&s_netif);
    sys_check_timeouts();
    ethernet_lwip_update_dhcp_status();
}

ethernet_lwip_status_t ethernet_lwip_get_status(void)
{
    return s_status;
}
