/*
 * Copyright (c) 2020 Andri Yngvason
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
#include <stdint.h>
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <linux/input-event-codes.h>

#include "pointer.h"
#include "shm.h"

#define STEP_SIZE 15.0

extern struct wl_shm* wl_shm;
extern struct wl_compositor* wl_compositor;

static struct wl_cursor_theme* pointer_load_cursor_theme(void)
{
	const char* xcursor_theme = getenv("XCURSOR_THEME");
	const char* xcursor_size_str = getenv("XCURSOR_SIZE");

	unsigned long xcursor_size = 24;
	if (xcursor_size_str) {
		char* end;
		unsigned long size = strtoul(xcursor_size_str, &end, 10);
		if (!*end && *xcursor_size_str)
			xcursor_size = size;
	}

	return wl_cursor_theme_load(xcursor_theme, xcursor_size, wl_shm);
}

struct pointer* pointer_new(struct wl_pointer* wl_pointer,
		struct pointer_collection* collection, struct seat* seat)
{
	struct pointer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->seat = seat;
	self->collection = collection;
	self->wl_pointer = wl_pointer;
	self->handle_event = false;

	if (collection->cursor_type == POINTER_CURSOR_LEFT_PTR)
		self->cursor_theme = pointer_load_cursor_theme();

	self->cursor_surface = wl_compositor_create_surface(wl_compositor);

	return self;
}

void pointer_destroy(struct pointer* self)
{
	wl_pointer_destroy(self->wl_pointer);
	if (self->cursor_theme)
		wl_cursor_theme_destroy(self->cursor_theme);
	wl_surface_destroy(self->cursor_surface);
	free(self);
}

struct pointer_collection* pointer_collection_new(
		enum pointer_cursor_type cursor_type)
{
	struct pointer_collection* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	wl_list_init(&self->pointers);
	self->cursor_type = cursor_type;

	return self;
}

static void pointer_collection_destroy_remote_cursor(
		struct pointer_collection* self)
{
	if (self->remote_buffer)
		wl_buffer_destroy(self->remote_buffer);
	if (self->remote_pixels)
		munmap(self->remote_pixels, self->remote_size);

	self->remote_buffer = NULL;
	self->remote_pixels = NULL;
	self->remote_size = 0;
	self->remote_cursor_active = false;
}

void pointer_collection_destroy(struct pointer_collection* self)
{
	struct pointer* pointer;
	struct pointer* tmp;

	wl_list_for_each_safe(pointer, tmp, &self->pointers, link) {
		wl_list_remove(&pointer->link);
		pointer_destroy(pointer);
	}

	pointer_collection_destroy_remote_cursor(self);

	free(self);
}

struct pointer* pointer_collection_find_wl_pointer(
		struct pointer_collection* self, struct wl_pointer* wl_pointer)
{
	struct pointer* pointer;
	wl_list_for_each(pointer, &self->pointers, link)
		if (pointer->wl_pointer == wl_pointer)
			return pointer;

	return NULL;
}

static void pointer_update_cursor_none(struct pointer* self)
{
	wl_surface_attach(self->cursor_surface, NULL, 0, 0);
	wl_pointer_set_cursor(self->wl_pointer, self->serial,
			self->cursor_surface, 0, 0);
	wl_surface_commit(self->cursor_surface);
}

static void pointer_update_cursor_left_ptr(struct pointer* self)
{
	if (!self->cursor_theme)
		return;

	struct wl_cursor* cursor = wl_cursor_theme_get_cursor(
			self->cursor_theme, "left_ptr");
	if (!cursor || cursor->image_count <= 0)
		return;

	struct wl_cursor_image* image = cursor->images[0];

	// TODO Set buffer scale
	wl_surface_attach(self->cursor_surface,
			wl_cursor_image_get_buffer(image), 0, 0);
	wl_pointer_set_cursor(self->wl_pointer, self->serial,
			self->cursor_surface, image->hotspot_x,
			image->hotspot_y);
	wl_surface_damage_buffer(self->cursor_surface, 0, 0, image->width,
			image->height);
	wl_surface_commit(self->cursor_surface);
}

static void pointer_update_cursor_remote(struct pointer* self)
{
	struct pointer_collection* collection = self->collection;

	assert(collection->remote_cursor_active);
	assert(collection->remote_buffer);

	wl_surface_attach(self->cursor_surface, collection->remote_buffer, 0,
			0);
	wl_pointer_set_cursor(self->wl_pointer, self->serial,
			self->cursor_surface, collection->remote_hotspot_x,
			collection->remote_hotspot_y);
	wl_surface_damage_buffer(self->cursor_surface, 0, 0,
			collection->remote_width, collection->remote_height);
	wl_surface_commit(self->cursor_surface);
}

static void pointer_update_cursor(struct pointer* self)
{
	struct pointer_collection* collection = self->collection;

	if (collection->cursor_type == POINTER_CURSOR_NONE) {
		pointer_update_cursor_none(self);
		return;
	}

	if (collection->remote_cursor_active) {
		pointer_update_cursor_remote(self);
		return;
	}

	if (collection->remote_cursor_received) {
		pointer_update_cursor_none(self);
		return;
	}

	pointer_update_cursor_left_ptr(self);
}

static void pointer_collection_refresh_cursors(struct pointer_collection* self)
{
	struct pointer* pointer;

	wl_list_for_each(pointer, &self->pointers, link) {
		if (!pointer->handle_event)
			continue;
		pointer_update_cursor(pointer);
	}
}

int pointer_collection_update_remote_cursor(struct pointer_collection* self,
		const uint32_t* argb, int width, int height, int hotspot_x,
		int hotspot_y)
{
	size_t size = (size_t)width * height * 4;
	int stride = width * 4;

	if (width <= 0 || height <= 0 || !argb)
		return -1;

	if (!self->remote_pixels || self->remote_size != size) {
		pointer_collection_destroy_remote_cursor(self);

		int fd = shm_alloc_fd(size);
		if (fd < 0)
			return -1;

		void* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
				fd, 0);
		if (pixels == MAP_FAILED) {
			close(fd);
			return -1;
		}

		struct wl_shm_pool* pool = wl_shm_create_pool(wl_shm, fd, size);
		if (!pool) {
			munmap(pixels, size);
			close(fd);
			return -1;
		}

		struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0,
				width, height, stride, WL_SHM_FORMAT_ARGB8888);
		wl_shm_pool_destroy(pool);
		close(fd);

		if (!buffer) {
			munmap(pixels, size);
			return -1;
		}

		self->remote_pixels = pixels;
		self->remote_size = size;
		self->remote_buffer = buffer;
	}

	memcpy(self->remote_pixels, argb, size);
	self->remote_width = width;
	self->remote_height = height;
	self->remote_hotspot_x = hotspot_x;
	self->remote_hotspot_y = hotspot_y;
	self->remote_cursor_active = true;
	self->remote_cursor_received = true;

	pointer_collection_refresh_cursors(self);

	return 0;
}

void pointer_collection_clear_remote_cursor(struct pointer_collection* self)
{
	if (!self->remote_cursor_active && !self->remote_cursor_received)
		return;

	self->remote_cursor_active = false;
	self->remote_cursor_received = true;
	pointer_collection_refresh_cursors(self);
}

static void pointer_enter(void* data, struct wl_pointer* wl_pointer,
		uint32_t serial, struct wl_surface* surface, wl_fixed_t x,
		wl_fixed_t y)
{
	struct pointer_collection* self = data;
	struct pointer* pointer =
		pointer_collection_find_wl_pointer(self, wl_pointer);
	assert(pointer);

	pointer->handle_event = self->handle_event(surface);
	if (!pointer->handle_event)
		return;

	pointer->serial = serial;
	pointer_update_cursor(pointer);
}

static void pointer_leave(void* data, struct wl_pointer* wl_pointer,
		uint32_t serial, struct wl_surface* surface)
{
	struct pointer_collection* self = data;
	struct pointer* pointer =
		pointer_collection_find_wl_pointer(self, wl_pointer);
	assert(pointer);

	pointer->handle_event = self->handle_event(surface);
	if (!pointer->handle_event)
		return;

	pointer->serial = serial;
}

static void pointer_motion(void* data, struct wl_pointer* wl_pointer,
		uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	struct pointer_collection* self = data;
	struct pointer* pointer =
		pointer_collection_find_wl_pointer(self, wl_pointer);
	assert(pointer);

	if (!pointer->handle_event)
		return;

	pointer->x = x;
	pointer->y = y;
}

static void pointer_set_button_state(struct pointer* self,
		enum pointer_button_mask button,
		enum wl_pointer_button_state state)
{
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		self->pressed |= button;
	} else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		self->pressed &= ~button;
	} else {
		abort();
	}
}

static void pointer_button(void* data, struct wl_pointer* wl_pointer,
		uint32_t serial, uint32_t t, uint32_t button,
		enum wl_pointer_button_state state)
{
	struct pointer_collection* self = data;
	struct pointer* pointer =
		pointer_collection_find_wl_pointer(self, wl_pointer);
	assert(pointer);

	if (!pointer->handle_event)
		return;

	pointer->serial = serial;
	pointer_update_cursor(pointer);

	switch (button) {
	case BTN_LEFT:
		pointer_set_button_state(pointer, POINTER_BUTTON_LEFT, state);
		break;
	case BTN_RIGHT:
		pointer_set_button_state(pointer, POINTER_BUTTON_RIGHT, state);
		break;
	case BTN_MIDDLE:
		pointer_set_button_state(pointer, POINTER_BUTTON_MIDDLE, state);
		break;
	// TODO: More buttons
	}
}

static void pointer_axis(void* data, struct wl_pointer* wl_pointer, uint32_t t,
		enum wl_pointer_axis axis, wl_fixed_t value)
{
	struct pointer_collection* self = data;
	struct pointer* pointer =
		pointer_collection_find_wl_pointer(self, wl_pointer);

	if (!pointer->handle_event)
		return;

	switch (axis) {
	case WL_POINTER_AXIS_VERTICAL_SCROLL:
		pointer->vertical_axis_value += wl_fixed_to_double(value);
		break;
	case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
		pointer->horizontal_axis_value += wl_fixed_to_double(value);
		break;
	default:;
	}
}

static void pointer_axis_source(void* data, struct wl_pointer* wl_pointer,
		enum wl_pointer_axis_source source)
{
	// TODO
}

static void pointer_axis_stop(void* data, struct wl_pointer* wl_pointer,
		uint32_t t, enum wl_pointer_axis axis)
{
	// TODO
}

static void pointer_axis_discrete(void* data, struct wl_pointer* wl_pointer,
		enum wl_pointer_axis axis, int steps)
{
	// Deprecated
}

static void pointer_frame(void* data, struct wl_pointer* wl_pointer)
{
	struct pointer_collection* self = data;
	struct pointer* pointer =
		pointer_collection_find_wl_pointer(self, wl_pointer);

	if (!pointer->handle_event)
		return;

	double vertical_steps = trunc(pointer->vertical_axis_value / STEP_SIZE);
	pointer->vertical_axis_value -= vertical_steps * STEP_SIZE;

	double horizontal_steps = trunc(pointer->horizontal_axis_value / STEP_SIZE);
	pointer->horizontal_axis_value -= horizontal_steps * STEP_SIZE;

	pointer->vertical_scroll_steps += vertical_steps;
	pointer->horizontal_scroll_steps += horizontal_steps;

	self->on_frame(self, pointer);

	pointer->vertical_scroll_steps = 0;
	pointer->horizontal_scroll_steps = 0;
}

static struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
	.frame = pointer_frame,
};

int pointer_collection_add_wl_pointer(struct pointer_collection* self,
		struct wl_pointer* wl_pointer, struct seat* seat)
{
	struct pointer* pointer = pointer_new(wl_pointer, self, seat);
	if (!pointer)
		return -1;

	wl_list_insert(&self->pointers, &pointer->link);
	wl_pointer_add_listener(pointer->wl_pointer, &pointer_listener, self);
	return 0;
}

void pointer_collection_remove_wl_pointer(struct pointer_collection* self,
	struct wl_pointer* wl_pointer)
{
	struct pointer* pointer =
		pointer_collection_find_wl_pointer(self, wl_pointer);
	wl_list_remove(&pointer->link);
	pointer_destroy(pointer);
}
