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

/* 名前付きパイプ \\.\pipe\mixer-bridge のサーバー。1 クライアントに 1 スレッドを使う */

#include "server.h"

#include <windows.h>
#include <sddl.h>

#include <obs-module.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/threading.h>

#include <plugin-support.h>

#include "protocol.h"

#define PIPE_NAME L"\\\\.\\pipe\\mixer-bridge"
#define BUFFER_SIZE 65536
/* 1 行の上限。これを超える要求は切断する */
#define MAX_LINE (1024 * 1024)

struct client {
	HANDLE pipe;
	pthread_t thread;
	volatile bool done;
};

static HANDLE stop_event;
static pthread_t listen_thread;
static bool running;
static pthread_mutex_t clients_mutex;
static DARRAY(struct client *) clients;

/* 今のユーザーと SYSTEM だけが接続できるようにする */
static bool make_security(SECURITY_ATTRIBUTES *sa)
{
	HANDLE token;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return false;
	BYTE buf[256];
	DWORD len;
	bool ok = false;
	if (GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len)) {
		LPWSTR sid;
		if (ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &sid)) {
			wchar_t sddl[256];
			swprintf(sddl, 256, L"D:P(A;;GA;;;%s)(A;;GA;;;SY)", sid);
			LocalFree(sid);
			sa->nLength = sizeof(*sa);
			sa->bInheritHandle = FALSE;
			ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1,
										  &sa->lpSecurityDescriptor, NULL);
		}
	}
	CloseHandle(token);
	return ok;
}

static bool write_line(HANDLE pipe, HANDLE io_event, const char *json)
{
	struct dstr line = {0};
	dstr_copy(&line, json);
	dstr_cat_ch(&line, '\n');
	OVERLAPPED ov = {.hEvent = io_event};
	DWORD written = 0;
	bool ok = WriteFile(pipe, line.array, (DWORD)line.len, NULL, &ov) || GetLastError() == ERROR_IO_PENDING;
	ok = ok && GetOverlappedResult(pipe, &ov, &written, TRUE) && written == line.len;
	dstr_free(&line);
	return ok;
}

static void *client_main(void *param)
{
	struct client *c = param;
	os_set_thread_name("mixer-bridge: client");
	struct session *s = session_create();
	HANDLE read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	HANDLE write_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	struct dstr pending = {0};
	char buf[BUFFER_SIZE];
	OVERLAPPED ov = {.hEvent = read_event};
	bool reading = false;

	for (;;) {
		if (!reading) {
			ResetEvent(read_event);
			if (!ReadFile(c->pipe, buf, sizeof(buf), NULL, &ov) && GetLastError() != ERROR_IO_PENDING)
				break;
			reading = true;
		}

		int due = session_meters_due_ms(s);
		HANDLE waits[2] = {read_event, stop_event};
		DWORD r = WaitForMultipleObjects(2, waits, FALSE, due < 0 ? INFINITE : (DWORD)due);

		if (r == WAIT_OBJECT_0 + 1)
			break;
		if (r == WAIT_TIMEOUT) {
			char *event = session_meters_event(s);
			bool ok = write_line(c->pipe, write_event, event);
			bfree(event);
			if (!ok)
				break;
			continue;
		}

		DWORD got = 0;
		reading = false;
		if (!GetOverlappedResult(c->pipe, &ov, &got, FALSE))
			break; /* 切断 */
		dstr_ncat(&pending, buf, got);

		char *nl;
		bool ok = true;
		while (ok && pending.array && (nl = strchr(pending.array, '\n')) != NULL) {
			*nl = '\0';
			if (nl > pending.array && nl[-1] == '\r')
				nl[-1] = '\0';
			if (pending.array[0] != '\0') {
				char *res = session_handle(s, pending.array);
				ok = write_line(c->pipe, write_event, res);
				bfree(res);
			}
			dstr_remove(&pending, 0, (size_t)(nl - pending.array) + 1);
		}
		if (!ok || pending.len > MAX_LINE)
			break;
	}

	if (reading)
		CancelIoEx(c->pipe, &ov);
	dstr_free(&pending);
	session_destroy(s);
	CloseHandle(read_event);
	CloseHandle(write_event);
	DisconnectNamedPipe(c->pipe);
	CloseHandle(c->pipe);
	c->pipe = INVALID_HANDLE_VALUE;
	c->done = true;
	return NULL;
}

/* 終わったクライアントのスレッドを回収する */
static void reap_clients(bool all)
{
	pthread_mutex_lock(&clients_mutex);
	for (size_t i = clients.num; i > 0; i--) {
		struct client *c = clients.array[i - 1];
		if (all || c->done) {
			pthread_join(c->thread, NULL);
			bfree(c);
			da_erase(clients, i - 1);
		}
	}
	pthread_mutex_unlock(&clients_mutex);
}

static void *listen_main(void *param)
{
	UNUSED_PARAMETER(param);
	os_set_thread_name("mixer-bridge: listen");
	SECURITY_ATTRIBUTES sa = {0};
	if (!make_security(&sa)) {
		obs_log(LOG_ERROR, "failed to build pipe security descriptor (%lu)", GetLastError());
		return NULL;
	}
	HANDLE connect_event = CreateEventW(NULL, TRUE, FALSE, NULL);

	for (;;) {
		HANDLE pipe = CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
					       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
						       PIPE_REJECT_REMOTE_CLIENTS,
					       PIPE_UNLIMITED_INSTANCES, BUFFER_SIZE, BUFFER_SIZE, 0, &sa);
		if (pipe == INVALID_HANDLE_VALUE) {
			obs_log(LOG_ERROR, "CreateNamedPipe failed (%lu)", GetLastError());
			break;
		}

		ResetEvent(connect_event);
		OVERLAPPED ov = {.hEvent = connect_event};
		bool connected = ConnectNamedPipe(pipe, &ov) || GetLastError() == ERROR_PIPE_CONNECTED;
		if (!connected && GetLastError() == ERROR_IO_PENDING) {
			HANDLE waits[2] = {connect_event, stop_event};
			DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
			DWORD unused;
			if (r == WAIT_OBJECT_0 + 1) {
				CancelIoEx(pipe, &ov);
				CloseHandle(pipe);
				break;
			}
			connected = GetOverlappedResult(pipe, &ov, &unused, FALSE);
		}
		if (!connected) {
			CloseHandle(pipe);
			continue;
		}

		reap_clients(false);
		struct client *c = bzalloc(sizeof(*c));
		c->pipe = pipe;
		if (pthread_create(&c->thread, NULL, client_main, c) != 0) {
			CloseHandle(pipe);
			bfree(c);
			continue;
		}
		pthread_mutex_lock(&clients_mutex);
		da_push_back(clients, &c);
		pthread_mutex_unlock(&clients_mutex);
	}

	CloseHandle(connect_event);
	LocalFree(sa.lpSecurityDescriptor);
	return NULL;
}

bool server_start(void)
{
	stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!stop_event || pthread_mutex_init(&clients_mutex, NULL) != 0)
		return false;
	da_init(clients);
	if (pthread_create(&listen_thread, NULL, listen_main, NULL) != 0) {
		CloseHandle(stop_event);
		pthread_mutex_destroy(&clients_mutex);
		return false;
	}
	running = true;
	obs_log(LOG_INFO, "listening on \\\\.\\pipe\\mixer-bridge");
	return true;
}

void server_stop(void)
{
	if (!running)
		return;
	SetEvent(stop_event);
	pthread_join(listen_thread, NULL);
	reap_clients(true);
	da_free(clients);
	pthread_mutex_destroy(&clients_mutex);
	CloseHandle(stop_event);
	running = false;
}
