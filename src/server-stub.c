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

/* Windows 以外ではまだ通信できない（名前付きパイプの代わりに Unix ドメインソケットを使う予定） */

#include "server.h"

#include <obs-module.h>
#include <plugin-support.h>

bool server_start(void)
{
	obs_log(LOG_WARNING, "this platform is not supported yet; the plugin does nothing");
	return true;
}

void server_stop(void) {}
