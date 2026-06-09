/*
 * status_led.c - three discrete active-low status LEDs (SYS / NET / CAN).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The LEDs share a common anode on 3V3 and sink through the MCU pin, so a pin
 * LOW lights the LED and a pin HIGH turns it off (active-low). Driven as plain
 * SIUL2 GPIO outputs; blink phases come from the lwIP millisecond clock.
 */
#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "can_service.h"
#include "ethernet_lwip.h"
#include "fsl_siul2.h"
#include "lwip/sys.h"

typedef struct
{
    siul2_port_num_t port;
    uint32_t pin;
} led_pin_t;

/* See the schematic: D1 is wired as three discrete LEDs, one MCU pin each. */
static const led_pin_t LED_SYS = {kSIUL2_PTC, 16U}; /* PTC16 - firmware heartbeat */
static const led_pin_t LED_NET = {kSIUL2_PTB, 22U}; /* PTB22 - Ethernet/IP state  */
static const led_pin_t LED_CAN = {kSIUL2_PTC, 14U}; /* PTC14 - CAN bus activity   */

static bool s_ready;

/* Active-low: drive the pin LOW to light the LED, HIGH to turn it off. */
static void led_set(led_pin_t led, bool on)
{
    SIUL2_PortPinWrite(SIUL2, led.port, led.pin, on ? 0U : 1U);
}

/* Configure one pin as a GPIO output (mirrors the output-pin setup in pin_mux.c). */
static void led_init_pin(uint32_t pinPortIdx)
{
    const siul2_pin_settings_t cfg = {
        .base            = SIUL2,
        .pinPortIdx      = pinPortIdx,
        .mux             = kPORT_MUX_AS_GPIO,
        .safeMode        = kPORT_SAFE_MODE_DISABLED,
        .inputFilter     = kPORT_INPUT_FILTER_NOT_AVAILABLE,
        .driveStrength   = kPORT_DRIVE_STRENTGTH_NOT_AVAILABLE,
        .pullConfig      = kPORT_INTERNAL_PULL_NOT_ENABLED,
        .slewRateCtrlSel = kPORT_SLEW_RATE_NOT_AVAILABLE,
        .pullKeep        = kPORT_PULL_KEEP_DISABLED,
        .invert          = kPORT_INVERT_DISABLED,
        .inputBuffer     = kPORT_INPUT_BUFFER_DISABLED,
        .outputBuffer    = kPORT_OUTPUT_BUFFER_ENABLED,
        .adcInterleaves  = {kMUX_MODE_NOT_AVAILABLE, kMUX_MODE_NOT_AVAILABLE},
        .initValue       = 1U, /* HIGH = LED off */
    };
    SIUL2_PinInit(&cfg);
}

void status_led_init(void)
{
    led_init_pin((uint32_t)LED_SYS.port * 32U + LED_SYS.pin);
    led_init_pin((uint32_t)LED_NET.port * 32U + LED_NET.pin);
    led_init_pin((uint32_t)LED_CAN.port * 32U + LED_CAN.pin);

    led_set(LED_SYS, false);
    led_set(LED_NET, false);
    led_set(LED_CAN, false);
    s_ready = true;
}

void status_led_poll(void)
{
    static uint32_t s_next;      /* next refresh time (ms) */
    static uint32_t s_canCount;  /* last summed CAN rx+tx frame count */
    static uint32_t s_canActMs;  /* time of last observed CAN activity */
    static bool s_countPrimed;

    uint32_t now;

    if (!s_ready)
    {
        return;
    }

    now = sys_now();
    if ((int32_t)(now - s_next) < 0) /* throttle: ~20 ms is plenty for the eye */
    {
        return;
    }
    s_next = now + 20U;

    /* --- gather state --- */
    ethernet_lwip_status_t eth = ethernet_lwip_get_status();

    bool canFault     = false;
    uint32_t canCount = 0U;
    for (uint8_t ch = 0U; ch < CAN_SERVICE_CHANNEL_COUNT; ch++)
    {
        can_service_status_t st = can_service_get_status(ch);
        if (st.enabled && (st.state >= CAN_SERVICE_STATE_ERROR_PASSIVE))
        {
            canFault = true;
        }
        canCount += st.rxCount + st.txStartCount;
    }
    if (!s_countPrimed)
    {
        s_canCount    = canCount;
        s_countPrimed = true;
    }
    if (canCount != s_canCount)
    {
        s_canCount = canCount;
        s_canActMs = now;
    }

    /* --- LED1 SYS: ~1 Hz heartbeat while the loop is running --- */
    led_set(LED_SYS, (now % 1000U) < 500U);

    /* --- LED2 NET: off / waiting-for-IP blink / solid-ready --- */
    bool net;
    if (!eth.linkUp)
    {
        net = false;
    }
    else if (eth.ipSource == ETHERNET_LWIP_IP_NONE)
    {
        net = (now % 500U) < 250U; /* ~2 Hz: link up, no IP yet */
    }
    else
    {
        net = true; /* has IP: ready */
    }
    led_set(LED_NET, net);

    /* --- LED3 CAN: solid-error wins, else blink on traffic, else off --- */
    bool can;
    if (canFault)
    {
        can = true;
    }
    else if ((now - s_canActMs) < 120U)
    {
        can = (now % 120U) < 60U; /* blink while frames are flowing */
    }
    else
    {
        can = false;
    }
    led_set(LED_CAN, can);
}

void status_led_fault(void)
{
    /* SYS solid, NET/CAN off: a clear "stopped on a fault" panel. */
    SIUL2_PortPinWrite(SIUL2, LED_SYS.port, LED_SYS.pin, 0U);
    SIUL2_PortPinWrite(SIUL2, LED_NET.port, LED_NET.pin, 1U);
    SIUL2_PortPinWrite(SIUL2, LED_CAN.port, LED_CAN.pin, 1U);
}
