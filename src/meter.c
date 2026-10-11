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

#include <ebur128.h>
#include <util/bmem.h>
#include <util/threading.h>

#define METER_MODE (EBUR128_MODE_M | EBUR128_MODE_S | EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK | EBUR128_MODE_HISTOGRAM)

struct meter {
	pthread_mutex_t mutex;
	ebur128_state *state;
	uint32_t channels;
	uint32_t sample_rate;
	/* ebur128 は interleaved しか受けないため、並べ替え用の作業領域 */
	float *buffer;
	size_t buffer_frames;
};

struct meter *meter_create(uint32_t channels, uint32_t sample_rate)
{
	struct meter *m = bzalloc(sizeof(*m));
	m->channels = channels;
	m->sample_rate = sample_rate;
	m->state = ebur128_init(channels, sample_rate, METER_MODE);
	if (!m->state || pthread_mutex_init(&m->mutex, NULL) != 0) {
		if (m->state)
			ebur128_destroy(&m->state);
		bfree(m);
		return NULL;
	}
	return m;
}

void meter_destroy(struct meter *m)
{
	if (!m)
		return;
	ebur128_destroy(&m->state);
	pthread_mutex_destroy(&m->mutex);
	bfree(m->buffer);
	bfree(m);
}

void meter_add_planar(struct meter *m, const float *const *planes, uint32_t frames, float gain)
{
	pthread_mutex_lock(&m->mutex);
	if (m->buffer_frames < frames) {
		m->buffer = brealloc(m->buffer, sizeof(float) * frames * m->channels);
		m->buffer_frames = frames;
	}
	for (uint32_t ch = 0; ch < m->channels; ch++) {
		const float *src = planes[ch];
		for (uint32_t i = 0; i < frames; i++)
			m->buffer[i * m->channels + ch] = src ? src[i] * gain : 0.0f;
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
	pthread_mutex_unlock(&m->mutex);
	out->true_peak = to_db(peak);
}

void meter_reset(struct meter *m)
{
	pthread_mutex_lock(&m->mutex);
	ebur128_state *fresh = ebur128_init(m->channels, m->sample_rate, METER_MODE);
	if (fresh) {
		ebur128_destroy(&m->state);
		m->state = fresh;
	}
	pthread_mutex_unlock(&m->mutex);
}
