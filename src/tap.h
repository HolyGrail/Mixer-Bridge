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
#include <stddef.h>

#include "meter.h"

/* 1 クライアント分の計測。ソースとミックス（音声トラック）に計を取り付ける */
struct tap;

struct tap_source_result {
	const char *uuid;
	struct meter_result result;
};

struct tap *tap_create(void);
/* 取り付けに失敗したソース（見つからない、音声がない）は黙って飛ばす */
void tap_add_source(struct tap *t, const char *uuid);
/* mix は 1〜6 */
void tap_add_mix(struct tap *t, int mix);
void tap_reset(struct tap *t);
double tap_elapsed(struct tap *t);

size_t tap_source_count(struct tap *t);
void tap_read_source(struct tap *t, size_t i, struct tap_source_result *out);
size_t tap_mix_count(struct tap *t);
void tap_read_mix(struct tap *t, size_t i, int *mix, struct meter_result *out);

void tap_destroy(struct tap *t);
