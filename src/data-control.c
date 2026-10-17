#include "data-control.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <aml.h>
#include <seat.h>
#include <vnc.h>

void run_main_loop_once(void);

#define MAX_CLIPBOARD_BYTES (1024U * 1024U)

static const char clipboard_mime[] = "text/plain;charset=utf-8";
static const char clipboard_own_mime[] = "application/x-wlvncc-clipboard";

struct receive_context {
	int fd;
	FILE* mem_fp;
	size_t mem_size;
	char* mem_data;
	struct data_control* self;
};

struct forward_job {
	char* data;
	size_t len;
	vnc_write_clipboard_t write_fn;
};

static bool clipboard_mime_matches(const char* mime)
{
	if (!mime)
		return false;
	if (strcmp(mime, clipboard_mime) == 0)
		return true;
	if (strcmp(mime, "text/plain") == 0)
		return true;
	if (strcmp(mime, "UTF8_STRING") == 0)
		return true;
	return strncmp(mime, "text/plain;", 11) == 0;
}

static int clipboard_mime_rank(const char* mime)
{
	if (!mime)
		return 0;
	if (strcmp(mime, "UTF8_STRING") == 0)
		return 100;
	if (strcmp(mime, clipboard_mime) == 0)
		return 90;
	if (strncmp(mime, "text/plain;charset=utf-8", 24) == 0)
		return 85;
	if (strncmp(mime, "text/plain;", 11) == 0)
		return 50;
	if (strcmp(mime, "text/plain") == 0)
		return 10;
	return 0;
}

static int set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static ssize_t write_all(int fd, const char* data, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, data + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct pollfd pfd = { .fd = fd, .events = POLLOUT };
				int pr = poll(&pfd, 1, 50);
				if (pr < 0) {
					if (errno == EINTR)
						continue;
					return -1;
				}
				if (pr == 0)
					run_main_loop_once();
				continue;
			}
			return -1;
		}
		if (n == 0)
			return -1;
		off += (size_t)n;
	}
	return (ssize_t)off;
}

static void forward_idle(struct aml_idle* idle)
{
	struct forward_job* job = aml_idle_get_userdata(idle);

	if (job->write_fn && job->len)
		job->write_fn(job->data, job->len);

	aml_stop(aml_get_default(), idle);
}

static void forward_job_free(void* userdata)
{
	struct forward_job* job = userdata;
	free(job->data);
	free(job);
}

static void schedule_forward(struct data_control* self, const char* data,
		size_t len)
{
	if (!self->vnc_write_clipboard || !len || len > MAX_CLIPBOARD_BYTES)
		return;

	struct forward_job* job = malloc(sizeof(*job));
	if (!job)
		return;

	job->data = malloc(len);
	if (!job->data) {
		free(job);
		return;
	}

	memcpy(job->data, data, len);
	job->len = len;
	job->write_fn = self->vnc_write_clipboard;

	struct aml_idle* idle = aml_idle_new(forward_idle, job, forward_job_free);
	if (!idle) {
		forward_job_free(job);
		return;
	}

	aml_start(aml_get_default(), idle);
	aml_unref(idle);
}

static void on_receive(struct aml_handler* handler)
{
	struct receive_context* ctx = aml_get_userdata(handler);
	int fd = aml_get_fd(handler);
	char buf[4096];

	ssize_t ret = read(fd, buf, sizeof(buf));
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		ret = 0;
	}

	if (ret > 0) {
		fwrite(buf, 1, (size_t)ret, ctx->mem_fp);
		fflush(ctx->mem_fp);
		return;
	}

	fclose(ctx->mem_fp);
	ctx->mem_fp = NULL;

	if (ctx->mem_size && ctx->mem_size <= MAX_CLIPBOARD_BYTES)
		schedule_forward(ctx->self, ctx->mem_data, ctx->mem_size);
	else if (ctx->mem_size > MAX_CLIPBOARD_BYTES)
		fprintf(stderr, "ignoring clipboard larger than %u bytes\n",
				MAX_CLIPBOARD_BYTES);

	aml_stop(aml_get_default(), handler);
}

static void destroy_receive_context(void* raw_ctx)
{
	struct receive_context* ctx = raw_ctx;

	if (ctx->mem_fp)
		fclose(ctx->mem_fp);
	free(ctx->mem_data);
	close(ctx->fd);

	if (ctx->self) {
		ctx->self->receive_in_progress = false;
		ctx->self->receive_handler = NULL;
		if (ctx->self->receiving_offer) {
			zwlr_data_control_offer_v1_destroy(ctx->self->receiving_offer);
			ctx->self->receiving_offer = NULL;
		}
	}
	free(ctx);
}

static void cancel_receive(struct data_control* self)
{
	if (!self->receive_handler)
		return;

	aml_stop(aml_get_default(), self->receive_handler);
	self->receive_handler = NULL;
}

static void receive_data(struct data_control* self,
		struct zwlr_data_control_offer_v1* offer)
{
	int pipe_fd[2];

	if (self->receive_in_progress)
		cancel_receive(self);

	if (pipe(pipe_fd) == -1)
		return;

	if (set_nonblocking(pipe_fd[0]) < 0) {
		close(pipe_fd[0]);
		close(pipe_fd[1]);
		return;
	}

	const char* mime = self->offer_mime[0] ? self->offer_mime : clipboard_mime;

	struct receive_context* ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		close(pipe_fd[0]);
		close(pipe_fd[1]);
		return;
	}

	zwlr_data_control_offer_v1_receive(offer, mime, pipe_fd[1]);
	close(pipe_fd[1]);

	ctx->self = self;
	ctx->fd = pipe_fd[0];
	ctx->mem_fp = open_memstream(&ctx->mem_data, &ctx->mem_size);
	if (!ctx->mem_fp) {
		close(ctx->fd);
		free(ctx);
		return;
	}

	struct aml_handler* handler = aml_handler_new(ctx->fd, on_receive, ctx,
			destroy_receive_context);
	if (!handler) {
		fclose(ctx->mem_fp);
		close(ctx->fd);
		free(ctx);
		return;
	}

	self->receive_in_progress = true;
	self->receiving_offer = offer;
	self->receive_handler = handler;
	aml_start(aml_get_default(), handler);
	aml_unref(handler);
}

static void data_control_offer(void* data,
		struct zwlr_data_control_offer_v1* offer, const char* mime)
{
	struct data_control* self = data;

	if (strcmp(mime, clipboard_own_mime) == 0) {
		self->skip_offer = true;
		return;
	}

	if (!clipboard_mime_matches(mime))
		return;

	if (!self->offer)
		self->offer = offer;

	if (clipboard_mime_rank(mime) >= clipboard_mime_rank(self->offer_mime))
		snprintf(self->offer_mime, sizeof(self->offer_mime), "%s", mime);
}

static const struct zwlr_data_control_offer_v1_listener data_control_offer_listener = {
	.offer = data_control_offer,
};

static void data_control_device_offer(void* data,
		struct zwlr_data_control_device_v1* device,
		struct zwlr_data_control_offer_v1* id)
{
	struct data_control* self = data;

	if (!id)
		return;

	self->offer = NULL;
	self->offer_mime[0] = '\0';
	self->skip_offer = false;

	zwlr_data_control_offer_v1_add_listener(id, &data_control_offer_listener,
			self);
}

static void data_control_device_selection(void* data,
		struct zwlr_data_control_device_v1* device,
		struct zwlr_data_control_offer_v1* id)
{
	struct data_control* self = data;

	if (!id) {
		self->offer = NULL;
		self->offer_mime[0] = '\0';
		self->skip_offer = false;
		return;
	}

	if (id == self->offer && !self->skip_offer)
		receive_data(self, id);

	if (id != self->receiving_offer)
		zwlr_data_control_offer_v1_destroy(id);

	self->offer = NULL;
	self->offer_mime[0] = '\0';
	self->skip_offer = false;
}

static void data_control_device_primary_selection(void* data,
		struct zwlr_data_control_device_v1* device,
		struct zwlr_data_control_offer_v1* id)
{
	/* Clipboard import is handled only via the regular selection event.
	 * Destroying the offer here often races selection and drops data. */
	(void)data;
	(void)device;
	(void)id;
}

static void data_control_device_finished(void* data,
		struct zwlr_data_control_device_v1* device)
{
	zwlr_data_control_device_v1_destroy(device);
}

static const struct zwlr_data_control_device_v1_listener data_control_device_listener = {
	.data_offer = data_control_device_offer,
	.selection = data_control_device_selection,
	.finished = data_control_device_finished,
	.primary_selection = data_control_device_primary_selection,
};

static void data_control_source_send(void* data,
		struct zwlr_data_control_source_v1* source, const char* mime,
		int32_t fd)
{
	struct data_control* self = data;

	(void)source;

	if (!self->cb_data || !self->cb_len) {
		close(fd);
		return;
	}

	if (strcmp(mime, clipboard_own_mime) == 0) {
		close(fd);
		return;
	}

	if (!clipboard_mime_matches(mime)) {
		close(fd);
		return;
	}

	if (write_all(fd, self->cb_data, self->cb_len) < 0)
		perror("clipboard write");
	close(fd);
}

static void data_control_source_cancelled(void* data,
		struct zwlr_data_control_source_v1* source)
{
	struct data_control* self = data;

	if (self->selection == source)
		self->selection = NULL;
	if (self->primary_selection == source)
		self->primary_selection = NULL;
	zwlr_data_control_source_v1_destroy(source);
}

static const struct zwlr_data_control_source_v1_listener data_control_source_listener = {
	.send = data_control_source_send,
	.cancelled = data_control_source_cancelled,
};

static struct zwlr_data_control_source_v1* set_selection(
		struct data_control* self, bool primary)
{
	struct zwlr_data_control_source_v1* source;

	source = zwlr_data_control_manager_v1_create_data_source(self->manager);
	if (!source)
		return NULL;

	zwlr_data_control_source_v1_add_listener(source,
			&data_control_source_listener, self);
	zwlr_data_control_source_v1_offer(source, "UTF8_STRING");
	zwlr_data_control_source_v1_offer(source, clipboard_mime);
	zwlr_data_control_source_v1_offer(source, "text/plain");
	zwlr_data_control_source_v1_offer(source, clipboard_own_mime);

	if (primary)
		zwlr_data_control_device_v1_set_primary_selection(self->device,
				source);
	else
		zwlr_data_control_device_v1_set_selection(self->device, source);

	return source;
}

void data_control_init(struct data_control* self, struct seat* seat,
		struct zwlr_data_control_manager_v1* manager)
{
	self->manager = manager;
	self->device = zwlr_data_control_manager_v1_get_data_device(self->manager,
			seat->wl_seat);
	self->cb_data = NULL;
	self->cb_len = 0;
	self->offer = NULL;
	self->receiving_offer = NULL;
	self->offer_mime[0] = '\0';
	self->skip_offer = false;
	self->receive_in_progress = false;
	self->receive_handler = NULL;

	zwlr_data_control_device_v1_add_listener(self->device,
			&data_control_device_listener, self);
}

void data_control_to_clipboard(struct data_control* self, const char* text,
		size_t len)
{
	if (!len || len > MAX_CLIPBOARD_BYTES)
		return;

	if (self->selection) {
		zwlr_data_control_source_v1_destroy(self->selection);
		self->selection = NULL;
	}
	if (self->primary_selection) {
		zwlr_data_control_source_v1_destroy(self->primary_selection);
		self->primary_selection = NULL;
	}

	free(self->cb_data);
	self->cb_data = malloc(len);
	if (!self->cb_data)
		return;

	memcpy(self->cb_data, text, len);
	self->cb_len = len;

	self->selection = set_selection(self, false);
	self->primary_selection = set_selection(self, true);
}

void data_control_destroy(struct data_control* self)
{
	cancel_receive(self);

	if (self->receiving_offer) {
		zwlr_data_control_offer_v1_destroy(self->receiving_offer);
		self->receiving_offer = NULL;
	}
	if (self->selection) {
		zwlr_data_control_source_v1_destroy(self->selection);
		self->selection = NULL;
	}
	if (self->primary_selection) {
		zwlr_data_control_source_v1_destroy(self->primary_selection);
		self->primary_selection = NULL;
	}
	if (self->device)
		zwlr_data_control_device_v1_destroy(self->device);
	free(self->cb_data);
}
