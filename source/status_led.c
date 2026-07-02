/*
 * status_led.c - active-low SYS / NET / CAN status LEDs.
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "can_hw.h"
#include "e2cf_config.h"
#include "e2cf_core.h"
#include "e2cf_proto.h"
#include "eth_raw.h"
#include "fsl_siul2.h"

typedef struct
{
    siul2_port_num_t port;
    uint32_t pin;
} led_pin_t;

static const led_pin_t LED_SYS = { kSIUL2_PTC, 16U };
static const led_pin_t LED_NET = { kSIUL2_PTB, 22U };
static const led_pin_t LED_CAN = { kSIUL2_PTC, 14U };

static bool s_ready;

static void led_set(led_pin_t led, bool on)
{
    SIUL2_PortPinWrite(SIUL2, led.port, led.pin, on ? 0U : 1U);
}

static void led_init_pin(uint32_t pin_port_idx)
{
    const siul2_pin_settings_t cfg = {
        .base = SIUL2,
        .pinPortIdx = pin_port_idx,
        .mux = kPORT_MUX_AS_GPIO,
        .safeMode = kPORT_SAFE_MODE_DISABLED,
        .inputFilter = kPORT_INPUT_FILTER_NOT_AVAILABLE,
        .driveStrength = kPORT_DRIVE_STRENTGTH_NOT_AVAILABLE,
        .pullConfig = kPORT_INTERNAL_PULL_NOT_ENABLED,
        .slewRateCtrlSel = kPORT_SLEW_RATE_NOT_AVAILABLE,
        .pullKeep = kPORT_PULL_KEEP_DISABLED,
        .invert = kPORT_INVERT_DISABLED,
        .inputBuffer = kPORT_INPUT_BUFFER_DISABLED,
        .outputBuffer = kPORT_OUTPUT_BUFFER_ENABLED,
        .adcInterleaves = { kMUX_MODE_NOT_AVAILABLE, kMUX_MODE_NOT_AVAILABLE },
        .initValue = 1U,
    };

    SIUL2_PinInit(&cfg);
}

static bool can_has_fault(void)
{
    for (uint8_t ch = 0U; ch < E2CF_NUM_CHANNELS; ch++)
    {
        e2cf_evt_rec_t evt;

        if (((E2CF_ACTIVE_CHAN_MASK & (1UL << ch)) == 0U) || !can_hw_is_running(ch))
        {
            continue;
        }
        can_hw_get_evt(ch, &evt);
        if (evt.state >= E2CF_STATE_ERROR_PASSIVE)
        {
            return true;
        }
    }
    return false;
}

static uint32_t can_activity_count(void)
{
    uint32_t count = 0U;

    for (uint8_t ch = 0U; ch < E2CF_NUM_CHANNELS; ch++)
    {
        const can_hw_stats_t *stats;

        if ((E2CF_ACTIVE_CHAN_MASK & (1UL << ch)) == 0U)
        {
            continue;
        }
        stats = can_hw_stats(ch);
        if (stats != NULL)
        {
            count += stats->rx_frames + stats->tx_frames;
        }
    }
    return count;
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

void status_led_poll(uint32_t now_ms)
{
    static uint32_t s_next_ms;
    static uint32_t s_last_can_count;
    static uint32_t s_last_can_activity_ms;
    static bool s_can_count_valid;
    uint32_t can_count;
    bool net;
    bool can;

    if (!s_ready || ((int32_t)(now_ms - s_next_ms) < 0))
    {
        return;
    }
    s_next_ms = now_ms + 20U;

    led_set(LED_SYS, (now_ms % 1000U) < 500U);

    if (!eth_raw_link_up())
    {
        net = false;
    }
    else if (!e2cf_core_link_ready())
    {
        net = (now_ms % 500U) < 250U;
    }
    else
    {
        net = true;
    }
    led_set(LED_NET, net);

    can_count = can_activity_count();
    if (!s_can_count_valid)
    {
        s_last_can_count = can_count;
        s_last_can_activity_ms = now_ms - 120U;
        s_can_count_valid = true;
    }
    else if (can_count != s_last_can_count)
    {
        s_last_can_count = can_count;
        s_last_can_activity_ms = now_ms;
    }

    if (can_has_fault())
    {
        can = true;
    }
    else if ((uint32_t)(now_ms - s_last_can_activity_ms) < 120U)
    {
        can = (now_ms % 120U) < 60U;
    }
    else
    {
        can = false;
    }
    led_set(LED_CAN, can);
}

void status_led_fault(void)
{
    led_set(LED_SYS, true);
    led_set(LED_NET, false);
    led_set(LED_CAN, false);
}
