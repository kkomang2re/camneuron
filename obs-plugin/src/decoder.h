/* FFmpeg으로 H.264/HEVC Annex-B 스트림을 푼다. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct AVFrame;
struct pcam_decoder;

typedef void (*pcam_frame_cb)(void *param, const struct AVFrame *frame);

/* codec은 PCAM_CODEC_H264 또는 PCAM_CODEC_H265. hw면 GPU 디코딩을 시도하고, 안 되면 CPU로 푼다. */
struct pcam_decoder *pcam_decoder_create(uint32_t codec, bool hw);
void pcam_decoder_destroy(struct pcam_decoder *decoder);
bool pcam_decoder_is_hw(const struct pcam_decoder *decoder);

/* CONFIG 패킷(SPS/PPS/VPS)은 보관했다가 다음 패킷 앞에 붙인다. 풀린 프레임마다 cb를 부른다 (항상 CPU 메모리 프레임). */
void pcam_decoder_decode(struct pcam_decoder *decoder, const uint8_t *data, size_t size, int64_t pts_us,
			 bool is_config, pcam_frame_cb cb, void *param);
