/* protocol/PROTOCOL.md v3 구현. 모든 정수는 big-endian. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PCAM_MAGIC 0x5043414Du /* "PCAM" */
#define PCAM_VERSION 3
#define PCAM_DEFAULT_PORT 9610

#define PCAM_CODEC_H264 0x68323634u /* 'h264' */
#define PCAM_CODEC_H265 0x68323635u /* 'h265' */
#define PCAM_AUDIO_OPUS 0x6F707573u /* 'opus' */
#define PCAM_AUDIO_PCM 0x70636D20u  /* 'pcm ': 16비트 little-endian, 채널을 섞어 배치 */

#define PCAM_STREAM_VIDEO 0
#define PCAM_STREAM_AUDIO 1
#define PCAM_STREAM_CONTROL 2
#define PCAM_FLAG_CONFIG 0x01
#define PCAM_FLAG_KEYFRAME 0x02
#define PCAM_HELLO_FLAG_CBR 0x01
#define PCAM_HELLO_FLAG_MIC 0x02

#define PCAM_HELLO_SIZE 28
#define PCAM_HANDSHAKE_SIZE 26
#define PCAM_HEADER_SIZE 16
#define PCAM_AUDIO_CONFIG_SIZE 12
#define PCAM_MAX_PACKET_SIZE (16 * 1024 * 1024)

struct pcam_hello {
	bool front;
	bool cbr;
	bool mic;
	uint32_t codec;
	uint32_t width;
	uint32_t height;
	uint16_t fps;
	uint32_t bitrate;
};

struct pcam_handshake {
	uint32_t video_codec;
	uint32_t audio_codec;
	uint32_t width;
	uint32_t height;
	uint16_t fps;
	uint16_t name_len;
};

struct pcam_packet_header {
	uint8_t stream_id;
	uint8_t flags;
	uint32_t size;
	int64_t pts_us;
};

/* 소리 CONFIG 패킷의 앞 12바이트. 그 뒤는 코덱 머리말 (Opus: OpusHead). */
struct pcam_audio_config {
	uint32_t codec;
	uint32_t sample_rate;
	uint8_t channels;
};

static inline void pcam_put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void pcam_put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline uint16_t pcam_get_u16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t pcam_get_u32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint64_t pcam_get_u64(const uint8_t *p)
{
	return ((uint64_t)pcam_get_u32(p) << 32) | pcam_get_u32(p + 4);
}

static inline void pcam_hello_encode(const struct pcam_hello *hello, uint8_t out[PCAM_HELLO_SIZE])
{
	memset(out, 0, PCAM_HELLO_SIZE);
	pcam_put_u32(out, PCAM_MAGIC);
	pcam_put_u16(out + 4, PCAM_VERSION);
	out[6] = hello->front ? 1 : 0;
	out[7] = (hello->cbr ? PCAM_HELLO_FLAG_CBR : 0) | (hello->mic ? PCAM_HELLO_FLAG_MIC : 0);
	pcam_put_u32(out + 8, hello->codec);
	pcam_put_u32(out + 12, hello->width);
	pcam_put_u32(out + 16, hello->height);
	pcam_put_u16(out + 20, hello->fps);
	pcam_put_u32(out + 24, hello->bitrate);
}

static inline bool pcam_handshake_decode(const uint8_t in[PCAM_HANDSHAKE_SIZE], struct pcam_handshake *hs)
{
	if (pcam_get_u32(in) != PCAM_MAGIC || pcam_get_u16(in + 4) != PCAM_VERSION)
		return false;
	hs->video_codec = pcam_get_u32(in + 6);
	hs->audio_codec = pcam_get_u32(in + 10);
	hs->width = pcam_get_u32(in + 14);
	hs->height = pcam_get_u32(in + 18);
	hs->fps = pcam_get_u16(in + 22);
	hs->name_len = pcam_get_u16(in + 24);
	return true;
}

static inline void pcam_header_decode(const uint8_t in[PCAM_HEADER_SIZE], struct pcam_packet_header *header)
{
	header->stream_id = in[0];
	header->flags = in[1];
	header->size = pcam_get_u32(in + 4);
	header->pts_us = (int64_t)pcam_get_u64(in + 8);
}

static inline bool pcam_audio_config_decode(const uint8_t *in, size_t size, struct pcam_audio_config *config)
{
	if (size < PCAM_AUDIO_CONFIG_SIZE)
		return false;
	config->codec = pcam_get_u32(in);
	config->sample_rate = pcam_get_u32(in + 4);
	config->channels = in[8];
	return true;
}

static inline void pcam_header_encode(uint8_t stream_id, uint32_t size, uint8_t out[PCAM_HEADER_SIZE])
{
	memset(out, 0, PCAM_HEADER_SIZE);
	out[0] = stream_id;
	pcam_put_u32(out + 4, size);
}
