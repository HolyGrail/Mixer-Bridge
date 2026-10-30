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

/* Unix ドメインソケット $TMPDIR/mixer-bridge.sock のサーバー。1 クライアントに 1 スレッドを使う */

#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <obs-module.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/threading.h>

#include <plugin-support.h>

#include "protocol.h"

#define SOCKET_NAME "mixer-bridge.sock"
#define LOCK_NAME "mixer-bridge.lock"
#define BUFFER_SIZE 65536
/* 1 行の上限。これを超える要求は切断する */
#define MAX_LINE (1024 * 1024)
/* 読まないクライアントに書き込みで止められないよう、送信はこの秒数で諦める */
#define SEND_TIMEOUT_SEC 5

struct client {
	/* 閉じるのはクライアントのスレッド。停止時の shutdown と競合しないよう clients_mutex の下で触る */
	int fd;
	pthread_t thread;
	volatile bool done;
};

static struct sockaddr_un addr;
static struct dstr lock_path;
/* ソケットのパスの持ち主であることを示すロック。OBS が落ちてもカーネルが外す */
static int lock_fd = -1;
static int listen_fd = -1;
/* 停止の合図。書き込み側を閉じると、読み込み側を poll している全スレッドが起きる */
static int stop_pipe[2] = {-1, -1};
static pthread_t listen_thread;
static bool running;
static pthread_mutex_t clients_mutex;
static DARRAY(struct client *) clients;

static void set_cloexec(int fd)
{
	fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

/* macOS の $TMPDIR はユーザーごとの 0700 のディレクトリなので、他のユーザーからは届かない */
static bool make_address(void)
{
	const char *dir = getenv("TMPDIR");
	if (!dir || !*dir)
		dir = "/tmp";
	struct dstr path = {0};
	dstr_copy(&path, dir);
	if (dstr_end(&path) != '/')
		dstr_cat_ch(&path, '/');
	dstr_copy_dstr(&lock_path, &path);
	dstr_cat(&lock_path, LOCK_NAME);
	dstr_cat(&path, SOCKET_NAME);

	bool ok = path.len < sizeof(addr.sun_path);
	if (ok) {
		addr.sun_family = AF_UNIX;
		strcpy(addr.sun_path, path.array);
	} else {
		obs_log(LOG_ERROR, "socket path is too long: %s", path.array);
		dstr_free(&lock_path);
	}
	dstr_free(&path);
	return ok;
}

static void unlock_address(void)
{
	if (lock_fd >= 0)
		close(lock_fd);
	lock_fd = -1;
}

/* 同時に起動した OBS どうしが互いのソケットを消さないよう、ロックを取った側だけがパスを扱う */
static bool lock_address(void)
{
	lock_fd = open(lock_path.array, O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
	if (lock_fd < 0)
		obs_log(LOG_ERROR, "failed to open %s (%s)", lock_path.array, strerror(errno));
	else if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0)
		obs_log(LOG_ERROR, "%s is already in use by another process", addr.sun_path);
	else {
		dstr_free(&lock_path);
		return true;
	}
	unlock_address();
	dstr_free(&lock_path);
	return false;
}

static bool open_listener(void)
{
	/* ロックを持っているので、残っているファイルは前に落ちた OBS のもの */
	unlink(addr.sun_path);

	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		obs_log(LOG_ERROR, "socket failed (%s)", strerror(errno));
		return false;
	}
	set_cloexec(listen_fd);

	/* 作った瞬間から自分以外が開けないよう、bind の間だけ umask を絞る */
	mode_t old_mask = umask(077);
	bool bound = bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
	umask(old_mask);
	bool ok = bound && chmod(addr.sun_path, S_IRUSR | S_IWUSR) == 0 && listen(listen_fd, SOMAXCONN) == 0;
	if (!ok) {
		obs_log(LOG_ERROR, "failed to listen on %s (%s)", addr.sun_path, strerror(errno));
		close(listen_fd);
		listen_fd = -1;
		if (bound)
			unlink(addr.sun_path);
	}
	return ok;
}

static bool write_line(int fd, const char *json)
{
	struct dstr line = {0};
	dstr_copy(&line, json);
	dstr_cat_ch(&line, '\n');
	size_t sent = 0;
	while (sent < line.len) {
		ssize_t n = write(fd, line.array + sent, line.len - sent);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		sent += (size_t)n;
	}
	bool ok = sent == line.len;
	dstr_free(&line);
	return ok;
}

static void *client_main(void *param)
{
	struct client *c = param;
	os_set_thread_name("mixer-bridge: client");
	struct session *s = session_create();
	struct dstr pending = {0};
	char buf[BUFFER_SIZE];

	for (;;) {
		struct pollfd fds[2] = {
			{.fd = c->fd, .events = POLLIN},
			{.fd = stop_pipe[0], .events = POLLIN},
		};
		int r = poll(fds, 2, session_meters_due_ms(s));
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (fds[1].revents)
			break;
		if (r == 0) {
			char *event = session_meters_event(s);
			bool ok = write_line(c->fd, event);
			bfree(event);
			if (!ok)
				break;
			continue;
		}

		ssize_t got = read(c->fd, buf, sizeof(buf));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break; /* 切断 */
		dstr_ncat(&pending, buf, (size_t)got);

		char *nl;
		bool ok = true;
		while (ok && pending.array && (nl = strchr(pending.array, '\n')) != NULL) {
			*nl = '\0';
			if (nl > pending.array && nl[-1] == '\r')
				nl[-1] = '\0';
			if (pending.array[0] != '\0') {
				char *res = session_handle(s, pending.array);
				ok = write_line(c->fd, res);
				bfree(res);
			}
			dstr_remove(&pending, 0, (size_t)(nl - pending.array) + 1);
		}
		if (!ok || pending.len > MAX_LINE)
			break;
	}

	dstr_free(&pending);
	session_destroy(s);
	pthread_mutex_lock(&clients_mutex);
	close(c->fd);
	c->fd = -1;
	c->done = true;
	pthread_mutex_unlock(&clients_mutex);
	return NULL;
}

/* 終わったクライアントのスレッドを回収する。終わり際のスレッドも mutex を取るので、join は mutex の外で行う */
static void reap_clients(bool all)
{
	DARRAY(struct client *) ended;
	da_init(ended);
	pthread_mutex_lock(&clients_mutex);
	for (size_t i = clients.num; i > 0; i--) {
		struct client *c = clients.array[i - 1];
		if (all || c->done) {
			da_push_back(ended, &c);
			da_erase(clients, i - 1);
		}
	}
	pthread_mutex_unlock(&clients_mutex);
	for (size_t i = 0; i < ended.num; i++) {
		pthread_join(ended.array[i]->thread, NULL);
		bfree(ended.array[i]);
	}
	da_free(ended);
}

/* 書き込みの途中で止まっているスレッドも、停止の合図を待たずに抜けさせる */
static void shutdown_clients(void)
{
	pthread_mutex_lock(&clients_mutex);
	for (size_t i = 0; i < clients.num; i++) {
		if (clients.array[i]->fd >= 0)
			shutdown(clients.array[i]->fd, SHUT_RDWR);
	}
	pthread_mutex_unlock(&clients_mutex);
}

/* 同じユーザーのプロセスだけを通す。ソケットファイルの権限に加えて、相手の uid でも確かめる */
static bool same_user(int fd)
{
	uid_t uid;
	gid_t gid;
	return getpeereid(fd, &uid, &gid) == 0 && uid == geteuid();
}

static void *listen_main(void *param)
{
	UNUSED_PARAMETER(param);
	os_set_thread_name("mixer-bridge: listen");

	for (;;) {
		struct pollfd fds[2] = {
			{.fd = listen_fd, .events = POLLIN},
			{.fd = stop_pipe[0], .events = POLLIN},
		};
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			obs_log(LOG_ERROR, "poll failed (%s)", strerror(errno));
			break;
		}
		if (fds[1].revents)
			break;

		int fd = accept(listen_fd, NULL, NULL);
		if (fd < 0)
			continue;
		set_cloexec(fd);
		if (!same_user(fd)) {
			close(fd);
			continue;
		}
		/* 切断済みの相手に書くと SIGPIPE で OBS ごと落ちるので、エラーで返させる */
		int on = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
		struct timeval timeout = {.tv_sec = SEND_TIMEOUT_SEC};
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

		reap_clients(false);
		struct client *c = bzalloc(sizeof(*c));
		c->fd = fd;
		if (pthread_create(&c->thread, NULL, client_main, c) != 0) {
			close(fd);
			bfree(c);
			continue;
		}
		pthread_mutex_lock(&clients_mutex);
		da_push_back(clients, &c);
		pthread_mutex_unlock(&clients_mutex);
	}
	return NULL;
}

static void close_stop_pipe(void)
{
	for (int i = 0; i < 2; i++) {
		if (stop_pipe[i] >= 0)
			close(stop_pipe[i]);
		stop_pipe[i] = -1;
	}
}

bool server_start(void)
{
	if (!make_address())
		return false;
	if (pipe(stop_pipe) != 0)
		return false;
	set_cloexec(stop_pipe[0]);
	set_cloexec(stop_pipe[1]);
	if (pthread_mutex_init(&clients_mutex, NULL) != 0) {
		close_stop_pipe();
		return false;
	}
	if (!lock_address() || !open_listener()) {
		unlock_address();
		pthread_mutex_destroy(&clients_mutex);
		close_stop_pipe();
		return false;
	}
	da_init(clients);
	if (pthread_create(&listen_thread, NULL, listen_main, NULL) != 0) {
		close(listen_fd);
		listen_fd = -1;
		unlink(addr.sun_path);
		unlock_address();
		pthread_mutex_destroy(&clients_mutex);
		close_stop_pipe();
		return false;
	}
	running = true;
	obs_log(LOG_INFO, "listening on %s", addr.sun_path);
	return true;
}

void server_stop(void)
{
	if (!running)
		return;
	close(stop_pipe[1]);
	stop_pipe[1] = -1;
	pthread_join(listen_thread, NULL);
	shutdown_clients();
	reap_clients(true);
	da_free(clients);
	pthread_mutex_destroy(&clients_mutex);
	close(listen_fd);
	listen_fd = -1;
	unlink(addr.sun_path);
	unlock_address();
	close_stop_pipe();
	running = false;
}
