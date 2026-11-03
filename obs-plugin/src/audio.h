/* 폰 마이크 소리(Opus 또는 PCM)를 풀어 OBS 소스의 소리로 내보낸다. protocol/PROTOCOL.md 3.1. */
#pragma once

#include <obs-module.h>

struct pcam_audio;

/* 소리 CONFIG 패킷으로 디코더를 만든다. 모르는 코덱이거나 열지 못하면 NULL. */
struct pcam_audio *pcam_audio_create(const uint8_t *config, size_t size);
void pcam_audio_destroy(struct pcam_audio *audio);

/* 패킷 하나를 풀어 source의 소리로 내보낸다. timestamp_ns는 OBS 시계(os_gettime_ns)로 이 소리를 낼 시각. */
void pcam_audio_output(struct pcam_audio *audio, const uint8_t *data, size_t size, uint64_t timestamp_ns,
		       obs_source_t *source);

/* 상태 표시용. 예: "Opus 48 kHz", 2 */
void pcam_audio_describe(const struct pcam_audio *audio, char *buf, size_t size, int *channels);

/* 지난번 호출 뒤 가장 큰 음량 (0~1). 로그용. */
float pcam_audio_take_peak(struct pcam_audio *audio);
