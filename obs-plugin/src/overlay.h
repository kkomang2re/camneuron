/*
 * 폰에서 정한 채팅·후원 창(치지직 위젯 URL)을 OBS 브라우저 소스로 열어 캠 화면 위에 합성한다.
 * 캠뉴런 소스에 숨은 필터 하나를 달아, 카메라 영상을 그린 뒤 그 위에 브라우저 화면을 그린다.
 */
#pragma once

#include <obs-module.h>

#define PCAM_OVERLAY_FILTER_ID "phonecam_overlay_filter"
#define PCAM_URL_MAX 1024

struct pcam_overlay_item {
	char url[PCAM_URL_MAX];
	bool stream; /* 송출에 포함 */
	double x;    /* 위치·크기: 영상 기준 0~1 */
	double y;
	double w;
	double h;
	double zoom;
	double opacity;
};

struct pcam_overlays {
	bool valid;
	/* 폰 화면에서 영상이 그려지는 폭(CSS px). 같은 모양이 되도록 브라우저 크기를 이 기준으로 정한다. */
	double reference_width;
	struct pcam_overlay_item chat;
	struct pcam_overlay_item alert;
};

extern struct obs_source_info pcam_overlay_filter_info;

void pcam_overlays_from_json(struct pcam_overlays *overlays, obs_data_t *msg);
bool pcam_overlays_streamed(const struct pcam_overlays *overlays);

/* 필터의 브라우저 소스와 배치를 overlays에 맞춘다. UI 스레드에서 부른다. */
void pcam_overlay_filter_apply(obs_source_t *filter, const struct pcam_overlays *overlays, uint32_t stream_width,
			       uint32_t stream_height);
