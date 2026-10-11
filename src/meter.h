/*
Mixer Bridge
Copyright (C) 2026 HolyGrail

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* EBU R 128 のラウドネス計。音声スレッドから追加し、通信スレッドから読む */
struct meter;

struct meter_result {
	/* 無音や未計測は -INFINITY */
	double momentary;
	double short_term;
	double integrated;
	double true_peak;
};

struct meter *meter_create(uint32_t channels, uint32_t sample_rate);
void meter_destroy(struct meter *m);

/* planar float の音声を gain 倍して足す */
void meter_add_planar(struct meter *m, const float *const *planes, uint32_t frames, float gain);
void meter_read(struct meter *m, struct meter_result *out);
void meter_reset(struct meter *m);
