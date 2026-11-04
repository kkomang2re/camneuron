#include "monitor.h"

#include <stdio.h>

#include <util/bmem.h>

#include "protocol.h"

/* 폰 화면에서 볼 만한 크기와 데이터량. 키프레임을 1초마다 넣어 Wi-Fi가 잠깐 막혀도 빨리 돌아온다. */
#define MONITOR_HEIGHT 540
#define MONITOR_FPS 30
#define MONITOR_VIDEO_KBPS 1500
#define MONITOR_AUDIO_KBPS 96

struct monitor_output {
	obs_output_t *output;
	pcam_monitor_packet_fn fn;
	void *param;
	bool audio;
};

struct pcam_monitor {
	obs_output_t *output;
	obs_encoder_t *video;
	obs_encoder_t *audio;
};

static const char *monitor_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "CamNeuron monitor";
}

static void *monitor_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	struct monitor_output *m = bzalloc(sizeof(*m));
	m->output = output;
	return m;
}

static void monitor_destroy(void *data)
{
	bfree(data);
}

static bool monitor_start_capture(void *data)
{
	struct monitor_output *m = data;
	uint32_t flags = m->audio ? 0 : OBS_OUTPUT_VIDEO;
	if (!obs_output_can_begin_data_capture(m->output, flags) || !obs_output_initialize_encoders(m->output, flags))
		return false;
	return obs_output_begin_data_capture(m->output, flags);
}

static void monitor_stop_capture(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	struct monitor_output *m = data;
	obs_output_end_data_capture(m->output);
}

static int64_t packet_pts_us(const struct encoder_packet *packet)
{
	if (packet->timebase_den <= 0)
		return 0;
	return packet->pts * 1000000LL * packet->timebase_num / packet->timebase_den;
}

/* 소리 CONFIG: 3.1장과 같은 12바이트 머리말 + OpusHead */
static void send_audio_config(struct monitor_output *m, int64_t pts_us)
{
	obs_encoder_t *audio = obs_output_get_audio_encoder(m->output, 0);
	uint8_t *extra = NULL;
	size_t extra_size = 0;
	if (!audio || !obs_encoder_get_extra_data(audio, &extra, &extra_size) || !extra_size)
		return;
	uint8_t config[PCAM_AUDIO_CONFIG_SIZE + 64];
	if (extra_size > sizeof(config) - PCAM_AUDIO_CONFIG_SIZE)
		return;
	memset(config, 0, PCAM_AUDIO_CONFIG_SIZE);
	pcam_put_u32(config, PCAM_AUDIO_OPUS);
	pcam_put_u32(config + 4, obs_encoder_get_sample_rate(audio));
	config[8] = (uint8_t)audio_output_get_channels(obs_get_audio());
	memcpy(config + PCAM_AUDIO_CONFIG_SIZE, extra, extra_size);
	m->fn(m->param, PCAM_STREAM_MONITOR_AUDIO, PCAM_FLAG_CONFIG, pts_us, config, PCAM_AUDIO_CONFIG_SIZE + extra_size);
}

static void monitor_packet(void *data, struct encoder_packet *packet)
{
	struct monitor_output *m = data;
	if (!packet)
		return;
	int64_t pts_us = packet_pts_us(packet);
	if (packet->type == OBS_ENCODER_VIDEO) {
		if (packet->keyframe) {
			/* 늦게 붙거나 밀려서 버린 뒤에도 바로 풀 수 있게 키프레임마다 SPS·PPS와 소리 머리말을 다시 보낸다. */
			uint8_t *extra = NULL;
			size_t extra_size = 0;
			obs_encoder_t *video = obs_output_get_video_encoder(m->output);
			if (video && obs_encoder_get_extra_data(video, &extra, &extra_size) && extra_size)
				m->fn(m->param, PCAM_STREAM_MONITOR_VIDEO, PCAM_FLAG_CONFIG, pts_us, extra, extra_size);
			if (m->audio)
				send_audio_config(m, pts_us);
		}
		m->fn(m->param, PCAM_STREAM_MONITOR_VIDEO, packet->keyframe ? PCAM_FLAG_KEYFRAME : 0, pts_us, packet->data,
		      packet->size);
	} else if (m->audio) {
		m->fn(m->param, PCAM_STREAM_MONITOR_AUDIO, 0, pts_us, packet->data, packet->size);
	}
}

struct obs_output_info pcam_monitor_output_info = {
	.id = "phonecam_monitor",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED,
	.get_name = monitor_get_name,
	.create = monitor_create,
	.destroy = monitor_destroy,
	.start = monitor_start_capture,
	.stop = monitor_stop_capture,
	.encoded_packet = monitor_packet,
};

/* 지연이 짧은 설정. 키 이름은 각 인코더 플러그인(OBS 33)의 설정 이름. */
static obs_encoder_t *create_video_encoder(const char *id, int kbps, uint32_t width, uint32_t height, uint32_t divisor)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "bitrate", kbps > 0 ? kbps : MONITOR_VIDEO_KBPS);
	obs_data_set_int(s, "keyint_sec", 1);
	obs_data_set_int(s, "bf", 0);
	obs_data_set_string(s, "profile", "main");
	if (strcmp(id, "obs_nvenc_h264_tex") == 0) {
		obs_data_set_string(s, "rate_control", "cbr");
		obs_data_set_string(s, "preset", "p3");
		obs_data_set_string(s, "tune", "ull");
		obs_data_set_string(s, "multipass", "disabled");
		obs_data_set_bool(s, "lookahead", false);
	} else if (strcmp(id, "h264_texture_amf") == 0) {
		obs_data_set_string(s, "rate_control", "CBR");
		obs_data_set_string(s, "preset", "speed");
	} else {
		obs_data_set_string(s, "rate_control", "CBR");
		obs_data_set_string(s, "preset", "veryfast");
		obs_data_set_string(s, "tune", "zerolatency");
	}
	obs_encoder_t *encoder = obs_video_encoder_create(id, "CamNeuron monitor video", s, NULL);
	obs_data_release(s);
	if (!encoder)
		return NULL;
	obs_encoder_set_scaled_size(encoder, width, height);
	/* GPU에서 줄인다: CPU 부담이 없고 NVENC·AMF는 텍스처(GPU) 경로를 그대로 쓴다. */
	obs_encoder_set_gpu_scale_type(encoder, OBS_SCALE_BILINEAR);
	if (divisor > 1)
		obs_encoder_set_frame_rate_divisor(encoder, divisor);
	obs_encoder_set_video(encoder, obs_get_video());
	return encoder;
}

static void release_monitor(struct pcam_monitor *m)
{
	if (m->output) {
		obs_output_stop(m->output);
		obs_output_release(m->output);
	}
	obs_encoder_release(m->video);
	obs_encoder_release(m->audio);
	bfree(m);
}

/* 하나의 인코더로 출력을 만들어 시작한다. 실패하면 모두 정리하고 NULL. */
static struct pcam_monitor *try_start(const char *encoder_id, int track, int kbps, pcam_monitor_packet_fn fn, void *param,
				      uint32_t width, uint32_t height, uint32_t divisor)
{
	struct pcam_monitor *m = bzalloc(sizeof(*m));
	m->video = create_video_encoder(encoder_id, kbps, width, height, divisor);
	if (!m->video) {
		bfree(m);
		return NULL;
	}
	m->output = obs_output_create("phonecam_monitor", "CamNeuron monitor", NULL, NULL);
	if (!m->output) {
		release_monitor(m);
		return NULL;
	}
	struct monitor_output *data = obs_obj_get_data(m->output);
	data->fn = fn;
	data->param = param;
	obs_output_set_video_encoder(m->output, m->video);

	if (track > 0) {
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "bitrate", MONITOR_AUDIO_KBPS);
		m->audio = obs_audio_encoder_create("ffmpeg_opus", "CamNeuron monitor audio", s, (size_t)(track - 1), NULL);
		obs_data_release(s);
		if (m->audio) {
			obs_encoder_set_audio(m->audio, obs_get_audio());
			obs_output_set_audio_encoder(m->output, m->audio, 0);
			data->audio = true;
		}
	}

	if (!obs_output_start(m->output)) {
		const char *why = obs_output_get_last_error(m->output);
		blog(LOG_WARNING, "[phonecam] monitor: %s did not start%s%s", encoder_id, why ? ": " : "", why ? why : "");
		release_monitor(m);
		return NULL;
	}
	return m;
}

struct pcam_monitor *pcam_monitor_start(int track, int kbps, pcam_monitor_packet_fn fn, void *param,
					struct pcam_monitor_info *info, char *error, size_t error_size)
{
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || !ovi.output_height || !ovi.fps_den) {
		snprintf(error, error_size, "OBS video is not running");
		return NULL;
	}
	uint32_t height = ovi.output_height < MONITOR_HEIGHT ? ovi.output_height : MONITOR_HEIGHT;
	uint32_t width = (uint32_t)((uint64_t)height * ovi.output_width / ovi.output_height) & ~1u;
	height &= ~1u;
	double fps = (double)ovi.fps_num / ovi.fps_den;
	uint32_t divisor = fps > MONITOR_FPS * 1.5 ? (uint32_t)(fps / MONITOR_FPS + 0.5) : 1;

	/* 그래픽카드 인코더를 먼저 써서 PC 부담을 줄이고, 없거나 실패하면 x264 */
	static const char *const encoders[] = {"obs_nvenc_h264_tex", "h264_texture_amf", "obs_x264"};
	for (size_t i = 0; i < sizeof(encoders) / sizeof(encoders[0]); i++) {
		struct pcam_monitor *m = try_start(encoders[i], track, kbps, fn, param, width, height, divisor);
		if (!m)
			continue;
		info->width = width;
		info->height = height;
		info->fps = (uint32_t)(fps / divisor + 0.5);
		info->audio = m->audio != NULL;
		snprintf(info->encoder, sizeof(info->encoder), "%s", encoders[i]);
		blog(LOG_INFO, "[phonecam] monitor started: %ux%u @ %u fps with %s, audio %s", width, height, info->fps,
		     encoders[i], m->audio ? "on" : "off");
		return m;
	}
	snprintf(error, error_size, "no H.264 encoder could start");
	return NULL;
}

void pcam_monitor_stop(struct pcam_monitor *m)
{
	if (!m)
		return;
	release_monitor(m);
	blog(LOG_INFO, "[phonecam] monitor stopped");
}

void pcam_monitor_set_bitrate(struct pcam_monitor *m, int kbps)
{
	if (!m || kbps <= 0)
		return;
	obs_data_t *settings = obs_encoder_get_settings(m->video);
	if (obs_data_get_int(settings, "bitrate") != kbps) {
		obs_data_set_int(settings, "bitrate", kbps);
		obs_encoder_update(m->video, settings); /* 실행 중에도 비트레이트만 바꾼다 (NVENC·AMF·x264) */
		blog(LOG_INFO, "[phonecam] monitor bitrate %d kbps", kbps);
	}
	obs_data_release(settings);
}
