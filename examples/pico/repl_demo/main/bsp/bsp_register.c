/* SPDX-License-Identifier: BSD-3-Clause */
#include "bsp_register.h"
#include "bsp_led.h"
#include "bsp_temp.h"
#include "bsp_button.h"
#include "bsp_tick.h"

int bsp_bind_event(struct UVM *vm, const char *name, urbi_event_id_t *out_id)
{
    int rc = urbi_event_register(vm, NULL, name, out_id);
    if (rc != URBI_OK) return rc;
    UValue ev = urbi_make_nil(), object = urbi_make_nil();
    rc = urbi_event_value(vm, *out_id, &ev);
    if (rc != URBI_OK) return rc;
    rc = urbi_global_get(vm, NULL, "Object", &object);
    if (rc != URBI_OK) return rc;
    return urbi_slot_set(vm, object, name, ev);
}

int bsp_register(struct UVM *vm)
{
    int rc;
    if ((rc = bsp_led_register(vm)) != URBI_OK) return rc;
    if ((rc = bsp_temp_register(vm)) != URBI_OK) return rc;
    if ((rc = bsp_button_register(vm)) != URBI_OK) return rc;
    if ((rc = bsp_tick_register(vm)) != URBI_OK) return rc;
    return URBI_OK;
}
