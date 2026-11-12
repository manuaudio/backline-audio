/* libbackline: what the library's own files share. Not installed.
 * SPDX-License-Identifier: MIT */
#ifndef BACKLINE_INTERNAL_H
#define BACKLINE_INTERNAL_H
#include <systemd/sd-bus.h>
#include "backline/backline.h"
sd_bus *bl__bus(bl_client *c);
int bl__fail_bus(bl_client *c, int r, const sd_bus_error *e, const char *what);
int bl__fail(bl_client *c, int r, const char *msg);
#endif
