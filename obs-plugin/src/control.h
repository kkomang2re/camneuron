/* 카메라·송출 설정 값과 제어 메시지(JSON) 변환. protocol/PROTOCOL.md 4장. */
#pragma once

#include <obs-module.h>

#define S_RESOLUTION "resolution"
#define S_FPS "fps"
#define S_CODEC "codec"
#define S_RATE_CONTROL "rate_control"
#define S_BITRATE "bitrate_mbps"
#define S_FACING "camera"
#define S_ZOOM "zoom"
#define S_FOCUS_MODE "focus_mode"
#define S_FOCUS_DISTANCE "focus_distance"
#define S_EV "ev"
#define S_AE_LOCK "ae_lock"
#define S_AWB_LOCK "awb_lock"
#define S_WB "wb"
#define S_TORCH "torch"
#define S_STABILIZATION "stabilization"
#define S_MIC "mic"
#define S_BEAUTY_SMOOTH "beauty_smooth"
#define S_BEAUTY_BRIGHT "beauty_bright"

#define PCAM_WB_COUNT 5

extern const char *const pcam_focus_modes[3]; /* continuous, auto, manual */
extern const char *const pcam_wb_modes[PCAM_WB_COUNT];

/*
 * OBS 설정과 폰 상태가 공유하는 값. 폰과 OBS 어느 쪽에서 바꿔도 이 값으로 맞춘다.
 * 송출 형식(해상도·fps·코덱·방식)도 연결을 끊지 않고 바뀐다.
 */
struct pcam_controls {
	uint32_t width;
	uint32_t height;
	uint32_t fps;
	uint32_t codec; /* PCAM_CODEC_H264 / PCAM_CODEC_H265 */
	bool cbr;
	int bitrate_mbps;
	bool front;
	double zoom;
	const char *focus_mode; /* pcam_focus_modes 중 하나 */
	double focus_distance;  /* 0 = 무한대, 1 = 가장 가까움 */
	double ev;
	bool ae_lock;
	bool awb_lock;
	const char *wb; /* pcam_wb_modes 중 하나 */
	bool torch;
	bool stabilization;
	bool mic; /* 폰 마이크 소리를 보냄 */
	/* 뷰티 보정 0~100 (프로). 폰 state에 없으면(예전 앱) -1: 그때는 OBS 값을 건드리지 않고 보내지도 않는다 */
	int beauty_smooth;
	int beauty_bright;
};

/* 지금 폰에서 열린 카메라가 지원하는 범위 */
struct pcam_caps {
	bool valid;
	bool front;
	bool has_front;
	bool has_back;
	double zoom_min;
	double zoom_max;
	double ev_min;
	double ev_max;
	double ev_step;
	bool manual_focus;
	bool torch;
	bool stabilization;
	char wb_modes[128];
};

struct pcam_stats {
	bool valid;
	double fps;
	double bitrate;
	int dropped;
	double encode_ms;
	int battery;
	double temp;
	int thermal;
};

void pcam_controls_from_settings(struct pcam_controls *c, obs_data_t *settings);
void pcam_controls_to_settings(const struct pcam_controls *c, obs_data_t *settings);
void pcam_controls_from_state(struct pcam_controls *c, obs_data_t *state);
bool pcam_controls_equal(const struct pcam_controls *a, const struct pcam_controls *b);

/*
 * from과 다른 값만 msg에 넣는다. 하나라도 넣었으면 true.
 * include_format이 false면 송출 형식(해상도·fps·코덱·방식·비트레이트)은 빼고 비교한다 (접속 직후 Hello로 이미 보냈으므로).
 */
bool pcam_controls_diff(const struct pcam_controls *from, const struct pcam_controls *to, obs_data_t *msg,
			bool include_format);

void pcam_caps_from_json(struct pcam_caps *caps, obs_data_t *msg);
void pcam_stats_from_json(struct pcam_stats *stats, obs_data_t *msg);
