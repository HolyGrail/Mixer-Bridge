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

#define MIXER_BRIDGE_PROTOCOL 1

/* 1 クライアントの状態。そのクライアントの通信スレッドだけが触る */
struct session;

struct session *session_create(void);
void session_destroy(struct session *s);

/* 1 行の要求を処理し、応答の JSON（改行なし）を返す。呼び出し側が bfree する */
char *session_handle(struct session *s, const char *line);

/* 次に meters イベントを送るまでのミリ秒。購読していなければ -1 */
int session_meters_due_ms(struct session *s);
/* meters イベントの JSON（改行なし）を返す。呼び出し側が bfree する */
char *session_meters_event(struct session *s);
