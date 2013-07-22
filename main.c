/* libuev - Micro event loop library
 *
 * Copyright (c) 2012  Flemming Madsen <flemming!madsen()madsensoft!dk>
 * Copyright (c) 2013  Joachim Nilsson <troglobit()gmail!com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <stdlib.h>		/* calloc(), free() */
#include <sys/epoll.h>
#include <unistd.h>		/* close(), read() */

#include "uev.h"

/* Private to libuev, do not use directly! */
uev_io_t *uev_watcher_create(uev_t *ctx, uev_type_t type, int fd, uev_dir_t dir, uev_cb_t *handler, void *data)
{
	uev_io_t *w;
	struct epoll_event ev;

	if (!ctx || !handler) {
		errno = EINVAL;
		return NULL;
	}

	w = (uev_io_t *)calloc(1, sizeof(*w));
	if (!w)
		return NULL;

	w->fd      = fd;
	w->dir     = dir;
	w->type    = type;
	w->handler = (void *)handler;
	w->data    = data;

	ev.events   = dir == UEV_DIR_OUTBOUND ? EPOLLOUT : EPOLLIN;
	ev.data.ptr = w;
	if (epoll_ctl(ctx->efd, EPOLL_CTL_ADD, fd, &ev) < 0) {
		free(w);
		return NULL;
	}

	LIST_INSERT_HEAD(&ctx->watchers, w, link);

	return w;
}

/* Private to libuev, do not use directly! */
int uev_watcher_delete(uev_t *ctx, uev_io_t *w)
{
	if (!ctx || !w) {
		errno = EINVAL;
		return -1;
	}

	/* Remove from kernel */
	epoll_ctl(ctx->efd, EPOLL_CTL_DEL, w->fd, NULL);

	/* Remove from internal list */
	LIST_REMOVE(w, link);
	free(w);

	return 0;
}

/**
 * Create an event loop context
 *
 * @return Returns a new uev_t context, or %NULL on error.
 */
uev_t *uev_ctx_create(void)
{
	int fd;
	uev_t *ctx;

	fd = epoll_create(1);
	if (fd < 0)
		return NULL;

	ctx = (uev_t *)calloc(1, sizeof(*ctx));
	if (!ctx)
		goto exit;

	ctx->events = (struct epoll_event *)calloc(UEV_MAX_EVENTS, sizeof(struct epoll_event));
	if (!ctx->events) {
		free(ctx);
	exit:
		close(fd);
		return NULL;
	}

	ctx->efd = fd;
	LIST_INIT(&ctx->watchers);

	return ctx;
}

/**
 * Destroy an event loop context
 * @param ctx A valid libuev context
 */
void uev_ctx_delete(uev_t *ctx)
{
	while (!LIST_EMPTY(&ctx->watchers)) {
		uev_io_t *w = LIST_FIRST(&ctx->watchers);

		if (UEV_TIMER_TYPE == w->type)
			uev_timer_delete(ctx, w);
		else
			uev_io_delete(ctx, w);
	}

	close(ctx->efd);
	free(ctx->events);
	free(ctx);
}

/**
 * Start the event loop
 * @param ctx A valid libuev context
 *
 * @return POSIX OK(0) upon successful termination of the event loop, or non-zero on error.
 */
int uev_run(uev_t *ctx)
{
	int result = 0;
	uev_io_t *w;

        if (!ctx) {
		errno = EINVAL;
                return -1;
	}

	/* Start the event loop */
	ctx->running = 1;

	/* Start all dormant timers */
	LIST_FOREACH(w, &ctx->watchers, link) {
		if (UEV_TIMER_TYPE == w->type)
			uev_timer_set(ctx, w, w->timeout, w->period);
	}

	while (ctx->running) {
		int i, nfds;

		while ((nfds = epoll_wait(ctx->efd, ctx->events, UEV_MAX_EVENTS, -1)) < 0) {
			if (EINTR == errno)
				continue; /* Signalled, try again */

			result = -1;
			ctx->running = 0;
			break;
		}

		for (i = 0; i < nfds; i++) {
			w = (uev_io_t *)ctx->events[i].data.ptr;

			if (w->handler)
				w->handler((struct uev *)ctx, w, w->data);

			if (UEV_TIMER_TYPE == w->type) {
				uint64_t exp;

				if (read(w->fd, &exp, sizeof(exp)) != sizeof(exp)) {
					result = -1;
					ctx->running = 0;
				}

				if (!w->period)
					uev_timer_delete(ctx, w);
			}
		}
	}

	return result;
}

/**
 * Terminate the event loop
 * @param ctx A valid libuev context
 */
void uev_exit(uev_t *ctx)
{
	ctx->running = 0;
}

/**
 * Local Variables:
 *  version-control: t
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */