/**
 * @file  radio_if.c
 * @brief Abstract radio HAL — vtable registration and dispatch.
 *
 * This file owns the single global vtable pointer `g_radio_if`.
 * All concrete driver code lives in radio_sx1262.c / radio_sx1276.c.
 * This file ONLY manages registration bookkeeping.
 *
 * BACKWARD COMPATIBILITY
 * ──────────────────────
 * Existing callers that call radio_sx1262_* directly are unaffected.
 * The radio_if layer is optional / additive; it is not required by the
 * main loop until migration is complete.
 */

#include "radio_if.h"
#include <stdio.h>

/* ── Registered vtable ──────────────────────────────────────────────────── */

/** Currently registered radio driver vtable (NULL until radio_if_register). */
const radio_if_vtable_t *g_radio_if = NULL;

/* ── Public API ─────────────────────────────────────────────────────────── */

void radio_if_register(const radio_if_vtable_t *vtable)
{
    g_radio_if = vtable;
}

const radio_if_vtable_t *radio_if_get(void)
{
    return g_radio_if;
}
