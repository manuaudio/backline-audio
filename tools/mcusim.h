/* A software Mackie Control / X-Touch: the device side of the protocol,
 * with motor faders, touch, LEDs, rings, a 2x56 LCD, X-Touch colours,
 * meters and the 7-segment displays. It answers the device query like the
 * hardware and records everything it is sent, so tests can check what the
 * engineer would see. Pure; tools/mcu-sim.c puts it on PipeWire.
 * SPDX-License-Identifier: MIT
 */
#ifndef BACKLINE_MCUSIM_H
#define BACKLINE_MCUSIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcu.h"

typedef void (*mcusim_out)(void *data, const uint8_t *msg, size_t n);

struct mcusim {
	char model_name[24];
	uint8_t model;
	bool master, colour_lcd;
	int strips;
	uint8_t serial[7];
	uint8_t challenge[4];
	bool connected;             /* the host answered (Mackie: query seen; Logic: right reply) */
	bool offline;               /* the host sent "go offline" */
	int fader[9];               /* where the fader is (14-bit) */
	int host_fader[9];          /* where the host last asked the motor to go */
	bool touched[9];
	uint8_t led[MCU_NOTES];     /* 0 off, 1 flash, 2 on */
	uint8_t ring[8];
	char lcd[MCU_LCD_CELLS];
	uint8_t colour[8];
	int meter[8];
	bool clip[8];
	uint8_t tc[MCU_TC_DIGITS], assign[2];
	uint64_t rx, rx_fader, rx_led, rx_ring, rx_lcd, rx_meter, rx_colour, rx_query, rx_other, lcd_bytes;
	char rxlog[64][52];          /* the last messages received, in hex (oldest first in the dump) */
	unsigned rxlog_n;
	mcusim_out out;
	void *data;
};

/* model: xtouch, xtouch-ext, mcu-pro, mcu-xt. serial: up to 7 characters. */
int mcusim_init(struct mcusim *s, const char *model, const char *serial, mcusim_out out, void *data);
/* Power cycle: everything dark, faders at the bottom. */
void mcusim_reset(struct mcusim *s);
/* One complete message from the host. */
void mcusim_receive(struct mcusim *s, const uint8_t *msg, size_t n);

/* The engineer: i is 0-7 for strips, 8 for the master fader. */
void mcusim_touch(struct mcusim *s, int i, bool on);
void mcusim_move(struct mcusim *s, int i, int v14);
void mcusim_press(struct mcusim *s, int id, bool down);
void mcusim_pot(struct mcusim *s, int i, int delta);
void mcusim_jog(struct mcusim *s, int delta);
/* The device's own host-connection query (what it sends at power-up). */
void mcusim_hello(struct mcusim *s);
/* Any bytes (a generic controller's CC, NRPN, ...): one message. */
void mcusim_raw(struct mcusim *s, const uint8_t *m, size_t n);

/* A line of the LCD (0 or 1), 56 characters. */
void mcusim_lcd_line(const struct mcusim *s, int line, char out[57]);
/* One strip's 7 cells of a line, trailing spaces removed. */
void mcusim_lcd_cell(const struct mcusim *s, int line, int strip, char out[8]);
/* The 7-segment displays as text (dots as '.'). */
void mcusim_tc_text(const struct mcusim *s, char *out, size_t n);
void mcusim_assign_text(const struct mcusim *s, char out[3]);
const char *mcusim_colour_name(uint8_t c);
/* The whole state as JSON (malloc'd). */
char *mcusim_state_json(const struct mcusim *s);

#endif
