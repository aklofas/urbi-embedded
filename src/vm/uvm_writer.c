/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_writer.c — Gap E pluggable I/O writer, Gap F time source, Gap S wake hook.
 *
 * Three public setters + urbi_vm_write:
 *   urbi_set_writer   — install / uninstall the channel writer callback (Gap E)
 *   urbi_vm_write     — emit one write through the active writer (Gap E)
 *   urbi_set_time_us  — install / uninstall the monotonic time source (Gap F)
 *   urbi_set_wake_fn  — install / uninstall the ISR wake callback (Gap S)
 *
 * All functions are NULL-safe on a NULL vm (no-op).
 * Thread safety: MAIN (writer and time); ISR or MAIN (wake_fn).
 */

#include "vm/uvm.h"
#include "urbi/urbi.h"

#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * Gap E: Pluggable I/O writer
 * =========================================================================
 *
 * Default writer: hosted builds route "cout"/"clog" to stdout, "cerr" to
 * stderr; all other channel names are silently discarded.  A trailing
 * newline is appended when the message doesn't already end in one, matching
 * the legacy 2.x terminal experience.  Freestanding builds compile out the
 * stdio dependency entirely and use the silent sink.
 *
 * Implementation note: the default writer uses fwrite rather than fputs so
 * that messages containing embedded NULs are not truncated.  The channel
 * comparison is a byte-level check (no interning required) so this function
 * is safe to call from any point during VM lifecycle. */

#if __STDC_HOSTED__
#  include <stdio.h>

static int
channel_matches(const char *channel, size_t channel_len, const char *name)
{
    size_t i;
    for (i = 0; i < channel_len; i++) {
        if (channel[i] != name[i] || name[i] == '\0') return 0;
    }
    return name[channel_len] == '\0';
}

static void
default_writer(void *ud,
               const char *channel, size_t channel_len,
               const char *msg,     size_t msg_len,
               uint64_t ts_us)
{
    FILE *fp;
    (void)ud; (void)ts_us;

    if (channel_matches(channel, channel_len, "cout") ||
        channel_matches(channel, channel_len, "clog")) {
        fp = stdout;
    } else if (channel_matches(channel, channel_len, "cerr")) {
        fp = stderr;
    } else {
        /* Unknown channel: silently discard. */
        return;
    }

    if (msg_len > 0) {
        fwrite(msg, 1, msg_len, fp);
    }
    /* Append newline if message doesn't already end with one. */
    if (msg_len == 0 || msg[msg_len - 1] != '\n') {
        fputc('\n', fp);
    }
    fflush(fp);
}

#else /* freestanding */

/* Silent sink: no stdio available on freestanding targets.
 * Embedders MUST call urbi_set_writer to see any output. */
static void
default_writer(void *ud,
               const char *channel, size_t channel_len,
               const char *msg,     size_t msg_len,
               uint64_t ts_us)
{
    (void)ud; (void)channel; (void)channel_len;
    (void)msg; (void)msg_len; (void)ts_us;
}

#endif /* __STDC_HOSTED__ */

/* urbi_set_writer: install a custom channel writer on `vm`.
 * Pass writer=NULL to restore the default writer.
 * NULL vm is a no-op. */
void
urbi_set_writer(struct UVM *vm, urbi_writer_fn writer, void *ud)
{
    if (!vm) return;
    if (writer != NULL) {
        vm->writer_fn = writer;
        vm->writer_ud = ud;
    } else {
        /* Restore default: point at the static default_writer fn. */
        vm->writer_fn = default_writer;
        vm->writer_ud = NULL;
    }
}

/* urbi_vm_write: emit msg to channel through the installed writer.
 * If no writer has been installed, uses the default_writer.
 * ts_us is fetched from vm->host_time_us() when available.
 * NULL vm is a no-op. */
void
urbi_vm_write(struct UVM *vm,
              const char *channel, size_t channel_len,
              const char *msg,     size_t msg_len)
{
    if (!vm) return;

    uint64_t ts = 0U;
    if (vm->host_time_us != NULL) {
        ts = vm->host_time_us();
    }

    /* Resolve the active writer: installed hook or built-in default. */
    void (*wfn)(void *, const char *, size_t, const char *, size_t, uint64_t)
        = vm->writer_fn;
    void *wud = vm->writer_ud;
    if (wfn == NULL) {
        wfn = default_writer;
        wud = NULL;
    }
    wfn(wud, channel, channel_len, msg, msg_len, ts);
}

/* =========================================================================
 * Gap F: Pluggable time source
 * =========================================================================
 *
 * urbi_set_time_us installs fn as the monotonic-microseconds source.  Pass
 * NULL to restore the built-in default (clock_gettime on hosted, returns-0
 * on freestanding).  The default is already installed by urbi_vm_init via
 * the uvm_init.c default_host_time_us_stub; this setter lets embedders swap
 * it at boot without accessing the internal UVM struct directly.
 *
 * Note: the UVM field `host_time_us` is typed as `uint64_t (*)(void)`,
 * identical to `urbi_time_us_fn`.  The public typedef exists so embedders
 * have a named type to declare their BSP shim against.
 */

/* The default stub is defined (static) in uvm_init.c and is not exported.
 * To restore the default we re-run the same assignment that urbi_vm_init
 * performs.  We expose a file-scope function pointer via a thin helper in
 * uvm_init to avoid duplicating the #ifdef logic. */
extern uint64_t urbi_default_host_time_us(void);  /* declared in uvm_init.c */

void
urbi_set_time_us(struct UVM *vm, urbi_time_us_fn fn)
{
    if (!vm) return;
    if (fn != NULL) {
        vm->host_time_us = fn;
    } else {
        /* Restore default. */
        vm->host_time_us = urbi_default_host_time_us;
    }
}

/* =========================================================================
 * Gap S: Wake notification hook
 * =========================================================================
 *
 * urbi_set_wake_fn installs (or removes) the ISR-safe wake callback.
 * Pass fn=NULL to restore the default (silent: embedder polls urbi_step).
 *
 * The wake_fn is called from urbi_inject_event after each successful SPSC
 * ring deposit (see uevent_ring.c).  It may run from ISR context; callers
 * MUST ensure it is O(1), non-blocking, and non-allocating.
 */

void
urbi_set_wake_fn(struct UVM *vm, urbi_wake_fn fn, void *ud)
{
    if (!vm) return;
    vm->wake_fn = fn;   /* NULL is valid: disables the hook */
    vm->wake_ud = ud;
}
