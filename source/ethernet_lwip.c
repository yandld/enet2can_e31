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
#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#define ETHERNET_LWIP_CLOCK_FREQ CLOCK_GetAipsPlatClkFreq()

/*
 * Networking is DHCP-first with a static fallback, so the board is reachable on
 * any network: it waits ETHERNET_LWIP_DHCP_FALLBACK_MS (timed from link up) for a
 * DHCP lease, then falls back to the static address below. DHCP keeps running in
 * the background after fallback - if a server appears later its lease takes over
 * automatically (the assigned IP is logged on every change, see the report below).
 *
 * Set ETHERNET_LWIP_USE_DHCP to 0 (e.g. in the build defines) for a no-DHCP
 * deployment: the board comes up on the static address with no startup wait.
 *
 * Pick the static address OUTSIDE the DHCP pool, so it cannot collide with a
 * lease if both a DHCP server and this fallback exist on the same subnet.
 */
#ifndef ETHERNET_LWIP_USE_DHCP
#define ETHERNET_LWIP_USE_DHCP 1
#endif
#define ETHERNET_LWIP_STATIC_IP        "192.168.8.113"
#define ETHERNET_LWIP_STATIC_MASK      "255.255.255.0"
#define ETHERNET_LWIP_STATIC_GW        "192.168.8.1"
#define ETHERNET_LWIP_DHCP_FALLBACK_MS 10000U

extern phy_lan8741_resource_t g_phy_resource;

static phy_handle_t s_phyHandle;
static struct netif s_netif;
static ethernet_lwip_status_t s_status;
static bool s_initialized;

/* Last (source, address) actually logged, so the IP is printed once and again on
 * every change - that single "Ethernet: IP = ..." line is what the operator reads. */
static bool s_reportValid;
static ethernet_lwip_ip_source_t s_reportSource;
static uint32_t s_reportAddr;

#if ETHERNET_LWIP_USE_DHCP
static bool s_prevLinkUp;
static uint32_t s_dhcpDeadlineMs; /* sys_now() at which we stop waiting and go static */
static bool s_fellBack;           /* static fallback applied for this link-up session */

static void ethernet_lwip_set_static(void)
{
    ip4_addr_t ip, mask, gw;

    (void)ip4addr_aton(ETHERNET_LWIP_STATIC_IP, &ip);
    (void)ip4addr_aton(ETHERNET_LWIP_STATIC_MASK, &mask);
    (void)ip4addr_aton(ETHERNET_LWIP_STATIC_GW, &gw);
    netif_set_addr(&s_netif, &ip, &mask, &gw);
}
#endif

/* Print the IP whenever it (or its source) changes; stay quiet otherwise. */
static void ethernet_lwip_report(void)
{
    if (s_reportValid && (s_status.ipSource == s_reportSource) && (s_status.ipv4Addr == s_reportAddr))
    {
        return;
    }
    s_reportValid  = true;
    s_reportSource = s_status.ipSource;
    s_reportAddr   = s_status.ipv4Addr;

    switch (s_status.ipSource)
    {
        case ETHERNET_LWIP_IP_DHCP:
            PRINTF("Ethernet: IP = %s  (DHCP)\r\n", ip4addr_ntoa(netif_ip4_addr(&s_netif)));
            break;
        case ETHERNET_LWIP_IP_STATIC:
            PRINTF("Ethernet: IP = %s  (static)\r\n", ip4addr_ntoa(netif_ip4_addr(&s_netif)));
            break;
        default:
            PRINTF(s_status.linkUp ? "Ethernet: link up, waiting for DHCP...\r\n"
                                   : "Ethernet: link down\r\n");
            break;
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

    enetConfig.phyHandle   = &s_phyHandle;
    enetConfig.phyAddr     = BOARD_EMAC_PHY_ADDRESS;
    enetConfig.phyOps      = &phylan8741_ops;
    enetConfig.phyResource = &g_phy_resource;
    enetConfig.srcClockHz  = ETHERNET_LWIP_CLOCK_FREQ;

    (void)SILICONID_ConvertToMacAddr(&enetConfig.macAddress);

    lwip_init();

#if ETHERNET_LWIP_USE_DHCP
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
    PRINTF("Ethernet: DHCP enabled (fallback to %s after %us if no lease)\r\n",
           ETHERNET_LWIP_STATIC_IP, (unsigned)(ETHERNET_LWIP_DHCP_FALLBACK_MS / 1000U));
#else
    {
        ip4_addr_t ip, mask, gw;

        (void)ip4addr_aton(ETHERNET_LWIP_STATIC_IP, &ip);
        (void)ip4addr_aton(ETHERNET_LWIP_STATIC_MASK, &mask);
        (void)ip4addr_aton(ETHERNET_LWIP_STATIC_GW, &gw);

        if (netif_add(&s_netif, &ip, &mask, &gw, &enetConfig, ethernetif0_init, ethernet_input) == NULL)
        {
            PRINTF("Ethernet: netif add failed\r\n");
            return false;
        }
        netif_set_default(&s_netif);
        netif_set_up(&s_netif);
    }
    s_initialized = true;
    PRINTF("Ethernet: static IP mode\r\n");
#endif

    return true;
}

void ethernet_lwip_poll(void)
{
    bool linkUp;

    if (!s_initialized)
    {
        return;
    }

    ethernetif_input(&s_netif);
    sys_check_timeouts();

    linkUp = netif_is_link_up(&s_netif) ? true : false;

#if ETHERNET_LWIP_USE_DHCP
    if (linkUp && !s_prevLinkUp)
    {
        /* Link just came up: start a fresh DHCP-first window from a clean slate. */
        s_dhcpDeadlineMs = sys_now() + ETHERNET_LWIP_DHCP_FALLBACK_MS;
        s_fellBack       = false;
        netif_set_addr(&s_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
    }
    s_prevLinkUp = linkUp;

    if (!linkUp)
    {
        s_status.ipSource = ETHERNET_LWIP_IP_NONE;
    }
    else if (dhcp_supplied_address(&s_netif))
    {
        s_status.ipSource = ETHERNET_LWIP_IP_DHCP; /* lease (re)acquired - it wins over the fallback */
        s_fellBack        = false;
    }
    else if (s_fellBack)
    {
        s_status.ipSource = ETHERNET_LWIP_IP_STATIC; /* on static; DHCP still hunting in the background */
    }
    else if ((int32_t)(sys_now() - s_dhcpDeadlineMs) >= 0)
    {
        ethernet_lwip_set_static(); /* no lease in time: take the static address, keep DHCP running */
        s_fellBack        = true;
        s_status.ipSource = ETHERNET_LWIP_IP_STATIC;
    }
    else
    {
        s_status.ipSource = ETHERNET_LWIP_IP_NONE; /* still inside the DHCP-first window */
    }
#else
    s_status.ipSource = linkUp ? ETHERNET_LWIP_IP_STATIC : ETHERNET_LWIP_IP_NONE;
#endif

    s_status.linkUp   = linkUp;
    s_status.ipv4Addr = (s_status.ipSource == ETHERNET_LWIP_IP_NONE) ? 0U : netif_ip4_addr(&s_netif)->addr;

    ethernet_lwip_report();
}

ethernet_lwip_status_t ethernet_lwip_get_status(void)
{
    return s_status;
}
