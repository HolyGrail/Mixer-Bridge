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

#include "protocol.h"

#include <math.h>
#include <string.h>

#include <cJSON.h>
#include <obs.h>
#include <media-io/audio-math.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <plugin-support.h>

#include "tap.h"

struct session {
	bool greeted;
	struct tap *tap;
	int interval_ms;
	uint64_t next_meters_ns;
};

struct session *session_create(void)
{
	return bzalloc(sizeof(struct session));
}

void session_destroy(struct session *s)
{
	if (!s)
		return;
	tap_destroy(s->tap);
	bfree(s);
}

/* ---- JSON の小道具 ---- */

static char *print(cJSON *json)
{
	char *text = cJSON_PrintUnformatted(json);
	char *out = bstrdup(text);
	cJSON_free(text);
	cJSON_Delete(json);
	return out;
}

/* 無音（-INFINITY）は null にする */
static void add_db(cJSON *obj, const char *key, double value)
{
	if (isfinite(value))
		cJSON_AddNumberToObject(obj, key, round(value * 100.0) / 100.0);
	else
		cJSON_AddNullToObject(obj, key);
}

static char *reply(const cJSON *id, cJSON *result)
{
	cJSON *res = cJSON_CreateObject();
	cJSON_AddItemToObject(res, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
	cJSON_AddItemToObject(res, "result", result ? result : cJSON_CreateObject());
	return print(res);
}

static char *fail(const cJSON *id, const char *code, const char *message)
{
	cJSON *res = cJSON_CreateObject();
	cJSON_AddItemToObject(res, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
	cJSON *err = cJSON_AddObjectToObject(res, "error");
	cJSON_AddStringToObject(err, "code", code);
	cJSON_AddStringToObject(err, "message", message);
	return print(res);
}

static const char *param_string(const cJSON *params, const char *key)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(params, key);
	return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* obs_data を cJSON に変換する（フィルタ設定の受け渡し用） */
static cJSON *settings_to_json(obs_data_t *settings)
{
	const char *text = settings ? obs_data_get_json(settings) : NULL;
	cJSON *json = text ? cJSON_Parse(text) : NULL;
	return json ? json : cJSON_CreateObject();
}

static double volume_db(obs_source_t *source)
{
	return mul_to_db(obs_source_get_volume(source));
}

/* ---- メソッド ---- */

static char *do_hello(struct session *s, const cJSON *id, const cJSON *params)
{
	const cJSON *protocol = cJSON_GetObjectItemCaseSensitive(params, "protocol");
	if (!cJSON_IsNumber(protocol) || protocol->valueint != MIXER_BRIDGE_PROTOCOL)
		return fail(id, "unsupported_protocol", "this plugin speaks protocol 1");
	const char *client = param_string(params, "client");
	obs_log(LOG_INFO, "client connected: %s", client ? client : "(unnamed)");
	s->greeted = true;

	cJSON *result = cJSON_CreateObject();
	cJSON_AddNumberToObject(result, "protocol", MIXER_BRIDGE_PROTOCOL);
	cJSON_AddStringToObject(result, "plugin", PLUGIN_VERSION);
	cJSON_AddStringToObject(result, "obs", obs_get_version_string());
	return reply(id, result);
}

static bool add_source(void *param, obs_source_t *source)
{
	cJSON *list = param;
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
		return true;
	cJSON *item = cJSON_CreateObject();
	cJSON_AddStringToObject(item, "uuid", obs_source_get_uuid(source));
	cJSON_AddStringToObject(item, "name", obs_source_get_name(source));
	cJSON_AddStringToObject(item, "kind", obs_source_get_id(source));
	add_db(item, "volume_db", volume_db(source));
	cJSON_AddBoolToObject(item, "muted", obs_source_muted(source));
	cJSON *mixes = cJSON_AddArrayToObject(item, "mixes");
	uint32_t mixers = obs_source_get_audio_mixers(source);
	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (mixers & (1u << i))
			cJSON_AddItemToArray(mixes, cJSON_CreateNumber(i + 1));
	}
	cJSON_AddItemToArray(list, item);
	return true;
}

static char *do_list_sources(const cJSON *id)
{
	cJSON *result = cJSON_CreateObject();
	obs_enum_sources(add_source, cJSON_AddArrayToObject(result, "sources"));
	return reply(id, result);
}

static obs_source_t *find_source(const cJSON *params)
{
	const char *uuid = param_string(params, "source");
	return uuid ? obs_get_source_by_uuid(uuid) : NULL;
}

static char *do_volume(const cJSON *id, const cJSON *params, bool set)
{
	obs_source_t *source = find_source(params);
	if (!source)
		return fail(id, "not_found", "source not found");
	if (set) {
		const cJSON *db = cJSON_GetObjectItemCaseSensitive(params, "volume_db");
		if (!cJSON_IsNumber(db)) {
			obs_source_release(source);
			return fail(id, "invalid_params", "volume_db must be a number");
		}
		double clamped = fmin(26.0, fmax(-100.0, db->valuedouble));
		obs_source_set_volume(source, db_to_mul((float)clamped));
	}
	cJSON *result = cJSON_CreateObject();
	add_db(result, "volume_db", volume_db(source));
	cJSON_AddBoolToObject(result, "muted", obs_source_muted(source));
	obs_source_release(source);
	return reply(id, result);
}

static cJSON *describe_filter(obs_source_t *filter)
{
	cJSON *item = cJSON_CreateObject();
	cJSON_AddStringToObject(item, "name", obs_source_get_name(filter));
	cJSON_AddStringToObject(item, "kind", obs_source_get_id(filter));
	cJSON_AddBoolToObject(item, "enabled", obs_source_enabled(filter));
	obs_data_t *settings = obs_source_get_settings(filter);
	cJSON_AddItemToObject(item, "settings", settings_to_json(settings));
	obs_data_release(settings);
	return item;
}

static void add_filter(obs_source_t *parent, obs_source_t *filter, void *param)
{
	UNUSED_PARAMETER(parent);
	cJSON_AddItemToArray(param, describe_filter(filter));
}

static char *do_list_filters(const cJSON *id, const cJSON *params)
{
	obs_source_t *source = find_source(params);
	if (!source)
		return fail(id, "not_found", "source not found");
	cJSON *result = cJSON_CreateObject();
	obs_source_enum_filters(source, add_filter, cJSON_AddArrayToObject(result, "filters"));
	obs_source_release(source);
	return reply(id, result);
}

static char *do_set_filter(const cJSON *id, const cJSON *params)
{
	obs_source_t *source = find_source(params);
	if (!source)
		return fail(id, "not_found", "source not found");
	const char *name = param_string(params, "filter");
	obs_source_t *filter = name ? obs_source_get_filter_by_name(source, name) : NULL;
	obs_source_release(source);
	if (!filter)
		return fail(id, "not_found", "filter not found");

	const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(params, "enabled");
	if (cJSON_IsBool(enabled))
		obs_source_set_enabled(filter, cJSON_IsTrue(enabled));
	const cJSON *settings = cJSON_GetObjectItemCaseSensitive(params, "settings");
	if (cJSON_IsObject(settings)) {
		char *text = cJSON_PrintUnformatted(settings);
		obs_data_t *data = obs_data_create_from_json(text);
		cJSON_free(text);
		if (data) {
			obs_source_update(filter, data);
			obs_data_release(data);
		}
	}
	char *out = reply(id, describe_filter(filter));
	obs_source_release(filter);
	return out;
}

static char *do_subscribe(struct session *s, const cJSON *id, const cJSON *params)
{
	const cJSON *interval = cJSON_GetObjectItemCaseSensitive(params, "interval_ms");
	int ms = cJSON_IsNumber(interval) ? interval->valueint : 100;
	if (ms < 100 || ms > 1000)
		return fail(id, "invalid_params", "interval_ms must be between 100 and 1000");

	bool quality = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "quality"));
	tap_destroy(s->tap);
	s->tap = tap_create(quality);
	const cJSON *item;
	cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(params, "sources"))
	{
		if (cJSON_IsString(item))
			tap_add_source(s->tap, item->valuestring);
	}
	const cJSON *mixes = cJSON_GetObjectItemCaseSensitive(params, "mixes");
	if (cJSON_IsArray(mixes)) {
		cJSON_ArrayForEach(item, mixes)
		{
			if (cJSON_IsNumber(item))
				tap_add_mix(s->tap, item->valueint);
		}
	} else {
		tap_add_mix(s->tap, 1);
	}
	s->interval_ms = ms;
	s->next_meters_ns = os_gettime_ns() + (uint64_t)ms * 1000000;
	if (!quality)
		return reply(id, NULL);
	/* 帯域の境界を返す。クライアントはこれで bands を解釈するので、境界を変えてもクライアントは壊れない */
	cJSON *result = cJSON_CreateObject();
	cJSON *q = cJSON_AddObjectToObject(result, "quality");
	cJSON_AddItemToObject(q, "band_edges_hz", cJSON_CreateDoubleArray(meter_band_edges_hz, METER_BANDS + 1));
	return reply(id, result);
}

static char *do_unsubscribe(struct session *s, const cJSON *id)
{
	tap_destroy(s->tap);
	s->tap = NULL;
	return reply(id, NULL);
}

static char *do_reset(struct session *s, const cJSON *id)
{
	if (s->tap)
		tap_reset(s->tap);
	return reply(id, NULL);
}

char *session_handle(struct session *s, const char *line)
{
	cJSON *req = cJSON_Parse(line);
	if (!cJSON_IsObject(req)) {
		cJSON_Delete(req);
		return fail(NULL, "invalid_request", "not a JSON object");
	}
	const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
	const char *method = param_string(req, "method");
	const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
	char *out;

	if (!method)
		out = fail(id, "invalid_request", "method is required");
	else if (strcmp(method, "hello") == 0)
		out = do_hello(s, id, params);
	else if (!s->greeted)
		out = fail(id, "hello_required", "send hello first");
	else if (strcmp(method, "list_sources") == 0)
		out = do_list_sources(id);
	else if (strcmp(method, "get_volume") == 0)
		out = do_volume(id, params, false);
	else if (strcmp(method, "set_volume") == 0)
		out = do_volume(id, params, true);
	else if (strcmp(method, "list_filters") == 0)
		out = do_list_filters(id, params);
	else if (strcmp(method, "set_filter") == 0)
		out = do_set_filter(id, params);
	else if (strcmp(method, "subscribe_meters") == 0)
		out = do_subscribe(s, id, params);
	else if (strcmp(method, "unsubscribe_meters") == 0)
		out = do_unsubscribe(s, id);
	else if (strcmp(method, "reset_meters") == 0)
		out = do_reset(s, id);
	else
		out = fail(id, "unknown_method", method);

	cJSON_Delete(req);
	return out;
}

int session_meters_due_ms(struct session *s)
{
	if (!s->tap)
		return -1;
	uint64_t now = os_gettime_ns();
	return now >= s->next_meters_ns ? 0 : (int)((s->next_meters_ns - now) / 1000000);
}

static void add_result(cJSON *obj, const struct meter_result *r)
{
	add_db(obj, "momentary", r->momentary);
	add_db(obj, "short_term", r->short_term);
	add_db(obj, "integrated", r->integrated);
	add_db(obj, "true_peak", r->true_peak);
	if (!r->has_quality)
		return;
	const struct meter_quality *q = &r->quality;
	cJSON *quality = cJSON_AddObjectToObject(obj, "quality");
	add_db(quality, "lra", q->lra);
	add_db(quality, "sample_peak", q->sample_peak);
	/* 1 秒間隔でも 1 サンプルのクリップが 0 に丸まらない桁数にする */
	cJSON_AddNumberToObject(quality, "clip_ratio", round(q->clip_ratio * 1e6) / 1e6);
	/* meter.c は無音の区間で全帯域を -INFINITY にする。そのときは要素ごとでなく配列ごと null にする */
	bool any_finite = false;
	for (int b = 0; b < METER_BANDS; b++)
		any_finite = any_finite || isfinite(q->bands[b]);
	if (!any_finite) {
		cJSON_AddNullToObject(quality, "bands");
		return;
	}
	cJSON *bands = cJSON_AddArrayToObject(quality, "bands");
	for (int b = 0; b < METER_BANDS; b++) {
		cJSON_AddItemToArray(bands, isfinite(q->bands[b])
						    ? cJSON_CreateNumber(round(q->bands[b] * 100.0) / 100.0)
						    : cJSON_CreateNull());
	}
}

char *session_meters_event(struct session *s)
{
	uint64_t step = (uint64_t)s->interval_ms * 1000000;
	uint64_t now = os_gettime_ns();
	s->next_meters_ns += step;
	if (s->next_meters_ns < now) /* 遅れたら追いつこうとせず、今から数え直す */
		s->next_meters_ns = now + step;

	cJSON *event = cJSON_CreateObject();
	cJSON_AddStringToObject(event, "event", "meters");
	cJSON *data = cJSON_AddObjectToObject(event, "data");
	cJSON_AddNumberToObject(data, "time", round(tap_elapsed(s->tap) * 1000.0) / 1000.0);

	cJSON *mixes = cJSON_AddArrayToObject(data, "mixes");
	for (size_t i = 0; i < tap_mix_count(s->tap); i++) {
		int mix;
		struct meter_result r;
		tap_read_mix(s->tap, i, &mix, &r);
		cJSON *item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "mix", mix);
		add_result(item, &r);
		cJSON_AddItemToArray(mixes, item);
	}
	cJSON *sources = cJSON_AddArrayToObject(data, "sources");
	for (size_t i = 0; i < tap_source_count(s->tap); i++) {
		struct tap_source_result r;
		tap_read_source(s->tap, i, &r);
		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "uuid", r.uuid);
		add_result(item, &r.result);
		cJSON_AddItemToArray(sources, item);
	}
	return print(event);
}
