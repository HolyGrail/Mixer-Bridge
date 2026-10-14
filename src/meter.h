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

/* 帯域別レベルの帯域数。境界は meter_band_edges_hz */
#define METER_BANDS 5
extern const double meter_band_edges_hz[METER_BANDS + 1];

/* 品質統計。gain を掛ける前（フィルタ後・フェーダー前）の音で測る。lra だけは ebur128 の計と同じ音（gain 後） */
struct meter_quality {
	/* 購読開始から積算したラウドネスレンジ（LU）。無音は -INFINITY */
	double lra;
	/* 以下は直前の meter_read からの区間の値。無音・未計測は -INFINITY */
	double sample_peak;        /* dBFS */
	double clip_ratio;         /* 0〜1。区間にサンプルがなければ 0 */
	double bands[METER_BANDS]; /* 帯域ごとの RMS（dBFS） */
};

struct meter_result {
	/* 無音や未計測は -INFINITY */
	double momentary;
	double short_term;
	double integrated;
	double true_peak;
	/* meter_create で quality を頼んだときだけ有効 */
	bool has_quality;
	struct meter_quality quality;
};

/* quality を true にすると品質統計も集める（音声スレッドの負荷が増える） */
struct meter *meter_create(uint32_t channels, uint32_t sample_rate, bool quality);
void meter_destroy(struct meter *m);

/* planar float の音声を gain 倍して足す。品質統計は gain を掛ける前の値で数える */
void meter_add_planar(struct meter *m, const float *const *planes, uint32_t frames, float gain);
/* 読むと、区間ごとの品質統計（sample_peak、clip_ratio、bands）は 0 に戻る */
void meter_read(struct meter *m, struct meter_result *out);
void meter_reset(struct meter *m);
