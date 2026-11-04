#include <math.h>
#include <stdio.h>

#include "control.h"
#include "protocol.h"

const char *const pcam_focus_modes[3] = {"continuous", "auto", "manual"};
const char *const pcam_wb_modes[PCAM_WB_COUNT] = {"auto", "incandescent", "fluorescent", "daylight", "cloudy"};

/* 알려진 이름이면 그 상수 문자열을, 아니면 첫 번째(기본값)를 돌려준다. 구조체가 문자열을 소유하지 않게 하려고. */
static const char *canonical(const char *const *names, size_t count, const char *value)
{
	for (size_t i = 0; value && i < count; i++) {
		if (strcmp(names[i], value) == 0)
			return names[i];
	}
	return names[0];
}

static bool near_equal(double a, double b)
{
	return fabs(a - b) < 0.005;
}

void pcam_controls_from_settings(struct pcam_controls *c, obs_data_t *settings)
{
	unsigned int width = 1920, height = 1080;
	sscanf(obs_data_get_string(settings, S_RESOLUTION), "%ux%u", &width, &height);
	c->width = width;
	c->height = height;
	c->fps = (uint32_t)obs_data_get_int(settings, S_FPS);
	c->codec = obs_data_get_int(settings, S_CODEC) == PCAM_CODEC_H265 ? PCAM_CODEC_H265 : PCAM_CODEC_H264;
	c->cbr = strcmp(obs_data_get_string(settings, S_RATE_CONTROL), "vbr") != 0;
	c->front = obs_data_get_int(settings, S_FACING) == 1;
	c->zoom = obs_data_get_double(settings, S_ZOOM);
	c->focus_mode = canonical(pcam_focus_modes, 3, obs_data_get_string(settings, S_FOCUS_MODE));
	c->focus_distance = obs_data_get_double(settings, S_FOCUS_DISTANCE);
	c->ev = obs_data_get_double(settings, S_EV);
	c->ae_lock = obs_data_get_bool(settings, S_AE_LOCK);
	c->awb_lock = obs_data_get_bool(settings, S_AWB_LOCK);
	c->wb = canonical(pcam_wb_modes, PCAM_WB_COUNT, obs_data_get_string(settings, S_WB));
	c->torch = obs_data_get_bool(settings, S_TORCH);
	c->stabilization = obs_data_get_bool(settings, S_STABILIZATION);
	c->mic = obs_data_get_bool(settings, S_MIC);
	c->bitrate_mbps = (int)obs_data_get_int(settings, S_BITRATE);
	c->beauty_smooth = (int)obs_data_get_int(settings, S_BEAUTY_SMOOTH);
	c->beauty_bright = (int)obs_data_get_int(settings, S_BEAUTY_BRIGHT);
}

void pcam_controls_to_settings(const struct pcam_controls *c, obs_data_t *settings)
{
	char resolution[32];
	snprintf(resolution, sizeof(resolution), "%ux%u", c->width, c->height);
	obs_data_set_string(settings, S_RESOLUTION, resolution);
	obs_data_set_int(settings, S_FPS, c->fps);
	obs_data_set_int(settings, S_CODEC, c->codec);
	obs_data_set_string(settings, S_RATE_CONTROL, c->cbr ? "cbr" : "vbr");
	obs_data_set_int(settings, S_FACING, c->front ? 1 : 0);
	obs_data_set_double(settings, S_ZOOM, c->zoom);
	obs_data_set_string(settings, S_FOCUS_MODE, c->focus_mode);
	obs_data_set_double(settings, S_FOCUS_DISTANCE, c->focus_distance);
	obs_data_set_double(settings, S_EV, c->ev);
	obs_data_set_bool(settings, S_AE_LOCK, c->ae_lock);
	obs_data_set_bool(settings, S_AWB_LOCK, c->awb_lock);
	obs_data_set_string(settings, S_WB, c->wb);
	obs_data_set_bool(settings, S_TORCH, c->torch);
	obs_data_set_bool(settings, S_STABILIZATION, c->stabilization);
	obs_data_set_bool(settings, S_MIC, c->mic);
	obs_data_set_int(settings, S_BITRATE, c->bitrate_mbps);
	if (c->beauty_smooth >= 0)
		obs_data_set_int(settings, S_BEAUTY_SMOOTH, c->beauty_smooth);
	if (c->beauty_bright >= 0)
		obs_data_set_int(settings, S_BEAUTY_BRIGHT, c->beauty_bright);
}

/* 폰 state에 있는 0~100 값, 없으면 -1 */
static int beauty_from_state(obs_data_t *state, const char *key)
{
	if (!obs_data_has_user_value(state, key))
		return -1;
	long long value = obs_data_get_int(state, key);
	return value < 0 ? 0 : value > 100 ? 100 : (int)value;
}

/* 한쪽이 모르면(-1) 같은 것으로 본다 */
static bool beauty_equal(int a, int b)
{
	return a < 0 || b < 0 || a == b;
}

void pcam_controls_from_state(struct pcam_controls *c, obs_data_t *state)
{
	c->width = (uint32_t)obs_data_get_int(state, "width");
	c->height = (uint32_t)obs_data_get_int(state, "height");
	c->fps = (uint32_t)obs_data_get_int(state, "fps");
	c->codec = strcmp(obs_data_get_string(state, "codec"), "h265") == 0 ? PCAM_CODEC_H265 : PCAM_CODEC_H264;
	c->cbr = strcmp(obs_data_get_string(state, "rate_control"), "vbr") != 0;
	c->front = strcmp(obs_data_get_string(state, "facing"), "front") == 0;
	c->zoom = obs_data_get_double(state, "zoom");
	c->focus_mode = canonical(pcam_focus_modes, 3, obs_data_get_string(state, "focus_mode"));
	c->focus_distance = obs_data_get_double(state, "focus_distance");
	c->ev = obs_data_get_double(state, "ev");
	c->ae_lock = obs_data_get_bool(state, "ae_lock");
	c->awb_lock = obs_data_get_bool(state, "awb_lock");
	c->wb = canonical(pcam_wb_modes, PCAM_WB_COUNT, obs_data_get_string(state, "wb"));
	c->torch = obs_data_get_bool(state, "torch");
	c->stabilization = obs_data_get_bool(state, "stabilization");
	c->mic = obs_data_get_bool(state, "mic");
	c->bitrate_mbps = (int)((obs_data_get_int(state, "bitrate") + 500000) / 1000000);
	c->beauty_smooth = beauty_from_state(state, "beauty_smooth");
	c->beauty_bright = beauty_from_state(state, "beauty_bright");
}

bool pcam_controls_equal(const struct pcam_controls *a, const struct pcam_controls *b)
{
	return a->width == b->width && a->height == b->height && a->fps == b->fps && a->codec == b->codec &&
	       a->cbr == b->cbr && a->front == b->front && near_equal(a->zoom, b->zoom) &&
	       a->focus_mode == b->focus_mode &&
	       near_equal(a->focus_distance, b->focus_distance) && near_equal(a->ev, b->ev) &&
	       a->ae_lock == b->ae_lock && a->awb_lock == b->awb_lock && a->wb == b->wb && a->torch == b->torch &&
	       a->stabilization == b->stabilization && a->mic == b->mic && a->bitrate_mbps == b->bitrate_mbps &&
	       beauty_equal(a->beauty_smooth, b->beauty_smooth) && beauty_equal(a->beauty_bright, b->beauty_bright);
}

bool pcam_controls_diff(const struct pcam_controls *from, const struct pcam_controls *to, obs_data_t *msg,
			bool include_format)
{
	bool any = false;

	if (include_format) {
		if (from->width != to->width || from->height != to->height) {
			obs_data_set_int(msg, "width", to->width);
			obs_data_set_int(msg, "height", to->height);
			any = true;
		}
		if (from->fps != to->fps) {
			obs_data_set_int(msg, "fps", to->fps);
			any = true;
		}
		if (from->codec != to->codec) {
			obs_data_set_string(msg, "codec", to->codec == PCAM_CODEC_H265 ? "h265" : "h264");
			any = true;
		}
		if (from->cbr != to->cbr) {
			obs_data_set_string(msg, "rate_control", to->cbr ? "cbr" : "vbr");
			any = true;
		}
		if (from->bitrate_mbps != to->bitrate_mbps) {
			obs_data_set_int(msg, "bitrate", (long long)to->bitrate_mbps * 1000000);
			any = true;
		}
	}

	if (from->front != to->front) {
		obs_data_set_string(msg, "facing", to->front ? "front" : "back");
		any = true;
	}
	if (!near_equal(from->zoom, to->zoom)) {
		obs_data_set_double(msg, "zoom", to->zoom);
		any = true;
	}
	if (from->focus_mode != to->focus_mode) {
		obs_data_set_string(msg, "focus_mode", to->focus_mode);
		any = true;
	}
	if (!near_equal(from->focus_distance, to->focus_distance)) {
		obs_data_set_double(msg, "focus_distance", to->focus_distance);
		any = true;
	}
	if (!near_equal(from->ev, to->ev)) {
		obs_data_set_double(msg, "ev", to->ev);
		any = true;
	}
	if (from->ae_lock != to->ae_lock) {
		obs_data_set_bool(msg, "ae_lock", to->ae_lock);
		any = true;
	}
	if (from->awb_lock != to->awb_lock) {
		obs_data_set_bool(msg, "awb_lock", to->awb_lock);
		any = true;
	}
	if (from->wb != to->wb) {
		obs_data_set_string(msg, "wb", to->wb);
		any = true;
	}
	if (from->torch != to->torch) {
		obs_data_set_bool(msg, "torch", to->torch);
		any = true;
	}
	if (from->stabilization != to->stabilization) {
		obs_data_set_bool(msg, "stabilization", to->stabilization);
		any = true;
	}
	if (from->mic != to->mic) {
		obs_data_set_bool(msg, "mic", to->mic);
		any = true;
	}
	/* 예전 폰 앱(state에 없음)에는 보내지 않는다 */
	if (from->beauty_smooth >= 0 && to->beauty_smooth >= 0 && from->beauty_smooth != to->beauty_smooth) {
		obs_data_set_int(msg, "beauty_smooth", to->beauty_smooth);
		any = true;
	}
	if (from->beauty_bright >= 0 && to->beauty_bright >= 0 && from->beauty_bright != to->beauty_bright) {
		obs_data_set_int(msg, "beauty_bright", to->beauty_bright);
		any = true;
	}
	return any;
}

void pcam_caps_from_json(struct pcam_caps *caps, obs_data_t *msg)
{
	caps->valid = true;
	caps->front = strcmp(obs_data_get_string(msg, "facing"), "front") == 0;
	caps->has_front = obs_data_get_bool(msg, "has_front");
	caps->has_back = obs_data_get_bool(msg, "has_back");
	caps->zoom_min = obs_data_get_double(msg, "zoom_min");
	caps->zoom_max = obs_data_get_double(msg, "zoom_max");
	caps->ev_min = obs_data_get_double(msg, "ev_min");
	caps->ev_max = obs_data_get_double(msg, "ev_max");
	caps->ev_step = obs_data_get_double(msg, "ev_step");
	caps->manual_focus = obs_data_get_bool(msg, "manual_focus");
	caps->torch = obs_data_get_bool(msg, "torch");
	caps->stabilization = obs_data_get_bool(msg, "stabilization");
	snprintf(caps->wb_modes, sizeof(caps->wb_modes), "%s", obs_data_get_string(msg, "wb_modes"));

	if (caps->zoom_max < caps->zoom_min)
		caps->zoom_max = caps->zoom_min;
	if (caps->ev_step <= 0.0)
		caps->ev_step = 0.1;
}

void pcam_stats_from_json(struct pcam_stats *stats, obs_data_t *msg)
{
	stats->valid = true;
	stats->fps = obs_data_get_double(msg, "fps");
	stats->bitrate = obs_data_get_double(msg, "bitrate");
	stats->dropped = (int)obs_data_get_int(msg, "dropped");
	stats->encode_ms = obs_data_get_double(msg, "encode_ms");
	stats->battery = (int)obs_data_get_int(msg, "battery");
	stats->charging = obs_data_has_user_value(msg, "charging") ? (obs_data_get_bool(msg, "charging") ? 1 : 0) : -1;
	stats->temp = obs_data_get_double(msg, "temp");
	stats->thermal = (int)obs_data_get_int(msg, "thermal");
}
