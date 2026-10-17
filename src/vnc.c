/*
 * Copyright (c) 2020 - 2022 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <pixman.h>
#include <libdrm/drm_fourcc.h>
#include <libavutil/frame.h>
#include <stdio.h>
#include <aml.h>
#include <data-control.h>

#include "rfbclient.h"
#include "vnc.h"
#include "pointer.h"
#include "open-h264.h"
#include "usdt.h"

#define RFB_ENCODING_OPEN_H264 50
#define RFB_ENCODING_PTS -1000

#define NO_PTS UINT64_MAX

extern const unsigned short code_map_linux_to_qnum[];
extern const unsigned int code_map_linux_to_qnum_len;

static uint64_t vnc_client_htonll(uint64_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap64(x);
#else
	return x;
#endif
}

static bool vnc_client_lock_handler(struct vnc_client* self)
{
	if (self->handler_lock)
		return false;

	self->handler_lock = true;
	return true;
}

static void vnc_client_unlock_handler(struct vnc_client* self)
{
	assert(self->handler_lock);
	self->handler_lock = false;
}

static rfbBool vnc_client_alloc_fb(rfbClient* client)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	return self->alloc_fb(self) < 0 ? FALSE : TRUE;
}

static void vnc_client_update_box(rfbClient* client, int x, int y, int width,
		int height)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	if (self->current_rect_is_av_frame) {
		self->current_rect_is_av_frame = false;
		return;
	}

	pixman_region_union_rect(&self->damage, &self->damage, x, y, width,
			height);
}

void vnc_client_clear_av_frames(struct vnc_client* self)
{
	for (int i = 0; i < self->n_av_frames; ++i) {
		av_frame_unref(self->av_frames[i]->frame);
		av_frame_free(&self->av_frames[i]->frame);
		free(self->av_frames[i]);
	}
	self->n_av_frames = 0;
}

static void vnc_client_start_update(rfbClient* client)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	self->pts = NO_PTS;
	pixman_region_clear(&self->damage);
	vnc_client_clear_av_frames(self);

	self->is_updating = true;
}

static void vnc_client_cancel_update(rfbClient* client)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	self->is_updating = false;
}

static void vnc_client_finish_update(rfbClient* client)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	DTRACE_PROBE2(wlvncc, vnc_client_finish_update, client, self->pts);

	self->is_updating = false;

	self->update_fb(self);
}

static void vnc_client_got_cut_text(rfbClient* client, const char* text,
		int len)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	if (self->cut_text)
		self->cut_text(self, text, len);
}

static uint32_t vnc_client_rfb_pixel_to_argb8888(rfbClient* client,
		const uint8_t* src, int bytes_per_pixel)
{
	uint32_t pixel = 0;

	switch (bytes_per_pixel) {
	case 1:
		pixel = *src;
		break;
	case 2:
		pixel = *(const uint16_t*)src;
		break;
	case 4:
		pixel = *(const uint32_t*)src;
		break;
	default:
		return 0;
	}

	uint32_t r = (pixel >> client->format.redShift) & client->format.redMax;
	uint32_t g = (pixel >> client->format.greenShift) &
			client->format.greenMax;
	uint32_t b = (pixel >> client->format.blueShift) &
			client->format.blueMax;

	if (client->format.redMax)
		r = r * 0x100 / (client->format.redMax + 1);
	if (client->format.greenMax)
		g = g * 0x100 / (client->format.greenMax + 1);
	if (client->format.blueMax)
		b = b * 0x100 / (client->format.blueMax + 1);

	return (0xffu << 24) | (r << 16) | (g << 8) | b;
}

static void vnc_client_got_cursor_shape(rfbClient* client, int xhot, int yhot,
		int width, int height, int bytes_per_pixel)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	if (!self->pointer_collection)
		return;

	if (width <= 0 || height <= 0) {
		pointer_collection_clear_remote_cursor(self->pointer_collection);
		return;
	}

	size_t n_pixels = (size_t)width * height;
	uint32_t* argb = malloc(n_pixels * sizeof(uint32_t));
	if (!argb)
		return;

	for (size_t i = 0; i < n_pixels; ++i) {
		const uint8_t* src = client->rcSource + i * bytes_per_pixel;

		if (!client->rcMask[i]) {
			argb[i] = 0;
			continue;
		}

		argb[i] = vnc_client_rfb_pixel_to_argb8888(client, src,
				bytes_per_pixel);
	}

	pointer_collection_update_remote_cursor(self->pointer_collection, argb,
			width, height, xhot, yhot);
	free(argb);
}

static rfbBool vnc_client_handle_open_h264_rect(rfbClient* client,
		rfbFramebufferUpdateRectHeader* rect_header)
{
	if ((int)rect_header->encoding != RFB_ENCODING_OPEN_H264)
		return FALSE;

	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	if (!self->open_h264)
		self->open_h264 = open_h264_create(client);

	if (!self->open_h264)
		return false;

	AVFrame* frame = open_h264_decode_rect(self->open_h264, rect_header);
	if (!frame)
		return false;

	assert(self->n_av_frames < VNC_CLIENT_MAX_AV_FRAMES);

	struct vnc_av_frame* f = calloc(1, sizeof(*f));
	if (!f) {
		av_frame_unref(frame);
		av_frame_free(&frame);
		return false;
	}

	f->frame = frame;
	f->x = rect_header->r.x;
	f->y = rect_header->r.y;
	f->width = rect_header->r.w;
	f->height = rect_header->r.h;

	self->av_frames[self->n_av_frames++] = f;

	self->current_rect_is_av_frame = true;
	return true;
}

static void vnc_client_init_open_h264(void)
{
	static int encodings[] = { RFB_ENCODING_OPEN_H264, 0 };
	static rfbClientProtocolExtension ext = {
		.encodings = encodings,
		.handleEncoding = vnc_client_handle_open_h264_rect,
	};
	rfbClientRegisterExtension(&ext);
}

static rfbBool vnc_client_handle_pts_rect(rfbClient* client,
		rfbFramebufferUpdateRectHeader* rect_header)
{
	if ((int)rect_header->encoding != RFB_ENCODING_PTS)
		return FALSE;

	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	uint64_t pts_msg = 0;
	if (!ReadFromRFBServer(self->client, (char*)&pts_msg, sizeof(pts_msg)))
		return FALSE;

	self->pts = vnc_client_htonll(pts_msg);

	DTRACE_PROBE1(wlvncc, vnc_client_handle_pts_rect, self->pts);

	return TRUE;
}

static void vnc_client_init_pts_ext(void)
{
	static int encodings[] = { RFB_ENCODING_PTS, 0 };
	static rfbClientProtocolExtension ext = {
		.encodings = encodings,
		.handleEncoding = vnc_client_handle_pts_rect,
	};
	rfbClientRegisterExtension(&ext);
}

struct vnc_client* vnc_client_create(struct data_control* data_control)
{
	vnc_client_init_open_h264();
	vnc_client_init_pts_ext();

	struct vnc_client* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	/* These are defaults that can be changed with
	 * vnc_client_set_pixel_format().
	 */
	int bits_per_sample = 8;
	int samples_per_pixel = 3;
	int bytes_per_pixel = 4;

	rfbClient* client = rfbGetClient(bits_per_sample, samples_per_pixel,
			bytes_per_pixel);
	if (!client)
		goto failure;

	self->client = client;
	self->data_control = data_control;
	rfbClientSetClientData(client, NULL, self);

	client->MallocFrameBuffer = vnc_client_alloc_fb;
	client->GotFrameBufferUpdate = vnc_client_update_box;
	client->FinishedFrameBufferUpdate = vnc_client_finish_update;
	client->StartingFrameBufferUpdate = vnc_client_start_update;
	client->CancelledFrameBufferUpdate = vnc_client_cancel_update;
	client->GotXCutText = vnc_client_got_cut_text;
	client->GotCursorShape = vnc_client_got_cursor_shape;
	self->cut_text = cut_text;

	self->pts = NO_PTS;

	return self;

failure:
	free(self);
	return NULL;
}

struct cut_text_apply_job {
	struct vnc_client* client;
	char* data;
	size_t len;
};

static void cut_text_apply_idle(struct aml_idle* idle)
{
	struct cut_text_apply_job* job = aml_idle_get_userdata(idle);

	if (job->client->data_control)
		data_control_to_clipboard(job->client->data_control, job->data,
				job->len);

	aml_stop(aml_get_default(), idle);
}

static void cut_text_apply_job_free(void* userdata)
{
	struct cut_text_apply_job* job = userdata;
	free(job->data);
	free(job);
}

void cut_text(struct vnc_client* self, const char* text, size_t size)
{
	if (!self->data_control || !size)
		return;

	struct cut_text_apply_job* job = malloc(sizeof(*job));
	if (!job)
		return;

	job->data = malloc(size);
	if (!job->data) {
		free(job);
		return;
	}

	memcpy(job->data, text, size);
	job->len = size;
	job->client = self;

	struct aml_idle* idle = aml_idle_new(cut_text_apply_idle, job,
			cut_text_apply_job_free);
	if (!idle) {
		cut_text_apply_job_free(job);
		return;
	}

	aml_start(aml_get_default(), idle);
	aml_unref(idle);
}

void vnc_client_destroy(struct vnc_client* self)
{
	vnc_client_clear_av_frames(self);
	open_h264_destroy(self->open_h264);
	rfbClientCleanup(self->client);
	free(self);
}

int vnc_client_connect(struct vnc_client* self, const char* address, int port)
{
	rfbClient* client = self->client;

	// This is needed for TLS authentication. TODO: Clean up
	free(client->serverHost);
	client->serverHost = strdup(address);

	return ConnectToRFBServer(client, address, port) ? 0 : -1;
}

int vnc_client_init(struct vnc_client* self)
{
	int rc = -1;
	rfbClient* client = self->client;

	vnc_client_lock_handler(self);

	if (!InitialiseRFBConnection(client))
		goto failure;

	client->width = client->si.framebufferWidth;
	client->height = client->si.framebufferHeight;

	if (!client->MallocFrameBuffer(client))
		goto failure;

	if (!SetFormatAndEncodings(client))
		goto failure;

	if (client->updateRect.x < 0) {
		client->updateRect.x = client->updateRect.y = 0;
		client->updateRect.w = client->width;
		client->updateRect.h = client->height;
	}

	if (!SendFramebufferUpdateRequest(client,
				client->updateRect.x, client->updateRect.y,
				client->updateRect.w, client->updateRect.h,
				FALSE))
		goto failure;

	SendIncrementalFramebufferUpdateRequest(client);
	SendIncrementalFramebufferUpdateRequest(client);

	rc = 0;
failure:
	vnc_client_unlock_handler(self);
	return rc;
}

int vnc_client_set_pixel_format(struct vnc_client* self, uint32_t format)
{
	rfbPixelFormat* dst = &self->client->format;
	int bpp = -1;

	switch (format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		dst->redShift = 16;
		dst->greenShift = 8;
		dst->blueShift = 0;
		bpp = 32;
		break;
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		dst->redShift = 0;
		dst->greenShift = 8;
		dst->blueShift = 16;
		bpp = 32;
		break;
	default:
		return -1;
	}

	switch (bpp) {
	case 32:
		dst->bitsPerPixel = 32;
		dst->depth = 24;
		dst->redMax = 0xff;
		dst->greenMax = 0xff;
		dst->blueMax = 0xff;
		break;
	default:
		abort();
	}

	dst->trueColour = 1;
	dst->bigEndian = FALSE;
	self->client->appData.requestedDepth = dst->depth;

	return 0;
}

int vnc_client_get_width(const struct vnc_client* self)
{
	return self->client->width;
}

int vnc_client_get_height(const struct vnc_client* self)
{
	return self->client->height;
}

int vnc_client_get_stride(const struct vnc_client* self)
{
	// TODO: What happens if bitsPerPixel == 24?
	return self->client->width * self->client->format.bitsPerPixel / 8;
}

void* vnc_client_get_fb(const struct vnc_client* self)
{
	return self->client->frameBuffer;
}

void vnc_client_set_fb(struct vnc_client* self, void* fb)
{
	self->client->frameBuffer = fb;
}

int vnc_client_get_fd(const struct vnc_client* self)
{
	return self->client->sock;
}

const char* vnc_client_get_desktop_name(const struct vnc_client* self)
{
	return self->client->desktopName;
}

int vnc_client_process(struct vnc_client* self)
{
	if (!ReadToBuffer(self->client))
		return -1;

	if (!vnc_client_lock_handler(self))
		return 0;

	int rc = 0;
	if (self->client->buffered > 0)
		rc = HandleRFBServerMessage(self->client) ? 0 : -1;

	vnc_client_unlock_handler(self);

	return rc;
}

void vnc_client_send_pointer_event(struct vnc_client* self, int x, int y,
		uint32_t button_mask)
{
	SendPointerEvent(self->client, x, y, button_mask);
}

void vnc_client_send_keyboard_event(struct vnc_client* self, uint32_t symbol,
		uint32_t code, bool is_pressed)
{
	if (code >= code_map_linux_to_qnum_len)
		return;

	uint32_t qnum = code_map_linux_to_qnum[code];
	if (!qnum)
		qnum = code;

	if (!SendExtendedKeyEvent(self->client, symbol, qnum, is_pressed))
		SendKeyEvent(self->client, symbol, is_pressed);
}

void vnc_client_set_encodings(struct vnc_client* self, const char* encodings)
{
	self->client->appData.encodingsString = encodings;
}

void vnc_client_set_quality_level(struct vnc_client* self, int value)
{
	self->client->appData.qualityLevel = value;
}

void vnc_client_set_compression_level(struct vnc_client* self, int value)
{
	self->client->appData.compressLevel = value;
}

void vnc_client_send_cut_text(struct vnc_client* self, const char* text,
		size_t len)
{
	if (len > INT_MAX) {
		fprintf(stderr, "clipboard too large for RFB (%zu bytes)\n", len);
		return;
	}

	if (!SendClientCutText(self->client, (char*)text, (int)len))
		fprintf(stderr, "SendClientCutText failed\n");
}

void vnc_client_set_pointer_collection(struct vnc_client* self,
		struct pointer_collection* pointers)
{
	self->pointer_collection = pointers;
}

void vnc_client_set_use_remote_cursor(struct vnc_client* self, bool enable)
{
	self->client->appData.useRemoteCursor = enable ? TRUE : FALSE;
}
