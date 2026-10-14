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

#include "meter.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <ebur128.h>
#include <util/bmem.h>
#include <util/threading.h>

#define METER_MODE (EBUR128_MODE_M | EBUR128_MODE_S | EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK | EBUR128_MODE_HISTOGRAM)

/* クリップとみなす絶対値。整数 PCM の正の最大値（16 bit なら 32767/32768）は float に直すと 1.0 に届かないので少し下に置く */
#define CLIP_THRESHOLD 0.999f

/* 無音のときフィルタの内部状態が非正規化数まで減衰して遅くなるのを防ぐ微小な直流。バンドパスなので出力には出ない */
#define ANTI_DENORMAL 1e-18

#define METER_PI 3.14159265358979323846

const double meter_band_edges_hz[METER_BANDS + 1] = {20.0, 150.0, 500.0, 2000.0, 5000.0, 10000.0};

/* 2 次のバンドパス（双 2 次フィルタ、transposed direct form II）。b1 は 0 なので持たない */
struct biquad {
	double b0, b2, a1, a2;
	double z1, z2;
};

/* 品質統計の集計領域。区間の値は meter_read で 0 に戻す */
struct quality_stats {
	struct biquad band[METER_BANDS];
	double band_energy[METER_BANDS]; /* 帯域ごとの出力の二乗和 */
	uint64_t band_frames;
	float peak;       /* 区間のサンプルピーク（絶対値） */
	uint64_t clipped; /* 区間で CLIP_THRESHOLD 以上だったサンプル数 */
	uint64_t samples; /* 区間の全サンプル数（全チャンネル分） */
};

struct meter {
	pthread_mutex_t mutex;
	ebur128_state *state;
	int mode;
	uint32_t channels;
	uint32_t sample_rate;
	/* ebur128 は interleaved しか受けないため、並べ替え用の作業領域 */
	float *buffer;
	/* 帯域別レベル用に全チャンネルを平均したモノラル。quality が NULL なら使わない */
	float *mono;
	size_t buffer_frames;
	struct quality_stats *quality; /* NULL なら集めない */
};

/* アナログの 2 次バンドパス H(s) = BW·s / (s² + BW·s + ω0²) を双一次変換する。
   端の周波数を先に歪ませておく（プリワープ）ので、-3 dB 点が lo_hz と hi_hz に正確に来る */
static void biquad_bandpass(struct biquad *q, double lo_hz, double hi_hz, double sample_rate)
{
	double w1 = tan(METER_PI * lo_hz / sample_rate);
	double w2 = tan(METER_PI * hi_hz / sample_rate);
	double bw = w2 - w1;
	double w0sq = w1 * w2;
	double a0 = 1.0 + bw + w0sq;
	q->b0 = bw / a0;
	q->b2 = -bw / a0;
	q->a1 = 2.0 * (w0sq - 1.0) / a0;
	q->a2 = (1.0 - bw + w0sq) / a0;
	q->z1 = 0.0;
	q->z2 = 0.0;
}

static inline double biquad_run(struct biquad *q, double x)
{
	double y = q->b0 * x + q->z1;
	q->z1 = q->z2 - q->a1 * y;
	q->z2 = q->b2 * x - q->a2 * y;
	return y;
}

static void quality_clear_interval(struct quality_stats *q)
{
	memset(q->band_energy, 0, sizeof(q->band_energy));
	q->band_frames = 0;
	q->peak = 0.0f;
	q->clipped = 0;
	q->samples = 0;
}

struct meter *meter_create(uint32_t channels, uint32_t sample_rate, bool quality)
{
	struct meter *m = bzalloc(sizeof(*m));
	m->channels = channels;
	m->sample_rate = sample_rate;
	m->mode = METER_MODE | (quality ? EBUR128_MODE_LRA : 0);
	m->state = ebur128_init(channels, sample_rate, m->mode);
	if (!m->state || pthread_mutex_init(&m->mutex, NULL) != 0) {
		if (m->state)
			ebur128_destroy(&m->state);
		bfree(m);
		return NULL;
	}
	if (quality) {
		m->quality = bzalloc(sizeof(*m->quality));
		for (int b = 0; b < METER_BANDS; b++)
			biquad_bandpass(&m->quality->band[b], meter_band_edges_hz[b], meter_band_edges_hz[b + 1],
					sample_rate);
	}
	return m;
}

void meter_destroy(struct meter *m)
{
	if (!m)
		return;
	ebur128_destroy(&m->state);
	pthread_mutex_destroy(&m->mutex);
	bfree(m->quality);
	bfree(m->mono);
	bfree(m->buffer);
	bfree(m);
}

/* gain を掛ける前の 1 チャンネル分から、サンプルピークとクリップ数を数え、モノラルに足し込む */
static void quality_count(struct quality_stats *q, float *mono, const float *src, uint32_t frames)
{
	float peak = q->peak;
	uint64_t clipped = 0;
	for (uint32_t i = 0; i < frames; i++) {
		float x = src[i];
		float a = fabsf(x);
		if (a > peak)
			peak = a;
		if (a >= CLIP_THRESHOLD)
			clipped++;
		mono[i] += x;
	}
	q->peak = peak;
	q->clipped += clipped;
}

static void quality_run_bands(struct quality_stats *q, const float *mono, uint32_t frames, uint32_t channels)
{
	double scale = 1.0 / (double)channels;
	for (uint32_t i = 0; i < frames; i++) {
		double x = (double)mono[i] * scale + ANTI_DENORMAL;
		for (int b = 0; b < METER_BANDS; b++) {
			double y = biquad_run(&q->band[b], x);
			q->band_energy[b] += y * y;
		}
	}
	q->band_frames += frames;
}

void meter_add_planar(struct meter *m, const float *const *planes, uint32_t frames, float gain)
{
	pthread_mutex_lock(&m->mutex);
	if (m->buffer_frames < frames) {
		m->buffer = brealloc(m->buffer, sizeof(float) * frames * m->channels);
		if (m->quality)
			m->mono = brealloc(m->mono, sizeof(float) * frames);
		m->buffer_frames = frames;
	}
	struct quality_stats *q = m->quality;
	if (q)
		memset(m->mono, 0, sizeof(float) * frames);
	for (uint32_t ch = 0; ch < m->channels; ch++) {
		const float *src = planes[ch];
		if (q && src)
			quality_count(q, m->mono, src, frames);
		for (uint32_t i = 0; i < frames; i++)
			m->buffer[i * m->channels + ch] = src ? src[i] * gain : 0.0f;
	}
	if (q) {
		q->samples += (uint64_t)frames * m->channels;
		quality_run_bands(q, m->mono, frames, m->channels);
	}
	ebur128_add_frames_float(m->state, m->buffer, frames);
	pthread_mutex_unlock(&m->mutex);
}

static double to_db(double linear)
{
	return linear > 0.0 ? 20.0 * log10(linear) : -INFINITY;
}

void meter_read(struct meter *m, struct meter_result *out)
{
	pthread_mutex_lock(&m->mutex);
	if (ebur128_loudness_momentary(m->state, &out->momentary) != EBUR128_SUCCESS)
		out->momentary = -INFINITY;
	if (ebur128_loudness_shortterm(m->state, &out->short_term) != EBUR128_SUCCESS)
		out->short_term = -INFINITY;
	if (ebur128_loudness_global(m->state, &out->integrated) != EBUR128_SUCCESS)
		out->integrated = -INFINITY;
	double peak = 0.0;
	for (uint32_t ch = 0; ch < m->channels; ch++) {
		double p;
		if (ebur128_true_peak(m->state, ch, &p) == EBUR128_SUCCESS && p > peak)
			peak = p;
	}
	out->true_peak = to_db(peak);

	struct quality_stats *q = m->quality;
	out->has_quality = q != NULL;
	if (q) {
		/* ebur128 はゲートに掛かるブロックがないと LRA を 0 で返すので、integrated が無音なら無音にそろえる */
		double lra;
		if (!isfinite(out->integrated) || ebur128_loudness_range(m->state, &lra) != EBUR128_SUCCESS)
			lra = -INFINITY;
		out->quality.lra = lra;
		out->quality.sample_peak = to_db(q->peak);
		out->quality.clip_ratio = q->samples ? (double)q->clipped / (double)q->samples : 0.0;
		for (int b = 0; b < METER_BANDS; b++) {
			out->quality.bands[b] = q->band_frames ? to_db(sqrt(q->band_energy[b] / (double)q->band_frames))
							       : -INFINITY;
		}
		quality_clear_interval(q);
	}
	pthread_mutex_unlock(&m->mutex);
}

void meter_reset(struct meter *m)
{
	pthread_mutex_lock(&m->mutex);
	ebur128_state *fresh = ebur128_init(m->channels, m->sample_rate, m->mode);
	if (fresh) {
		ebur128_destroy(&m->state);
		m->state = fresh;
	}
	if (m->quality)
		quality_clear_interval(m->quality);
	pthread_mutex_unlock(&m->mutex);
}
