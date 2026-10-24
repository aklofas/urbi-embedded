/* SPDX-License-Identifier: BSD-3-Clause */
/* An in-memory byte stream as a UTransport: the smoke feeds a request
 * into one buffer and takes the session's reply out of the other. */
#ifndef SMOKE_TRANSPORT_H
#define SMOKE_TRANSPORT_H
#include <stddef.h>
#include "urbi/repl.h"

void   smoke_transport_vtable(UTransport *out);
void   smoke_transport_feed(const char *s, size_t n);
size_t smoke_transport_take(char *buf, size_t cap);

#endif
