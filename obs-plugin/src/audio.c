#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <obs-module.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#include "audio.h"
#include "protocol.h"

struct pcam_audio {
	struct pcam_audio_config config;
	AVCodecContext *ctx; /* Opus일 때만 */
	AVPacket *packet;
	AVFrame *frame;
	float peak;
};

static enum speaker_layout speakers_for(int channels)
{
	return channels == 1 ? SPEAKERS_MONO : channels == 2 ? SPEAKERS_STEREO : SPEAKERS_UNKNOWN;
}

static enum audio_format obs_format_for(int format)
{
	switch (format) {
	case AV_SAMPLE_FMT_U8:
		return AUDIO_FORMAT_U8BIT;
	case AV_SAMPLE_FMT_S16:
		return AUDIO_FORMAT_16BIT;
	case AV_SAMPLE_FMT_S32:
		return AUDIO_FORMAT_32BIT;
	case AV_SAMPLE_FMT_FLT:
		return AUDIO_FORMAT_FLOAT;
	case AV_SAMPLE_FMT_U8P:
		return AUDIO_FORMAT_U8BIT_PLANAR;
	case AV_SAMPLE_FMT_S16P:
		return AUDIO_FORMAT_16BIT_PLANAR;
	case AV_SAMPLE_FMT_S32P:
		return AUDIO_FORMAT_32BIT_PLANAR;
	case AV_SAMPLE_FMT_FLTP:
		return AUDIO_FORMAT_FLOAT_PLANAR;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

static float peak_s16(const uint8_t *data, size_t samples)
{
	const int16_t *s = (const int16_t *)data;
	int max = 0;
	for (size_t i = 0; i < samples; i++) {
		int v = abs(s[i]);
		if (v > max)
			max = v;
	}
	return max / 32768.0f;
}

static float peak_float(const float *s, size_t samples)
{
	float max = 0.0f;
	for (size_t i = 0; i < samples; i++)
		max = fmaxf(max, fabsf(s[i]));
	return max;
}

static void track_peak(struct pcam_audio *audio, const AVFrame *frame)
{
	size_t samples = (size_t)frame->nb_samples;
	int channels = frame->ch_layout.nb_channels;
	float peak = 0.0f;
	switch (frame->format) {
	case AV_SAMPLE_FMT_FLTP:
		for (int c = 0; c < channels; c++)
			peak = fmaxf(peak, peak_float((const float *)frame->extended_data[c], samples));
		break;
	case AV_SAMPLE_FMT_FLT:
		peak = peak_float((const float *)frame->data[0], samples * channels);
		break;
	case AV_SAMPLE_FMT_S16:
		peak = peak_s16(frame->data[0], samples * channels);
		break;
	default:
		return;
	}
	audio->peak = fmaxf(audio->peak, peak);
}

static bool open_opus(struct pcam_audio *audio, const uint8_t *header, size_t header_size)
{
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	if (!codec) {
		blog(LOG_ERROR, "[phonecam] no FFmpeg decoder for Opus");
		return false;
	}
	audio->ctx = avcodec_alloc_context3(codec);
	audio->packet = av_packet_alloc();
	audio->frame = av_frame_alloc();
	if (!audio->ctx || !audio->packet || !audio->frame)
		return false;

	audio->ctx->sample_rate = (int)audio->config.sample_rate;
	/* pre-skip을 버릴 때 FFmpeg가 시각을 고치려고 쓴다 (없으면 경고를 남김). 시각은 직접 계산하므로 값은 상관없다. */
	audio->ctx->pkt_timebase = (AVRational){1, 1000000};
	av_channel_layout_default(&audio->ctx->ch_layout, audio->config.channels);
	/* OpusHead: 채널 수와 앞부분 지연(pre-skip). 디코더가 그만큼을 버려 첫 소리가 제자리에 온다. */
	if (header_size) {
		audio->ctx->extradata = av_mallocz(header_size + AV_INPUT_BUFFER_PADDING_SIZE);
		if (!audio->ctx->extradata)
			return false;
		memcpy(audio->ctx->extradata, header, header_size);
		audio->ctx->extradata_size = (int)header_size;
	}
	if (avcodec_open2(audio->ctx, codec, NULL) < 0) {
		blog(LOG_ERROR, "[phonecam] failed to open %s decoder", codec->name);
		return false;
	}
	return true;
}

struct pcam_audio *pcam_audio_create(const uint8_t *data, size_t size)
{
	struct pcam_audio_config config;
	if (!pcam_audio_config_decode(data, size, &config) || config.channels < 1 || config.channels > 2 ||
	    config.sample_rate < 8000 || config.sample_rate > 192000) {
		blog(LOG_WARNING, "[phonecam] unsupported audio format from the phone");
		return NULL;
	}
	if (config.codec != PCAM_AUDIO_OPUS && config.codec != PCAM_AUDIO_PCM) {
		blog(LOG_WARNING, "[phonecam] unknown audio codec %08x: update the plugin", config.codec);
		return NULL;
	}

	struct pcam_audio *audio = bzalloc(sizeof(*audio));
	audio->config = config;
	if (config.codec == PCAM_AUDIO_OPUS &&
	    !open_opus(audio, data + PCAM_AUDIO_CONFIG_SIZE, size - PCAM_AUDIO_CONFIG_SIZE)) {
		pcam_audio_destroy(audio);
		return NULL;
	}

	char desc[32];
	pcam_audio_describe(audio, desc, sizeof(desc), NULL);
	blog(LOG_INFO, "[phonecam] audio: %s, %u channel(s)", desc, config.channels);
	return audio;
}

void pcam_audio_destroy(struct pcam_audio *audio)
{
	if (!audio)
		return;
	avcodec_free_context(&audio->ctx);
	av_packet_free(&audio->packet);
	av_frame_free(&audio->frame);
	bfree(audio);
}

void pcam_audio_describe(const struct pcam_audio *audio, char *buf, size_t size, int *channels)
{
	snprintf(buf, size, "%s %u kHz", audio->config.codec == PCAM_AUDIO_OPUS ? "Opus" : "PCM",
		 audio->config.sample_rate / 1000);
	if (channels)
		*channels = audio->config.channels;
}

float pcam_audio_take_peak(struct pcam_audio *audio)
{
	float peak = audio->peak;
	audio->peak = 0.0f;
	return peak;
}

static void output_pcm(struct pcam_audio *audio, const uint8_t *data, size_t size, uint64_t timestamp,
		       obs_source_t *source)
{
	size_t channels = audio->config.channels;
	struct obs_source_audio out = {0};
	out.data[0] = data;
	out.frames = (uint32_t)(size / (2 * channels));
	out.format = AUDIO_FORMAT_16BIT;
	out.speakers = speakers_for((int)channels);
	out.samples_per_sec = audio->config.sample_rate;
	out.timestamp = timestamp;
	if (!out.frames)
		return;
	audio->peak = fmaxf(audio->peak, peak_s16(data, out.frames * channels));
	obs_source_output_audio(source, &out);
}

void pcam_audio_output(struct pcam_audio *audio, const uint8_t *data, size_t size, uint64_t timestamp_ns,
		       obs_source_t *source)
{
	if (audio->config.codec == PCAM_AUDIO_PCM) {
		output_pcm(audio, data, size, timestamp_ns, source);
		return;
	}

	if (size > INT_MAX || av_new_packet(audio->packet, (int)size) < 0)
		return;
	memcpy(audio->packet->data, data, size);
	int ret = avcodec_send_packet(audio->ctx, audio->packet);
	av_packet_unref(audio->packet);
	if (ret < 0 && ret != AVERROR(EAGAIN))
		return; /* 깨진 패킷은 건너뛴다 */

	while (avcodec_receive_frame(audio->ctx, audio->frame) == 0) {
		AVFrame *frame = audio->frame;
		enum audio_format format = obs_format_for(frame->format);
		if (format != AUDIO_FORMAT_UNKNOWN && frame->nb_samples > 0 && frame->sample_rate > 0) {
			struct obs_source_audio out = {0};
			for (size_t i = 0; i < MAX_AV_PLANES; i++)
				out.data[i] = frame->data[i];
			out.frames = (uint32_t)frame->nb_samples;
			out.format = format;
			out.speakers = speakers_for(frame->ch_layout.nb_channels);
			out.samples_per_sec = (uint32_t)frame->sample_rate;
			out.timestamp = timestamp_ns;
			track_peak(audio, frame);
			obs_source_output_audio(source, &out);
			timestamp_ns += (uint64_t)frame->nb_samples * 1000000000ULL / (uint64_t)frame->sample_rate;
		}
		av_frame_unref(frame);
	}
}
