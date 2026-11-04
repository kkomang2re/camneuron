/*
 * 방송 모니터: OBS 송출 화면을 작게 압축하고 고른 오디오 트랙과 함께 폰으로 보낸다 (protocol/PROTOCOL.md 9.3).
 * OBS 출력(output)을 하나 더 만들어 방송·녹화와 같은 화면과 소리를 받는다.
 */
#pragma once

#include <obs-module.h>

typedef void (*pcam_monitor_packet_fn)(void *param, uint8_t stream_id, uint8_t flags, int64_t pts_us,
				       const uint8_t *data, size_t size);

struct pcam_monitor_info {
	uint32_t width;
	uint32_t height;
	uint32_t fps;
	char encoder[64];
	bool audio;
};

struct pcam_monitor;

extern struct obs_output_info pcam_monitor_output_info;

/*
 * [track]은 보낼 OBS 오디오 트랙 1~6, 0이면 소리 없이. [fn]은 인코더 스레드에서 불리므로 기다리지 말아야 한다.
 * 실패하면 NULL을 돌려주고 [error]에 이유를 적는다.
 */
struct pcam_monitor *pcam_monitor_start(int track, int kbps, pcam_monitor_packet_fn fn, void *param,
					struct pcam_monitor_info *info, char *error, size_t error_size);

void pcam_monitor_stop(struct pcam_monitor *m);

/* Wi-Fi가 약할 때 폰이 낮춰 달라고 하면 끊지 않고 비트레이트만 바꾼다. */
void pcam_monitor_set_bitrate(struct pcam_monitor *m, int kbps);
