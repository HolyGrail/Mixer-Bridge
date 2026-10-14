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

#include "tap.h"

#include <obs.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/platform.h>

struct source_tap {
	obs_weak_source_t *weak;
	char *uuid;
	struct meter *meter;
};

struct mix_tap {
	int mix;
	struct meter *meter;
};

struct tap {
	DARRAY(struct source_tap *) sources;
	DARRAY(struct mix_tap *) mixes;
	uint64_t started_ns;
	bool quality;
};

static struct meter *new_meter(bool quality)
{
	struct obs_audio_info oai;
	if (!obs_get_audio_info(&oai))
		return NULL;
	return meter_create((uint32_t)get_audio_channels(oai.speakers), oai.samples_per_sec, quality);
}

/* ソースの音声はフィルタを通った後、フェーダーの前に届くので、ここでフェーダーとミュートを掛ける */
static void source_audio(void *param, obs_source_t *source, const struct audio_data *data, bool muted)
{
	struct source_tap *st = param;
	float gain = muted ? 0.0f : obs_source_get_volume(source);
	meter_add_planar(st->meter, (const float *const *)data->data, data->frames, gain);
}

static void mix_audio(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	struct mix_tap *mt = param;
	meter_add_planar(mt->meter, (const float *const *)data->data, data->frames, 1.0f);
}

struct tap *tap_create(bool quality)
{
	struct tap *t = bzalloc(sizeof(*t));
	t->started_ns = os_gettime_ns();
	t->quality = quality;
	return t;
}

void tap_add_source(struct tap *t, const char *uuid)
{
	obs_source_t *source = obs_get_source_by_uuid(uuid);
	if (!source)
		return;
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0) {
		obs_source_release(source);
		return;
	}
	struct meter *meter = new_meter(t->quality);
	if (!meter) {
		obs_source_release(source);
		return;
	}
	struct source_tap *st = bzalloc(sizeof(*st));
	st->weak = obs_source_get_weak_source(source);
	st->uuid = bstrdup(uuid);
	st->meter = meter;
	da_push_back(t->sources, &st);
	obs_source_add_audio_capture_callback(source, source_audio, st);
	obs_source_release(source);
}

void tap_add_mix(struct tap *t, int mix)
{
	if (mix < 1 || mix > MAX_AUDIO_MIXES)
		return;
	struct meter *meter = new_meter(false);
	if (!meter)
		return;
	struct mix_tap *mt = bzalloc(sizeof(*mt));
	mt->mix = mix;
	mt->meter = meter;
	da_push_back(t->mixes, &mt);
	obs_add_raw_audio_callback((size_t)(mix - 1), NULL, mix_audio, mt);
}

void tap_reset(struct tap *t)
{
	for (size_t i = 0; i < t->sources.num; i++)
		meter_reset(t->sources.array[i]->meter);
	for (size_t i = 0; i < t->mixes.num; i++)
		meter_reset(t->mixes.array[i]->meter);
	t->started_ns = os_gettime_ns();
}

double tap_elapsed(struct tap *t)
{
	return (double)(os_gettime_ns() - t->started_ns) / 1e9;
}

size_t tap_source_count(struct tap *t)
{
	return t->sources.num;
}

void tap_read_source(struct tap *t, size_t i, struct tap_source_result *out)
{
	struct source_tap *st = t->sources.array[i];
	out->uuid = st->uuid;
	meter_read(st->meter, &out->result);
}

size_t tap_mix_count(struct tap *t)
{
	return t->mixes.num;
}

void tap_read_mix(struct tap *t, size_t i, int *mix, struct meter_result *out)
{
	struct mix_tap *mt = t->mixes.array[i];
	*mix = mt->mix;
	meter_read(mt->meter, out);
}

void tap_destroy(struct tap *t)
{
	if (!t)
		return;
	/* コールバックを外し終えてから計を壊す（外した後は呼ばれない） */
	for (size_t i = 0; i < t->sources.num; i++) {
		struct source_tap *st = t->sources.array[i];
		obs_source_t *source = obs_weak_source_get_source(st->weak);
		if (source) {
			obs_source_remove_audio_capture_callback(source, source_audio, st);
			obs_source_release(source);
		}
		obs_weak_source_release(st->weak);
		meter_destroy(st->meter);
		bfree(st->uuid);
		bfree(st);
	}
	for (size_t i = 0; i < t->mixes.num; i++) {
		struct mix_tap *mt = t->mixes.array[i];
		obs_remove_raw_audio_callback((size_t)(mt->mix - 1), mix_audio, mt);
		meter_destroy(mt->meter);
		bfree(mt);
	}
	da_free(t->sources);
	da_free(t->mixes);
	bfree(t);
}
