#include "wlr-data-control-unstable-v1.h"
#include "seat.h"

typedef void (*vnc_write_clipboard_t)(char* text, size_t size);

struct data_control {
	struct wl_display* wl_display;
	struct nvnc* server;
	struct zwlr_data_control_manager_v1* manager;
	struct zwlr_data_control_device_v1* device;
	struct zwlr_data_control_source_v1* selection;
	struct zwlr_data_control_source_v1* primary_selection;
	struct zwlr_data_control_offer_v1* offer;
	const char* mime_type;
	char* cb_data;
	size_t cb_len;
	vnc_write_clipboard_t vnc_write_clipboard;
};

void data_control_init(struct data_control* self, struct seat* seat, struct zwlr_data_control_manager_v1 *manager);
void data_control_to_clipboard(struct data_control* self, const char* text, size_t len);
void data_control_destroy(struct data_control* self);

#pragma once