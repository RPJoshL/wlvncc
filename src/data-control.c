#include "data-control.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <aml.h>
#include <seat.h>
#include <vnc.h>

struct receive_context {
	//struct data_control* data_control;
	struct zwlr_data_control_offer_v1* offer;
	int fd;
	FILE* mem_fp;
	size_t mem_size;
	char* mem_data;
	struct data_control* self;
};

static char* mime_type = "text/plain;charset=utf-8";

static bool isStringEqual(int lena, char* a, int lenb, char* b) {
	if (lena != lenb) {
		return false;
	}

	// Compare every character
	for (size_t i = 0; i < lena; ++i) {
		if (a[i] != b[i]) {
			return false;
		}
	}

	return true;
}

static void on_receive(struct aml_handler* handler)
{
	struct receive_context* ctx = aml_get_userdata(handler);
	int fd = aml_get_fd(handler);
	assert(ctx->fd == fd);

	char buf[4096];

	ssize_t ret = read(fd, &buf, sizeof(buf));
	if (ret > 0) {
		fwrite(&buf, 1, ret, ctx->mem_fp);
		return;
	}

	fclose(ctx->mem_fp);
	ctx->mem_fp = NULL;

	if (ctx->mem_size && ctx->self->vnc_write_clipboard) {
		// If we receive clipboard data from the VNC server, we set the clipboard of the client.
		// This "change" of clipboard data results into this "on_receive" event. That would leed
		// to an endless loop...
		// So we check if we send exactle that clipboard string to avoid an loop
		if (!ctx->self->cb_data || !isStringEqual(ctx->self->cb_len, ctx->self->cb_data, strlen(ctx->mem_data), ctx->mem_data)) {
			//printf("Received data FROM clipboard: %s\n", ctx->mem_data);
			ctx->self->vnc_write_clipboard(ctx->mem_data, ctx->mem_size);
		} 
	}

	aml_stop(aml_get_default(), handler);
}

static void destroy_receive_context(void* raw_ctx)
{
	struct receive_context* ctx = raw_ctx;
	int fd = ctx->fd;

	if (ctx->mem_fp)
		fclose(ctx->mem_fp);
	free(ctx->mem_data);
	zwlr_data_control_offer_v1_destroy(ctx->offer);
	close(fd);
	free(ctx);
}

static void receive_data(void* data,
	struct zwlr_data_control_offer_v1* offer)
{
	struct data_control* self = data;
	int pipe_fd[2];

	if (pipe(pipe_fd) == -1) {
		printf("pipe() failed");
		return;
	}

	struct receive_context* ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		printf("OOM");
		close(pipe_fd[0]);
		close(pipe_fd[1]);
		return;
	}

	zwlr_data_control_offer_v1_receive(self->offer, mime_type, pipe_fd[1]);
	//wl_display_flush(self->wl_display);
	close(pipe_fd[1]);

	ctx->self = self;
	ctx->fd = pipe_fd[0];
	//ctx->data_control = self;
	ctx->offer = self->offer;
	ctx->mem_fp = open_memstream(&ctx->mem_data, &ctx->mem_size);
	if (!ctx->mem_fp) {
		close(ctx->fd);
		free(ctx);
		printf("open_memstream() failed");
		return;
	}

	struct aml_handler* handler = aml_handler_new(ctx->fd, on_receive,
			ctx, destroy_receive_context);
	if (!handler) {
		close(ctx->fd);
		free(ctx);
		return;
	}

	aml_start(aml_get_default(), handler);
	aml_unref(handler);
}

static void data_control_offer(void* data,
	struct zwlr_data_control_offer_v1* zwlr_data_control_offer_v1,
	const char* mime_type)
{
	struct data_control* self = data;

	if (self->offer)
		return;
	if (strcmp(mime_type, mime_type) != 0) {
		return;
	}

	self->offer = zwlr_data_control_offer_v1;
}

struct zwlr_data_control_offer_v1_listener data_control_offer_listener = {
	data_control_offer
};

static void data_control_device_offer(void* data,
	struct zwlr_data_control_device_v1* zwlr_data_control_device_v1,
	struct zwlr_data_control_offer_v1* id)
{
	if (!id)
		return;

	zwlr_data_control_offer_v1_add_listener(id, &data_control_offer_listener, data);
}

static void data_control_device_selection(void* data,
	struct zwlr_data_control_device_v1* zwlr_data_control_device_v1,
	struct zwlr_data_control_offer_v1* id)
{
	struct data_control* self = data;
	if (id && self->offer == id) {
		receive_data(data, id);
		self->offer = NULL;
	}
}

static void data_control_device_primary_selection(void* data,
	struct zwlr_data_control_device_v1* zwlr_data_control_device_v1,
	struct zwlr_data_control_offer_v1* id)
{
	struct data_control* self = data;
	if (id && self->offer == id) {
		receive_data(data, id);
		self->offer = NULL;
		return;
	}
}

static void data_control_device_finished(void* data,
	struct zwlr_data_control_device_v1* zwlr_data_control_device_v1)
{
	zwlr_data_control_device_v1_destroy(zwlr_data_control_device_v1);
}

static struct zwlr_data_control_device_v1_listener data_control_device_listener = {
	.data_offer = data_control_device_offer,
	.selection = data_control_device_selection,
	.finished = data_control_device_finished,
	.primary_selection = data_control_device_primary_selection
};


static void
data_control_source_send(void* data,
	struct zwlr_data_control_source_v1* zwlr_data_control_source_v1,
	const char* mime_type,
	int32_t fd)
{
	struct data_control* self = data;
	char* d = self->cb_data;
	size_t len = self->cb_len;
	int ret;

	assert(d);

	ret = write(fd, d, len);

	if (ret < (int)len)
		printf("write from clipboard incomplete");

	close(fd);
}

static void data_control_source_cancelled(void* data,
	struct zwlr_data_control_source_v1* zwlr_data_control_source_v1)
{
	struct data_control* self = data;

	if (self->selection == zwlr_data_control_source_v1) {
		self->selection = NULL;
	}
	if (self->primary_selection == zwlr_data_control_source_v1) {
		self->primary_selection = NULL;
	}
	zwlr_data_control_source_v1_destroy(zwlr_data_control_source_v1);
}

struct zwlr_data_control_source_v1_listener data_control_source_listener = {
	.send = data_control_source_send,
	.cancelled = data_control_source_cancelled
};

static struct zwlr_data_control_source_v1* set_selection(struct data_control* self, bool primary) {
	struct zwlr_data_control_source_v1* selection;
	selection = zwlr_data_control_manager_v1_create_data_source(self->manager);
	if (selection == NULL) {
		printf("zwlr_data_control_manager_v1_create_data_source() failed");
		free(self->cb_data);
		self->cb_data = NULL;
		return NULL;
	}

	zwlr_data_control_source_v1_add_listener(selection, &data_control_source_listener, self);
	zwlr_data_control_source_v1_offer(selection, mime_type);

	if (primary)
		zwlr_data_control_device_v1_set_primary_selection(self->device, selection);
	else
		zwlr_data_control_device_v1_set_selection(self->device, selection);

	return selection;
}

void data_control_init(struct data_control* self, struct seat* seat, struct zwlr_data_control_manager_v1 *manager) {
	self->manager = manager;
	self->device = zwlr_data_control_manager_v1_get_data_device(self->manager, seat->wl_seat);
	self->cb_data = NULL;
	self->cb_len = 0;

	zwlr_data_control_device_v1_add_listener(self->device, &data_control_device_listener, self);
}

void data_control_to_clipboard(struct data_control* self, const char* text, size_t len)
{
	//printf("Writing text TO CLIPBOARD: %s\n", text);
	if (!len) {
		printf("%s called with 0 length", __func__);
		return;
	}
	if (self->cb_data) {
		free(self->cb_data);
	}
	

	self->cb_data = malloc(len);
	if (!self->cb_data) {
		printf("OOM");
		return;
	}

	memcpy(self->cb_data, text, len);
	self->cb_len = len;
	// Set copy/paste buffer
	self->selection = set_selection(self, false);
	// Set highlight/middle_click buffer
	self->primary_selection = set_selection(self, true);
}

void data_control_destroy(struct data_control* self)
{
	if (self->selection) {
		zwlr_data_control_source_v1_destroy(self->selection);
		self->selection = NULL;
	}
	if (self->primary_selection) {
		zwlr_data_control_source_v1_destroy(self->primary_selection);
		self->primary_selection = NULL;
	}
	zwlr_data_control_device_v1_destroy(self->device);
	free(self->cb_data);
}