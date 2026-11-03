#include <limits.h>

#include <obs-module.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>

#include "decoder.h"
#include "protocol.h"

struct pcam_decoder {
	AVCodecContext *ctx;
	AVPacket *packet;
	AVFrame *frame;
	AVFrame *sw_frame;
	AVBufferRef *hw_device;
	enum AVPixelFormat hw_format;
	bool hw_active;
	uint8_t *config;
	size_t config_size;
};

/* 디코더가 고를 수 있는 형식 중 GPU 형식이 있으면 그것을, 없으면 CPU 형식을 쓴다. */
static enum AVPixelFormat choose_format(AVCodecContext *ctx, const enum AVPixelFormat *formats)
{
	struct pcam_decoder *decoder = ctx->opaque;
	for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == decoder->hw_format) {
			decoder->hw_active = true;
			return *p;
		}
	}
	blog(LOG_WARNING, "[phonecam] hardware decoding not available for this stream, using CPU");
	decoder->hw_active = false;
	return avcodec_default_get_format(ctx, formats);
}

static void try_enable_hw(struct pcam_decoder *decoder)
{
#ifdef _WIN32
	if (av_hwdevice_ctx_create(&decoder->hw_device, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0) < 0) {
		blog(LOG_WARNING, "[phonecam] failed to create D3D11VA device, using CPU decoding");
		return;
	}
	decoder->hw_format = AV_PIX_FMT_D3D11;
	decoder->ctx->hw_device_ctx = av_buffer_ref(decoder->hw_device);
	decoder->ctx->opaque = decoder;
	decoder->ctx->get_format = choose_format;
#else
	UNUSED_PARAMETER(decoder);
#endif
}

struct pcam_decoder *pcam_decoder_create(uint32_t codec, bool hw)
{
	enum AVCodecID id = codec == PCAM_CODEC_H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
	const AVCodec *av_codec = avcodec_find_decoder(id);
	if (!av_codec) {
		blog(LOG_ERROR, "[phonecam] no FFmpeg decoder for %s", avcodec_get_name(id));
		return NULL;
	}

	struct pcam_decoder *decoder = bzalloc(sizeof(*decoder));
	decoder->hw_format = AV_PIX_FMT_NONE;
	decoder->ctx = avcodec_alloc_context3(av_codec);
	decoder->packet = av_packet_alloc();
	decoder->frame = av_frame_alloc();
	decoder->sw_frame = av_frame_alloc();
	if (!decoder->ctx || !decoder->packet || !decoder->frame || !decoder->sw_frame)
		goto fail;

	/* 프레임을 모아 두지 않고 들어오는 즉시 내보내게 한다. 폰 인코더는 B-프레임을 쓰지 않는다. */
	decoder->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	if (hw)
		try_enable_hw(decoder);
	/* GPU 디코딩에서는 스레드가 필요 없고, CPU 디코딩은 슬라이스 단위로만 나눈다 (프레임 단위는 지연이 늘어남). */
	decoder->ctx->thread_type = FF_THREAD_SLICE;
	decoder->ctx->thread_count = decoder->hw_device ? 1 : 0;

	if (avcodec_open2(decoder->ctx, av_codec, NULL) < 0) {
		blog(LOG_ERROR, "[phonecam] failed to open %s decoder", av_codec->name);
		goto fail;
	}
	blog(LOG_INFO, "[phonecam] using %s decoder%s", av_codec->name, decoder->hw_device ? " (D3D11VA)" : "");
	return decoder;

fail:
	pcam_decoder_destroy(decoder);
	return NULL;
}

void pcam_decoder_destroy(struct pcam_decoder *decoder)
{
	if (!decoder)
		return;
	avcodec_free_context(&decoder->ctx);
	av_buffer_unref(&decoder->hw_device);
	av_packet_free(&decoder->packet);
	av_frame_free(&decoder->frame);
	av_frame_free(&decoder->sw_frame);
	bfree(decoder->config);
	bfree(decoder);
}

bool pcam_decoder_is_hw(const struct pcam_decoder *decoder)
{
	return decoder && decoder->hw_active;
}

void pcam_decoder_decode(struct pcam_decoder *decoder, const uint8_t *data, size_t size, int64_t pts_us,
			 bool is_config, pcam_frame_cb cb, void *param)
{
	if (is_config) {
		decoder->config = brealloc(decoder->config, size);
		memcpy(decoder->config, data, size);
		decoder->config_size = size;
		return;
	}

	size_t total = decoder->config_size + size;
	if (total > INT_MAX || av_new_packet(decoder->packet, (int)total) < 0)
		return;
	if (decoder->config_size) {
		memcpy(decoder->packet->data, decoder->config, decoder->config_size);
		decoder->config_size = 0;
	}
	memcpy(decoder->packet->data + (total - size), data, size);
	decoder->packet->pts = pts_us;

	int ret = avcodec_send_packet(decoder->ctx, decoder->packet);
	av_packet_unref(decoder->packet);
	if (ret < 0 && ret != AVERROR(EAGAIN))
		return; /* 깨진 패킷은 건너뛰고 다음 패킷부터 다시 시도한다. */

	while (avcodec_receive_frame(decoder->ctx, decoder->frame) == 0) {
		const AVFrame *out = decoder->frame;
		if (decoder->frame->format == decoder->hw_format) {
			/* GPU에서 푼 프레임을 OBS에 넘기려면 CPU 메모리(NV12/P010)로 옮겨야 한다. */
			if (av_hwframe_transfer_data(decoder->sw_frame, decoder->frame, 0) < 0) {
				av_frame_unref(decoder->frame);
				continue;
			}
			av_frame_copy_props(decoder->sw_frame, decoder->frame);
			out = decoder->sw_frame;
		}
		cb(param, out);
		av_frame_unref(decoder->frame);
		av_frame_unref(decoder->sw_frame);
	}
}
