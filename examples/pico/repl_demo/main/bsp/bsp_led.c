/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/bsp/bsp_led.c
 *
 * GPIO25 on-board LED fixture for the Pi Pico REPL demo.
 *
 * Hardware: GP25 is the on-board LED on Pico (RP2040).  PWM uses PWM
 * slice 4, channel B (the slice that drives GP24/GP25).  Duty is
 * clamped to [0.0, 1.0] and quantized to 16-bit (wrap = 65535). */

#include "bsp_led.h"
#include "urbi/urbi.h"
#include "urbi/types.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"

#define PICO_LED_PIN        25U
#define PICO_LED_PWM_WRAP   65535U
#define PICO_LED_PWM_SLICE  4U     /* pwm_gpio_to_slice_num(25) */
#define PICO_LED_PWM_CHAN   PWM_CHAN_B  /* GP25 is channel B of slice 4 */

static bool s_pwm_inited = false;

/* Lazily enable PWM mode on GP25.  Called by led_pwm; led_on/off/toggle
 * stay in GPIO mode and re-init the pin as GPIO_FUNC_SIO on entry to
 * undo any prior PWM hand-off. */
static void led_pwm_init_once(void)
{
    if (s_pwm_inited) {
        return;
    }
    gpio_set_function(PICO_LED_PIN, GPIO_FUNC_PWM);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, PICO_LED_PWM_WRAP);
    pwm_init(PICO_LED_PWM_SLICE, &cfg, true);
    s_pwm_inited = true;
}

/* Force GP25 back to SIO/GPIO mode (overrides any prior PWM hand-off). */
static void led_gpio_mode(void)
{
    gpio_init(PICO_LED_PIN);
    gpio_set_dir(PICO_LED_PIN, GPIO_OUT);
}

static int c_led_on(struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    led_gpio_mode();
    gpio_put(PICO_LED_PIN, 1);
    *out = urbi_make_nil();
    return UEXEC_OK;
}

static int c_led_off(struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    led_gpio_mode();
    gpio_put(PICO_LED_PIN, 0);
    *out = urbi_make_nil();
    return UEXEC_OK;
}

static int c_led_toggle(struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    led_gpio_mode();
    gpio_xor_mask(1U << PICO_LED_PIN);
    *out = urbi_make_nil();
    return UEXEC_OK;
}

static int c_led_pwm(struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)nargs;   /* arity is checked by the VM: exactly one */
    double duty;
    if (urbi_value_kind(args[0]) == URBI_VALUE_INT)        duty = (double)urbi_value_as_int(args[0]);
    else if (urbi_value_kind(args[0]) == URBI_VALUE_FLOAT) duty = urbi_value_as_float(args[0]);
    else return urbi_throw(vm, "TypeError", "led_pwm: duty must be a number");
    if (duty < 0.0) duty = 0.0;
    if (duty > 1.0) duty = 1.0;
    led_pwm_init_once();
    pwm_set_chan_level(PICO_LED_PWM_SLICE, PICO_LED_PWM_CHAN, (uint16_t)(duty * (double)PICO_LED_PWM_WRAP + 0.5));
    *out = urbi_make_nil();
    return UEXEC_OK;
}

int bsp_led_register(struct UVM *vm)
{
    led_gpio_mode();
    gpio_put(PICO_LED_PIN, 0);
    int rc;
    if ((rc = urbi_register(vm, "Lobby.led_on",     c_led_on,     0, 0)) != URBI_OK) return rc;
    if ((rc = urbi_register(vm, "Lobby.led_off",    c_led_off,    0, 0)) != URBI_OK) return rc;
    if ((rc = urbi_register(vm, "Lobby.led_toggle", c_led_toggle, 0, 0)) != URBI_OK) return rc;
    return         urbi_register(vm, "Lobby.led_pwm",    c_led_pwm,    1, 1);
}
