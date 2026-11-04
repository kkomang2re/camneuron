#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include "adb.h"
#include "audio.h"
#include "control.h"
#include "decoder.h"
#include "discovery.h"
#include "net.h"
#include "overlay.h"
#include "monitor.h"
#include "pairing.h"
#include "protocol.h"
#include "remote.h"
#include "writer.h"

#define S_CONNECTION "connection"
#define S_HOST "host"
#define S_PORT "port"
#define S_ADB_PATH "adb_path"
#define S_HW_DECODE "hw_decode"
#define S_LOW_LATENCY "low_latency"
#define S_RECONNECT "reconnect"
#define S_FOCUS_CENTER "focus_center"
#define S_STATUS "status"
#define S_CHAT_URL "chat_url"
#define S_CHAT_STREAM "chat_stream"
#define S_ALERT_URL "alert_url"
#define S_ALERT_STREAM "alert_stream"
#define S_DEVICE_ID "device_id"
#define S_FIND "find_phone"
#define S_FOUND "found_phone"
#define S_PAIR_CODE "pair_code"
#define S_PHONE_SCENE "phone_scene"
#define S_MONITOR_TRACK "monitor_track"
#define S_MONITOR_REMOVE_MICS "monitor_remove_mics"

/* 소스의 필터 목록에 보이는 이름. 0.2.0까지는 LEGACY 이름이었으므로 그 이름의 필터를 찾으면 새 이름으로 바꾼다. */
#define OVERLAY_FILTER_NAME "CamNeuron chat/alert overlay"
#define LEGACY_OVERLAY_FILTER_NAME "PhoneCam chat/alert overlay"

#define T_(text) obs_module_text(text)

#define CONNECT_TIMEOUT_MS 3000
#define RECV_TIMEOUT_MS 5000
#define RETRY_DELAY_MS 1000
/* 폰이 승인을 거절한 뒤 다시 접속하는 간격. 이렇게 다시 물어야 폰 화면의 코드가 사라지지 않는다 (1분). */
#define AUTH_RETRY_DELAY_MS 2000
#define MAX_AUTH_MESSAGE_BYTES (64 * 1024)
#define PING_INTERVAL_NS 2000000000ULL
#define VIEW_INTERVAL_NS 1000000000ULL
#define OBS_STATE_INTERVAL_NS 1000000000ULL
#define STATS_INTERVAL_NS 1000000000ULL
#define MAX_LATENCY_US 5000000LL
/* 소리를 낼 시각이 지금보다 이만큼 넘게 지났으면 버린다 (OBS가 모든 소리의 버퍼를 늘리지 않게). */
#define MAX_AUDIO_LATE_US 150000LL
#define MAX_AUDIO_EARLY_US 1500000LL
#define DISCOVERY_TIMEOUT_MS 800
/* 저장된 주소로 이만큼 연달아 접속하지 못하면 같은 폰(ID)을 네트워크에서 다시 찾는다 (IP가 바뀐 경우). */
#define REDISCOVER_AFTER_FAILURES 3

enum connection_type {
	CONNECTION_WIFI = 0,
	CONNECTION_USB = 1,
};

/* 폰이 연결을 받아 주지 않은 이유 (PROTOCOL.md 1.1). 속성 창 상태에 보여 준다. */
enum auth_status {
	AUTH_NONE = 0,      /* 문제 없음 (연결됐거나 아직 접속 전) */
	AUTH_PAIR_REQUIRED, /* 처음 보는 PC: 폰 화면의 코드를 입력해야 함 */
	AUTH_BAD_CODE,      /* 입력한 코드가 틀림 */
	AUTH_LOCKED,        /* 틀린 코드가 여러 번이라 폰이 잠시 코드를 받지 않음 */
	AUTH_VERSION,       /* 폰 앱의 프로토콜 버전이 다름 */
	AUTH_CLOSED,        /* 폰이 아무 답 없이 끊음 (옛 버전 앱이거나 승인 대기 접속이 너무 많음) */
};

/* 바뀌면 다시 접속해야 하는 설정. 송출 형식과 카메라 값은 접속을 유지한 채 제어 메시지로 바꾼다. */
struct connection_settings {
	enum connection_type type;
	char *host;
	int port;
	char *adb_path;
	bool hw_decode;
	struct pcam_hello hello; /* 접속할 때 wanted에서 채운다 */
};

/* OBS 속성 창에서 넣는 채팅·후원 창 값. 위치·크기는 폰에서만 정한다. */
struct overlay_wanted {
	char chat_url[PCAM_URL_MAX];
	char alert_url[PCAM_URL_MAX];
	bool chat_stream;
	bool alert_stream;
};

struct phonecam_source {
	obs_source_t *source;

	/* 아래 묶음, sock, 소켓 쓰기는 mutex로 보호한다. */
	pthread_mutex_t mutex;
	struct connection_settings settings;
	long generation; /* 접속 설정이 바뀔 때마다 증가. 수신 스레드는 이를 보고 다시 접속한다. */
	pcam_socket_t sock;
	bool sock_ready; /* 승인을 마쳐 다른 스레드도 제어 메시지를 보내도 됨 (승인 메시지와 섞이지 않게) */
	struct pcam_writer *writer;   /* 승인 뒤 폰으로 보내는 것은 모두 이 스레드가 보낸다 */
	struct pcam_monitor *monitor; /* 폰이 켠 방송 모니터 (protocol 9.3) */
	char phone_scene_setting[256]; /* 속성의 "폰 장면" (비면 자동) */
	int monitor_track;             /* 모니터로 보낼 오디오 트랙 1~6 */
	enum auth_status auth;
	int auth_attempts_left;
	int auth_retry_s;
	uint16_t phone_version;                       /* AUTH_VERSION일 때 폰이 알려 온 버전 */
	char pair_code[PCAM_PAIR_CODE_LEN + 1];       /* 속성에 입력한 코드 (6자리일 때만) */
	char rejected_code[PCAM_PAIR_CODE_LEN + 1];   /* 폰이 틀렸다고 한 코드. 다시 보내지 않는다 */
	struct pcam_controls wanted; /* OBS 설정에 있는 값 */
	struct pcam_controls phone;  /* 폰이 알려 온 현재 값 */
	bool have_phone_state;
	bool caps_changed;
	struct pcam_caps caps;
	struct pcam_stats stats;
	char device_name[64];
	uint32_t stream_width;
	uint32_t stream_height;
	uint32_t stream_fps;
	uint32_t stream_codec; /* 폰이 state로 알려 온 실제 코덱. 바뀌면 디코더를 바꾼다. */
	int heat_level;        /* 0 = 정상, 1~3 = 폰이 뜨거워 요청보다 낮게 보내는 단계 */
	bool free_limited;     /* 폰 앱이 무료 버전이라 요청보다 낮게(720p 30fps) 보내는 중 */
	double send_mbps;      /* 폰이 실제로 쓰는 비트레이트 */
	double latency_ms;
	double decode_fps;
	bool hw_decoding;
	struct overlay_wanted overlay_wanted; /* OBS 설정에 있는 값 */
	struct pcam_overlays overlays;        /* 폰이 알려 온 채팅·후원 창 */
	bool have_overlays;
	char audio_desc[32]; /* 예: "Opus 48 kHz". 소리 설정을 받기 전엔 빈 문자열 */
	int audio_channels;
	char device_id[64]; /* 마지막으로 연결한 폰의 ID. IP가 바뀌면 이것으로 다시 찾는다 */
	struct pcam_found_phone found[PCAM_MAX_FOUND]; /* 마지막으로 찾은 폰들 */
	size_t found_count;
	bool searched;

	pthread_t thread;
	bool thread_created;
	volatile bool stopping;
	volatile long sync_queued;
	volatile long overlay_queued;
	os_event_t *wake;

	/* "폰 찾기" 버튼 (UI 스레드가 시작하고 destroy에서 기다린다) */
	pthread_t find_thread;
	bool find_thread_created;
	volatile long finding;

	/* 수신 스레드 전용 */
	uint8_t *buffer;
	size_t buffer_capacity;
	bool warned_format;
	bool logged_first_frame;
	uint64_t last_ping_ns;
	uint64_t ping_sent_us;
	long long ping_id;
	uint64_t last_view_ns;
	int sent_flip; /* 폰에 알린 장면 뒤집기 (1 좌우, 2 상하). -1이면 아직 안 알림 */
	uint64_t last_obs_refresh_ns;
	long sent_obs_version; /* 폰에 알린 OBS 상태 번호. -1이면 아직 안 알림 */
	bool sent_phone_mode;
	char sent_phone_scene[256];
	bool obs_tick; /* 1초가 지나 방송 시간을 다시 알릴 때 */

	/* 폰에서 방송 진행 (UI 스레드에서만 쓴다). phone_mode는 수신 스레드가 읽는 사본. */
	struct pcam_remote_mode remote_mode;
	volatile bool phone_mode;
	char remove_mics_result[128]; /* 속성의 "트랙에서 마이크 빼기" 결과 */
	bool clock_valid;
	int64_t clock_offset_us;
	int64_t best_rtt_us;
	uint64_t window_start_ns;
	uint32_t window_frames;
	int64_t window_latency_us;
	uint32_t window_latency_count;
	uint32_t stats_windows;
	int64_t last_latency_us; /* 마지막으로 잰 촬영→디코딩 지연. 0이면 아직 모름 */
	struct pcam_audio *audio;
	int64_t audio_delay_us; /* 녹음 시각에서 소리를 낼 때까지. 0이면 아직 안 정함 */
	int64_t window_audio_latency_us;
	uint32_t window_audio_count;
	uint32_t window_audio_late;
	int connect_failures;
};

/* ------------------------------------------------------------------------- */
/* 설정 */

static void settings_free(struct connection_settings *settings)
{
	bfree(settings->host);
	bfree(settings->adb_path);
	settings->host = NULL;
	settings->adb_path = NULL;
}

static void settings_copy(struct connection_settings *dst, const struct connection_settings *src)
{
	*dst = *src;
	dst->host = bstrdup(src->host);
	dst->adb_path = bstrdup(src->adb_path);
}

static bool str_equal(const char *a, const char *b)
{
	return strcmp(a ? a : "", b ? b : "") == 0;
}

static bool settings_equal(const struct connection_settings *a, const struct connection_settings *b)
{
	return a->type == b->type && a->port == b->port && str_equal(a->host, b->host) &&
	       str_equal(a->adb_path, b->adb_path) && a->hw_decode == b->hw_decode;
}

/* 폰 화면에 "192.168.0.12:9610"처럼 표시되므로 주소에 포트가 붙어 있으면 떼어 낸다. */
static char *parse_host(const char *input, int *port)
{
	struct dstr host = {0};
	dstr_copy(&host, input);
	dstr_depad(&host);

	const char *colon = host.array ? strchr(host.array, ':') : NULL;
	if (colon && !strchr(colon + 1, ':')) {
		int parsed = atoi(colon + 1);
		if (parsed > 0 && parsed < 65536)
			*port = parsed;
		dstr_resize(&host, (size_t)(colon - host.array));
	}
	return host.array;
}

static bool send_json(pcam_socket_t sock, obs_data_t *msg)
{
	const char *json = obs_data_get_json(msg);
	size_t len = strlen(json);
	uint8_t header[PCAM_HEADER_SIZE];
	pcam_header_encode(PCAM_STREAM_CONTROL, (uint32_t)len, header);
	return pcam_send_all(sock, header, sizeof(header)) && pcam_send_all(sock, json, len);
}

/* mutex를 잡은 상태에서 부른다. 보내기 스레드에 넣기만 하므로 Wi-Fi가 느려도 부른 스레드(OBS UI 등)는 기다리지 않는다. */
static void send_control_locked(struct phonecam_source *ctx, obs_data_t *msg)
{
	if (ctx->sock == PCAM_INVALID_SOCKET || !ctx->sock_ready || !ctx->writer)
		return;
	const char *json = obs_data_get_json(msg);
	pcam_writer_control(ctx->writer, json, strlen(json));
}

static void send_raw_control(struct phonecam_source *ctx, const char *json)
{
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->sock_ready && ctx->writer)
		pcam_writer_control(ctx->writer, json, strlen(json));
	pthread_mutex_unlock(&ctx->mutex);
}

static void send_control(struct phonecam_source *ctx, obs_data_t *msg)
{
	pthread_mutex_lock(&ctx->mutex);
	send_control_locked(ctx, msg);
	pthread_mutex_unlock(&ctx->mutex);
}

/*
 * mutex를 잡은 상태에서 부른다. 폰 값과 OBS 값이 다르면 OBS 값을 폰에 보낸다.
 * 접속 직후에는 송출 형식을 Hello로 이미 보냈으므로 include_format을 false로 부른다.
 */
static void push_wanted_locked(struct phonecam_source *ctx, bool include_format)
{
	if (!ctx->have_phone_state)
		return;
	obs_data_t *msg = obs_data_create();
	obs_data_set_string(msg, "type", "set");
	if (pcam_controls_diff(&ctx->phone, &ctx->wanted, msg, include_format)) {
		send_control_locked(ctx, msg);
		/* 폰이 곧 적용한 값을 state로 알려 온다. 보내지 않은 송출 형식은 폰이 알려 온 실제 값을 유지한다. */
		struct pcam_controls next = ctx->wanted;
		if (!include_format) {
			next.width = ctx->phone.width;
			next.height = ctx->phone.height;
			next.fps = ctx->phone.fps;
			next.codec = ctx->phone.codec;
			next.cbr = ctx->phone.cbr;
			next.bitrate_mbps = ctx->phone.bitrate_mbps;
		}
		ctx->phone = next;
	}
	obs_data_release(msg);
}

static void overlay_wanted_from_settings(struct overlay_wanted *w, obs_data_t *settings)
{
	struct dstr url = {0};
	dstr_copy(&url, obs_data_get_string(settings, S_CHAT_URL));
	dstr_depad(&url);
	snprintf(w->chat_url, sizeof(w->chat_url), "%s", url.array ? url.array : "");
	dstr_copy(&url, obs_data_get_string(settings, S_ALERT_URL));
	dstr_depad(&url);
	snprintf(w->alert_url, sizeof(w->alert_url), "%s", url.array ? url.array : "");
	dstr_free(&url);
	w->chat_stream = obs_data_get_bool(settings, S_CHAT_STREAM);
	w->alert_stream = obs_data_get_bool(settings, S_ALERT_STREAM);
}

/*
 * mutex를 잡은 상태에서 부른다. OBS 속성의 채팅·후원 창 값이 폰과 다르면 폰에 보낸다.
 * only_missing이면 폰에 URL이 비어 있는 것만 채운다 (접속 직후: 폰에서 정한 값을 우선).
 */
static void push_overlay_wanted_locked(struct phonecam_source *ctx, bool only_missing)
{
	if (!ctx->have_overlays)
		return;
	struct overlay_wanted *w = &ctx->overlay_wanted;
	struct pcam_overlays *o = &ctx->overlays;
	obs_data_t *msg = obs_data_create();
	obs_data_set_string(msg, "type", "set");
	bool any = false;

	if (strcmp(w->chat_url, o->chat.url) != 0 && (!only_missing || (!o->chat.url[0] && w->chat_url[0]))) {
		obs_data_set_string(msg, "chat_url", w->chat_url);
		snprintf(o->chat.url, sizeof(o->chat.url), "%s", w->chat_url);
		any = true;
	}
	if (strcmp(w->alert_url, o->alert.url) != 0 && (!only_missing || (!o->alert.url[0] && w->alert_url[0]))) {
		obs_data_set_string(msg, "alert_url", w->alert_url);
		snprintf(o->alert.url, sizeof(o->alert.url), "%s", w->alert_url);
		any = true;
	}
	if (!only_missing && w->chat_stream != o->chat.stream) {
		obs_data_set_bool(msg, "chat_stream", w->chat_stream);
		o->chat.stream = w->chat_stream;
		any = true;
	}
	if (!only_missing && w->alert_stream != o->alert.stream) {
		obs_data_set_bool(msg, "alert_stream", w->alert_stream);
		o->alert.stream = w->alert_stream;
		any = true;
	}
	if (any)
		send_control_locked(ctx, msg);
	obs_data_release(msg);
}

static void phonecam_update(void *data, obs_data_t *settings)
{
	struct phonecam_source *ctx = data;
	struct connection_settings next = {0};

	next.type = (enum connection_type)obs_data_get_int(settings, S_CONNECTION);
	next.port = (int)obs_data_get_int(settings, S_PORT);
	next.host = parse_host(obs_data_get_string(settings, S_HOST), &next.port);
	next.adb_path = bstrdup(obs_data_get_string(settings, S_ADB_PATH));
	next.hw_decode = obs_data_get_bool(settings, S_HW_DECODE);

	struct pcam_controls wanted;
	pcam_controls_from_settings(&wanted, settings);
	struct overlay_wanted overlay_wanted;
	overlay_wanted_from_settings(&overlay_wanted, settings);
	/* 입력하는 중(6자리가 되기 전)에는 보내지 않는다. 틀린 코드로 셀 수 있어서 */
	char code[PCAM_PAIR_CODE_LEN + 1];
	if (!pcam_pairing_normalize_code(obs_data_get_string(settings, S_PAIR_CODE), code))
		code[0] = 0;

	obs_source_set_async_unbuffered(ctx->source, obs_data_get_bool(settings, S_LOW_LATENCY));

	pthread_mutex_lock(&ctx->mutex);
	bool reconnect = !settings_equal(&ctx->settings, &next);
	/* 새 코드를 넣으면 다음 접속을 기다리지 않고 바로 보낸다. */
	bool new_code = code[0] && strcmp(code, ctx->pair_code) != 0;
	snprintf(ctx->pair_code, sizeof(ctx->pair_code), "%s", code);
	ctx->wanted = wanted;
	ctx->overlay_wanted = overlay_wanted;
	snprintf(ctx->phone_scene_setting, sizeof(ctx->phone_scene_setting), "%s",
		 obs_data_get_string(settings, S_PHONE_SCENE));
	int track = (int)obs_data_get_int(settings, S_MONITOR_TRACK);
	ctx->monitor_track = track >= 1 && track <= 6 ? track : 6;
	snprintf(ctx->device_id, sizeof(ctx->device_id), "%s", obs_data_get_string(settings, S_DEVICE_ID));
	if (reconnect) {
		settings_free(&ctx->settings);
		ctx->settings = next;
		ctx->generation++;
		pcam_shutdown(ctx->sock);
	} else {
		push_wanted_locked(ctx, true);
		push_overlay_wanted_locked(ctx, false);
	}
	pthread_mutex_unlock(&ctx->mutex);

	if (reconnect || new_code)
		os_event_signal(ctx->wake);
	if (!reconnect)
		settings_free(&next);
}

/* ------------------------------------------------------------------------- */
/* 폰 → OBS 설정 동기화 (UI 스레드) */

static void sync_settings_task(void *param)
{
	obs_weak_source_t *weak = param;
	obs_source_t *source = obs_weak_source_get_source(weak);
	obs_weak_source_release(weak);
	if (!source)
		return;

	struct phonecam_source *ctx = obs_obj_get_data(source);
	os_atomic_set_long(&ctx->sync_queued, 0);

	pthread_mutex_lock(&ctx->mutex);
	struct pcam_controls phone = ctx->phone;
	bool have_state = ctx->have_phone_state;
	bool caps_changed = ctx->caps_changed;
	ctx->caps_changed = false;
	pthread_mutex_unlock(&ctx->mutex);

	obs_data_t *settings = obs_source_get_settings(source);
	struct pcam_controls current;
	pcam_controls_from_settings(&current, settings);
	bool changed = have_state && !pcam_controls_equal(&current, &phone);
	if (changed) {
		/* 폰 화면에서 바꾼 값. OBS 설정에 적고, 되돌려 보내지 않도록 wanted도 맞춘다. */
		pcam_controls_to_settings(&phone, settings);
		pthread_mutex_lock(&ctx->mutex);
		ctx->wanted = phone;
		pthread_mutex_unlock(&ctx->mutex);
	}
	obs_data_release(settings);

	if (changed || caps_changed)
		obs_source_update_properties(source);
	obs_source_release(source);
}

static void queue_settings_sync(struct phonecam_source *ctx)
{
	if (os_atomic_exchange_long(&ctx->sync_queued, 1) == 1)
		return; /* 이미 대기 중인 작업이 최신 값을 읽는다 */
	obs_queue_task(OBS_TASK_UI, sync_settings_task, obs_source_get_weak_source(ctx->source), false);
}

/* ------------------------------------------------------------------------- */
/* 채팅·후원 창: 합성 필터 관리와 OBS 설정 동기화 (UI 스레드) */

static void sync_overlay_settings(struct phonecam_source *ctx, obs_source_t *source, const struct pcam_overlays *o)
{
	obs_data_t *settings = obs_source_get_settings(source);
	struct overlay_wanted current;
	overlay_wanted_from_settings(&current, settings);
	bool changed = strcmp(current.chat_url, o->chat.url) != 0 || strcmp(current.alert_url, o->alert.url) != 0 ||
		       current.chat_stream != o->chat.stream || current.alert_stream != o->alert.stream;
	if (changed) {
		/* 폰에서 바꾼 값. OBS 설정에 적고, 되돌려 보내지 않도록 overlay_wanted도 맞춘다. */
		obs_data_set_string(settings, S_CHAT_URL, o->chat.url);
		obs_data_set_string(settings, S_ALERT_URL, o->alert.url);
		obs_data_set_bool(settings, S_CHAT_STREAM, o->chat.stream);
		obs_data_set_bool(settings, S_ALERT_STREAM, o->alert.stream);
		pthread_mutex_lock(&ctx->mutex);
		overlay_wanted_from_settings(&ctx->overlay_wanted, settings);
		pthread_mutex_unlock(&ctx->mutex);
	}
	obs_data_release(settings);
	if (changed)
		obs_source_update_properties(source);
}

static void overlay_task(void *param)
{
	obs_weak_source_t *weak = param;
	obs_source_t *source = obs_weak_source_get_source(weak);
	obs_weak_source_release(weak);
	if (!source)
		return;

	struct phonecam_source *ctx = obs_obj_get_data(source);
	os_atomic_set_long(&ctx->overlay_queued, 0);

	pthread_mutex_lock(&ctx->mutex);
	struct pcam_overlays *overlays = bmemdup(&ctx->overlays, sizeof(ctx->overlays));
	uint32_t width = ctx->stream_width;
	uint32_t height = ctx->stream_height;
	pthread_mutex_unlock(&ctx->mutex);

	if (overlays->valid) {
		sync_overlay_settings(ctx, source, overlays);

		/* 송출에 포함할 창이 있을 때만 숨은 합성 필터를 단다. */
		obs_source_t *filter = obs_source_get_filter_by_name(source, OVERLAY_FILTER_NAME);
		if (!filter) {
			filter = obs_source_get_filter_by_name(source, LEGACY_OVERLAY_FILTER_NAME);
			if (filter)
				obs_source_set_name(filter, OVERLAY_FILTER_NAME);
		}
		bool streamed = pcam_overlays_streamed(overlays);
		if (streamed && !filter) {
			filter = obs_source_create_private(PCAM_OVERLAY_FILTER_ID, OVERLAY_FILTER_NAME, NULL);
			if (filter)
				obs_source_filter_add(source, filter);
		}
		if (filter) {
			if (streamed) {
				pcam_overlay_filter_apply(filter, overlays, width, height);
			} else {
				obs_source_filter_remove(source, filter);
			}
			obs_source_release(filter);
		}
	}
	bfree(overlays);
	obs_source_release(source);
}

static void queue_overlay_update(struct phonecam_source *ctx)
{
	if (os_atomic_exchange_long(&ctx->overlay_queued, 1) == 1)
		return;
	obs_queue_task(OBS_TASK_UI, overlay_task, obs_source_get_weak_source(ctx->source), false);
}

static void handle_overlays(struct phonecam_source *ctx, obs_data_t *msg)
{
	struct pcam_overlays *overlays = bzalloc(sizeof(*overlays));
	pcam_overlays_from_json(overlays, msg);

	pthread_mutex_lock(&ctx->mutex);
	bool first = !ctx->have_overlays;
	ctx->overlays = *overlays;
	ctx->have_overlays = true;
	/* 접속 직후에는 폰에서 정한 값을 우선하고, 폰에 URL이 없을 때만 OBS에 넣어 둔 URL을 보낸다. */
	if (first)
		push_overlay_wanted_locked(ctx, true);
	pthread_mutex_unlock(&ctx->mutex);

	bfree(overlays);
	queue_overlay_update(ctx);
}

/* ------------------------------------------------------------------------- */
/* 폰 찾기 */

static void refresh_properties_task(void *param)
{
	obs_weak_source_t *weak = param;
	obs_source_t *source = obs_weak_source_get_source(weak);
	obs_weak_source_release(weak);
	if (!source)
		return;
	obs_source_update_properties(source);
	obs_source_release(source);
}

struct remember_phone_task {
	obs_weak_source_t *weak;
	char host[64]; /* 비어 있으면 ID만 적는다 */
	int port;
	char id[64];
};

/* 찾은 폰을 OBS 설정에 적는다 (UI 스레드). 다음에 OBS를 켜도 이 주소로 접속하고, 주소가 바뀌면 ID로 다시 찾는다. */
static void remember_phone_task(void *param)
{
	struct remember_phone_task *task = param;
	obs_source_t *source = obs_weak_source_get_source(task->weak);
	obs_weak_source_release(task->weak);
	if (source) {
		obs_data_t *settings = obs_source_get_settings(source);
		if (task->host[0]) {
			obs_data_set_string(settings, S_HOST, task->host);
			obs_data_set_int(settings, S_PORT, task->port);
		}
		obs_data_set_string(settings, S_DEVICE_ID, task->id);
		obs_data_release(settings);
		obs_source_update_properties(source);
		obs_source_release(source);
	}
	bfree(task);
}

static void queue_remember_phone(struct phonecam_source *ctx, const char *host, int port, const char *id)
{
	struct remember_phone_task *task = bzalloc(sizeof(*task));
	task->weak = obs_source_get_weak_source(ctx->source);
	snprintf(task->host, sizeof(task->host), "%s", host);
	task->port = port;
	snprintf(task->id, sizeof(task->id), "%s", id);
	obs_queue_task(OBS_TASK_UI, remember_phone_task, task, false);
}

/* 네트워크에서 폰을 찾아 목록을 바꾼다. 찾은 수를 돌려준다. */
static size_t search_phones(struct phonecam_source *ctx)
{
	struct pcam_found_phone found[PCAM_MAX_FOUND];
	size_t count = pcam_discover(found, PCAM_MAX_FOUND, DISCOVERY_TIMEOUT_MS);
	pthread_mutex_lock(&ctx->mutex);
	memcpy(ctx->found, found, count * sizeof(found[0]));
	ctx->found_count = count;
	ctx->searched = true;
	pthread_mutex_unlock(&ctx->mutex);
	return count;
}

/*
 * 찾은 폰 중 접속할 폰을 고른다. 전에 연결한 폰(ID)이 있으면 그 폰만 고른다.
 * ID를 모를 때 any면 다른 PC와 연결 중이 아닌 첫 폰을, 아니면 폰이 하나뿐일 때만 그 폰을 고른다.
 */
static bool pick_found(struct phonecam_source *ctx, bool any, struct pcam_found_phone *out)
{
	struct pcam_found_phone found[PCAM_MAX_FOUND];
	char device_id[sizeof(ctx->device_id)];
	pthread_mutex_lock(&ctx->mutex);
	size_t count = ctx->found_count;
	memcpy(found, ctx->found, count * sizeof(found[0]));
	snprintf(device_id, sizeof(device_id), "%s", ctx->device_id);
	pthread_mutex_unlock(&ctx->mutex);

	const struct pcam_found_phone *pick = NULL;
	if (device_id[0]) {
		for (size_t i = 0; i < count && !pick; i++) {
			if (strcmp(found[i].id, device_id) == 0)
				pick = &found[i];
		}
	} else if (any) {
		/* 이 PC를 승인한 폰을 먼저 고른다. 같은 네트워크의 남의 폰에 연결 요청을 띄우지 않게. */
		uint8_t token[PCAM_TOKEN_BYTES];
		for (size_t i = 0; i < count && !pick; i++) {
			if (!found[i].busy && pcam_pairing_token(found[i].id, token))
				pick = &found[i];
		}
		for (size_t i = 0; i < count && !pick; i++) {
			if (!found[i].busy)
				pick = &found[i];
		}
		if (!pick && count)
			pick = &found[0];
	} else if (count == 1) {
		pick = &found[0];
	}
	if (pick)
		*out = *pick;
	return pick != NULL;
}

/* 접속 설정을 찾은 폰으로 바꾸고 OBS 설정에도 적는다. 연결 스레드는 다음 접속부터 이 주소를 쓴다. */
static void adopt_phone(struct phonecam_source *ctx, const struct pcam_found_phone *phone)
{
	pthread_mutex_lock(&ctx->mutex);
	bfree(ctx->settings.host);
	ctx->settings.host = bstrdup(phone->host);
	ctx->settings.port = phone->port;
	snprintf(ctx->device_id, sizeof(ctx->device_id), "%s", phone->id);
	pthread_mutex_unlock(&ctx->mutex);

	blog(LOG_INFO, "[phonecam] found phone '%s' at %s:%d", phone->name, phone->host, phone->port);
	queue_remember_phone(ctx, phone->host, phone->port, phone->id);
}

static void *find_thread_main(void *data)
{
	struct phonecam_source *ctx = data;
	os_set_thread_name("phonecam-find");
	size_t count = search_phones(ctx);
	blog(LOG_INFO, "[phonecam] phone search found %zu phone(s)", count);
	os_atomic_set_long(&ctx->finding, 0);
	obs_queue_task(OBS_TASK_UI, refresh_properties_task, obs_source_get_weak_source(ctx->source), false);
	return NULL;
}

/* "폰 찾기" 버튼. 찾는 동안(1초 미만) 속성 창이 멈추지 않게 따로 스레드에서 찾는다. */
static void start_find(struct phonecam_source *ctx)
{
	if (os_atomic_exchange_long(&ctx->finding, 1) == 1)
		return;
	if (ctx->find_thread_created)
		pthread_join(ctx->find_thread, NULL); /* 이전 검색은 이미 끝났다 */
	ctx->find_thread_created = pthread_create(&ctx->find_thread, NULL, find_thread_main, ctx) == 0;
	if (!ctx->find_thread_created)
		os_atomic_set_long(&ctx->finding, 0);
}

/* ------------------------------------------------------------------------- */
/* 제어 메시지 수신 */

/* 폰이 접속 직후 알려 주는 ID. 주소를 직접 넣어 연결한 폰도 IP가 바뀌면 다시 찾을 수 있게 기억한다. */
static void handle_device(struct phonecam_source *ctx, obs_data_t *msg)
{
	const char *id = obs_data_get_string(msg, "id");
	if (!*id)
		return;
	pthread_mutex_lock(&ctx->mutex);
	bool changed = strcmp(ctx->device_id, id) != 0;
	if (changed)
		snprintf(ctx->device_id, sizeof(ctx->device_id), "%s", id);
	pthread_mutex_unlock(&ctx->mutex);
	if (changed)
		queue_remember_phone(ctx, "", 0, id);
}

static void handle_state(struct phonecam_source *ctx, obs_data_t *msg)
{
	struct pcam_controls state;
	pcam_controls_from_state(&state, msg);

	/*
	 * width/height/fps/bitrate는 요청 값이라 OBS 설정에 그대로 남긴다. 폰이 뜨거우면 send_*로 더 낮게 보내므로
	 * 화면 크기(합성 위치 계산)와 상태 표시는 send_* 값을 쓴다.
	 */
	uint32_t width = (uint32_t)obs_data_get_int(msg, "send_width");
	uint32_t height = (uint32_t)obs_data_get_int(msg, "send_height");
	uint32_t fps = (uint32_t)obs_data_get_int(msg, "send_fps");
	if (!width || !height) {
		width = state.width;
		height = state.height;
	}
	if (!fps)
		fps = state.fps;
	int heat_level = (int)obs_data_get_int(msg, "heat_level");
	bool free_limited = obs_data_get_bool(msg, "free_limit");
	double send_mbps = obs_data_has_user_value(msg, "send_bitrate") ? obs_data_get_int(msg, "send_bitrate") / 1e6
									 : state.bitrate_mbps;

	pthread_mutex_lock(&ctx->mutex);
	bool first = !ctx->have_phone_state;
	bool resized = ctx->stream_width != width || ctx->stream_height != height;
	int previous_heat = ctx->heat_level;
	bool was_free_limited = ctx->free_limited;
	ctx->phone = state;
	ctx->have_phone_state = true;
	ctx->stream_width = width;
	ctx->stream_height = height;
	ctx->stream_fps = fps;
	ctx->stream_codec = state.codec;
	ctx->heat_level = heat_level;
	ctx->free_limited = free_limited;
	ctx->send_mbps = send_mbps;
	/*
	 * 접속 직후에는 OBS에 저장된 카메라 값을 폰에 적용한다 (송출 형식은 Hello로 이미 보냄).
	 * 그 뒤로는 폰에서 바뀐 값을 OBS 설정이 따라간다.
	 */
	if (first)
		push_wanted_locked(ctx, false);
	bool overlays_streamed = ctx->have_overlays && pcam_overlays_streamed(&ctx->overlays);
	pthread_mutex_unlock(&ctx->mutex);

	if (free_limited != was_free_limited)
		blog(LOG_INFO, "[phonecam] %s", free_limited ? "free version on the phone: sending at most 720p 30 fps H.264"
							     : "phone sends the requested quality (Pro)");
	if (heat_level != previous_heat) {
		if (heat_level > 0)
			blog(LOG_WARNING, "[phonecam] phone is hot (level %d): sending %ux%u @ %u fps, %.1f Mbps", heat_level,
			     width, height, fps, send_mbps);
		else
			blog(LOG_INFO, "[phonecam] phone cooled down: back to %ux%u @ %u fps, %.1f Mbps", width, height, fps,
			     send_mbps);
	}

	queue_settings_sync(ctx);
	/* 해상도가 바뀌면 합성 위치(픽셀)도 다시 계산한다. */
	if (resized && overlays_streamed)
		queue_overlay_update(ctx);
}

static void handle_pong(struct phonecam_source *ctx, obs_data_t *msg)
{
	if (obs_data_get_int(msg, "id") != ctx->ping_id || !ctx->ping_sent_us)
		return;
	int64_t now_us = (int64_t)(os_gettime_ns() / 1000);
	int64_t sent_us = (int64_t)ctx->ping_sent_us;
	int64_t rtt = now_us - sent_us;
	int64_t offset = obs_data_get_int(msg, "clock_us") - (sent_us + now_us) / 2;

	/* 왕복이 가장 짧았던 측정이 가장 정확하다. 오래된 최솟값이 계속 이기지 않게 기준을 조금씩 늘린다. */
	if (!ctx->clock_valid || rtt <= ctx->best_rtt_us) {
		ctx->clock_offset_us = offset;
		ctx->best_rtt_us = rtt;
		ctx->clock_valid = true;
	} else {
		ctx->best_rtt_us += 1000;
	}
}

/* ------------------------------------------------------------------------- */
/* 폰에서 방송 진행 (protocol 9장) */

/* 모니터 인코더 스레드에서 불린다. 보내기 스레드 줄에 넣기만 한다. */
static void monitor_packet_to_phone(void *param, uint8_t stream_id, uint8_t flags, int64_t pts_us,
				    const uint8_t *data, size_t size)
{
	struct phonecam_source *ctx = param;
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->writer)
		pcam_writer_media(ctx->writer, stream_id, flags, pts_us, data, size);
	pthread_mutex_unlock(&ctx->mutex);
}

static void handle_monitor(struct phonecam_source *ctx, obs_data_t *msg)
{
	bool video = obs_data_get_bool(msg, "video");
	bool audio = obs_data_get_bool(msg, "audio");

	pthread_mutex_lock(&ctx->mutex);
	struct pcam_monitor *old = ctx->monitor;
	ctx->monitor = NULL;
	int track = ctx->monitor_track;
	pthread_mutex_unlock(&ctx->mutex);
	pcam_monitor_stop(old);

	obs_data_t *reply = obs_data_create();
	obs_data_set_string(reply, "type", "monitor_state");
	struct pcam_monitor_info info = {0};
	char error[128] = {0};
	struct pcam_monitor *monitor = NULL;
	if (video) {
		monitor = pcam_monitor_start(audio ? track : 0, monitor_packet_to_phone, ctx, &info, error, sizeof(error));
		if (!monitor)
			blog(LOG_WARNING, "[phonecam] monitor: %s", error);
	}
	obs_data_set_bool(reply, "video", monitor != NULL);
	obs_data_set_bool(reply, "audio", monitor && info.audio);
	if (monitor) {
		obs_data_set_int(reply, "width", info.width);
		obs_data_set_int(reply, "height", info.height);
		obs_data_set_int(reply, "fps", info.fps);
		obs_data_set_int(reply, "track", track);
		obs_data_set_bool(reply, "mic_on_track", info.audio && pcam_remote_mics_on_track(track));
	}
	if (error[0])
		obs_data_set_string(reply, "error", error);

	pthread_mutex_lock(&ctx->mutex);
	if (ctx->sock_ready && monitor) {
		ctx->monitor = monitor;
		monitor = NULL;
	}
	send_control_locked(ctx, reply);
	pthread_mutex_unlock(&ctx->mutex);
	pcam_monitor_stop(monitor); /* 그사이 연결이 끊겼으면 */
	obs_data_release(reply);
}

struct remote_command {
	obs_weak_source_t *weak;
	obs_data_t *cmd;
};

/* obs-frontend-api는 UI 스레드에서 부른다. */
static void remote_command_task(void *param)
{
	struct remote_command *task = param;
	obs_source_t *source = obs_weak_source_get_source(task->weak);
	if (source) {
		struct phonecam_source *ctx = obs_obj_get_data(source);
		char setting[sizeof(ctx->phone_scene_setting)];
		pthread_mutex_lock(&ctx->mutex);
		snprintf(setting, sizeof(setting), "%s", ctx->phone_scene_setting);
		pthread_mutex_unlock(&ctx->mutex);
		pcam_remote_execute(source, &ctx->remote_mode, setting, task->cmd);
		os_atomic_set_bool(&ctx->phone_mode, ctx->remote_mode.phone);
		obs_source_release(source);
	}
	obs_weak_source_release(task->weak);
	obs_data_release(task->cmd);
	bfree(task);
}

static void handle_remote_command(struct phonecam_source *ctx, obs_data_t *msg)
{
	struct remote_command *task = bzalloc(sizeof(*task));
	task->weak = obs_source_get_weak_source(ctx->source);
	task->cmd = msg;
	obs_data_addref(msg);
	obs_queue_task(OBS_TASK_UI, remote_command_task, task, false);
}

/* 연결 중에는 1초마다 OBS 상태를 모으게 하고, 바뀌었거나 방송·녹화 중이면(시간 표시) 폰에 알린다. */
static void maybe_send_obs_state(struct phonecam_source *ctx)
{
	uint64_t now = os_gettime_ns();
	bool tick = now - ctx->last_obs_refresh_ns >= OBS_STATE_INTERVAL_NS;
	if (tick) {
		ctx->last_obs_refresh_ns = now;
		pcam_remote_request_refresh();
		ctx->obs_tick = true;
	}
	long version = pcam_remote_version();
	if (version == 0) /* 아직 한 번도 모으지 않음 */
		return;
	bool phone_mode = os_atomic_load_bool(&ctx->phone_mode);
	bool due = version != ctx->sent_obs_version || phone_mode != ctx->sent_phone_mode ||
		   (ctx->obs_tick && pcam_remote_live());
	if (!due && !tick)
		return;

	/* 폰 장면은 장면을 고치거나 소스 크기를 알게 되면 바뀔 수 있어 1초마다 다시 본다. */
	char setting[sizeof(ctx->phone_scene_setting)];
	pthread_mutex_lock(&ctx->mutex);
	snprintf(setting, sizeof(setting), "%s", ctx->phone_scene_setting);
	pthread_mutex_unlock(&ctx->mutex);
	char *phone_scene = pcam_remote_phone_scene(ctx->source, setting);
	if (!due && strcmp(phone_scene, ctx->sent_phone_scene) == 0) {
		bfree(phone_scene);
		return;
	}
	ctx->obs_tick = false;
	ctx->sent_obs_version = version;
	ctx->sent_phone_mode = phone_mode;
	snprintf(ctx->sent_phone_scene, sizeof(ctx->sent_phone_scene), "%s", phone_scene);
	char *json = pcam_remote_state_json(ctx->source, phone_mode, phone_scene);
	send_raw_control(ctx, json);
	bfree(json);
	bfree(phone_scene);
}

static void handle_control(struct phonecam_source *ctx, const uint8_t *data, size_t size)
{
	char *json = bstrdup_n((const char *)data, size);
	obs_data_t *msg = obs_data_create_from_json(json);
	bfree(json);
	if (!msg)
		return;

	const char *type = obs_data_get_string(msg, "type");
	if (strcmp(type, "state") == 0) {
		handle_state(ctx, msg);
	} else if (strcmp(type, "caps") == 0) {
		pthread_mutex_lock(&ctx->mutex);
		pcam_caps_from_json(&ctx->caps, msg);
		ctx->caps_changed = true;
		pthread_mutex_unlock(&ctx->mutex);
		queue_settings_sync(ctx);
	} else if (strcmp(type, "stats") == 0) {
		pthread_mutex_lock(&ctx->mutex);
		pcam_stats_from_json(&ctx->stats, msg);
		pthread_mutex_unlock(&ctx->mutex);
	} else if (strcmp(type, "pong") == 0) {
		handle_pong(ctx, msg);
	} else if (strcmp(type, "overlays") == 0) {
		handle_overlays(ctx, msg);
	} else if (strcmp(type, "device") == 0) {
		handle_device(ctx, msg);
	} else if (strcmp(type, "monitor") == 0) {
		handle_monitor(ctx, msg);
	} else if (strcmp(type, "obs_cmd") == 0) {
		handle_remote_command(ctx, msg);
	}
	obs_data_release(msg);
}

static void maybe_send_ping(struct phonecam_source *ctx)
{
	uint64_t now = os_gettime_ns();
	if (now - ctx->last_ping_ns < PING_INTERVAL_NS)
		return;
	ctx->last_ping_ns = now;
	ctx->ping_id++;
	ctx->ping_sent_us = now / 1000;

	obs_data_t *msg = obs_data_create();
	obs_data_set_string(msg, "type", "ping");
	obs_data_set_int(msg, "id", ctx->ping_id);
	send_control(ctx, msg);
	obs_data_release(msg);
}

struct flip_search {
	obs_source_t *source;
	int scene_score;
	int best_score;
	bool flip_h;
	bool flip_v;
};

static bool collect_scene(void *param, obs_source_t *scene)
{
	DARRAY(obs_source_t *) *scenes = param;
	obs_source_t *ref = obs_source_get_ref(scene);
	if (ref)
		da_push_back(*scenes, &ref);
	return true;
}

static bool check_item(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct flip_search *search = param;
	if (obs_sceneitem_get_source(item) != search->source)
		return true;
	int score = search->scene_score + (obs_sceneitem_visible(item) ? 1 : 0);
	if (score <= search->best_score)
		return true;
	struct vec2 scale;
	obs_sceneitem_get_scale(item, &scale);
	bool upside_down = fabsf(fmodf(fabsf(obs_sceneitem_get_rot(item)), 360.0f) - 180.0f) < 45.0f;
	search->best_score = score;
	search->flip_h = (scale.x < 0.0f) != upside_down;
	search->flip_v = (scale.y < 0.0f) != upside_down;
	return true;
}

/*
 * 이 소스를 OBS 장면에서 좌우·상하로 뒤집어 쓰는지 (변환 → 뒤집기, 180도 회전 포함).
 * 여러 장면에 있으면 방송 중인 장면 > 미리보기 장면 > 보이는 항목 순으로 고른다. 그룹 자체의 뒤집기는 보지 않는다.
 * 장면 목록 잠금을 잡은 채 장면 항목을 보지 않도록 장면을 먼저 모은다.
 */
static void find_scene_flip(obs_source_t *source, bool *flip_h, bool *flip_v)
{
	DARRAY(obs_source_t *) scenes;
	da_init(scenes);
	obs_enum_scenes(collect_scene, &scenes);

	struct flip_search search = {.source = source, .best_score = -1};
	for (size_t i = 0; i < scenes.num; i++) {
		obs_source_t *scene_source = scenes.array[i];
		obs_scene_t *scene = obs_group_or_scene_from_source(scene_source);
		if (scene) {
			search.scene_score = (obs_source_active(scene_source) ? 4 : 0) +
					     (obs_source_showing(scene_source) ? 2 : 0);
			obs_scene_enum_items(scene, check_item, &search);
		}
		obs_source_release(scene_source);
	}
	da_free(scenes);
	*flip_h = search.flip_h;
	*flip_v = search.flip_v;
}

/* 장면에서 뒤집어 쓰면 폰이 워터마크를 미리 뒤집어 그려, 방송 화면에서는 바로 읽히게 한다 (프로토콜 "view"). */
static void maybe_send_view(struct phonecam_source *ctx)
{
	uint64_t now = os_gettime_ns();
	if (now - ctx->last_view_ns < VIEW_INTERVAL_NS)
		return;
	ctx->last_view_ns = now;

	bool flip_h, flip_v;
	find_scene_flip(ctx->source, &flip_h, &flip_v);
	int flip = (flip_h ? 1 : 0) | (flip_v ? 2 : 0);
	if (flip == ctx->sent_flip)
		return;
	ctx->sent_flip = flip;

	obs_data_t *msg = obs_data_create();
	obs_data_set_string(msg, "type", "view");
	obs_data_set_bool(msg, "flip_h", flip_h);
	obs_data_set_bool(msg, "flip_v", flip_v);
	send_control(ctx, msg);
	obs_data_release(msg);
}

/* ------------------------------------------------------------------------- */
/* 영상 출력 */

static enum video_colorspace colorspace_for(const AVFrame *frame)
{
	switch (frame->colorspace) {
	case AVCOL_SPC_BT709:
		return VIDEO_CS_709;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
		return VIDEO_CS_601;
	default:
		return frame->height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;
	}
}

/* 촬영→디코딩 지연과 수신 fps를 1초 단위로 모아 폰에도 알려 준다. */
static void update_stats(struct phonecam_source *ctx, const AVFrame *frame)
{
	uint64_t now_ns = os_gettime_ns();
	if (ctx->clock_valid && frame->pts > 0) {
		int64_t latency = (int64_t)(now_ns / 1000) - (frame->pts - ctx->clock_offset_us);
		if (latency > 0 && latency < MAX_LATENCY_US) {
			ctx->window_latency_us += latency;
			ctx->window_latency_count++;
		}
	}
	ctx->window_frames++;

	if (!ctx->window_start_ns) {
		ctx->window_start_ns = now_ns;
		return;
	}
	uint64_t elapsed = now_ns - ctx->window_start_ns;
	if (elapsed < STATS_INTERVAL_NS)
		return;

	double fps = ctx->window_frames * 1e9 / (double)elapsed;
	double latency_ms = ctx->window_latency_count
				    ? (double)ctx->window_latency_us / ctx->window_latency_count / 1000.0
				    : -1.0;
	if (ctx->window_latency_count)
		ctx->last_latency_us = ctx->window_latency_us / ctx->window_latency_count;

	obs_data_t *msg = obs_data_create();
	obs_data_set_string(msg, "type", "pc_stats");
	obs_data_set_int(msg, "latency_ms", latency_ms >= 0 ? (long long)(latency_ms + 0.5) : -1);
	obs_data_set_double(msg, "fps", fps);

	pthread_mutex_lock(&ctx->mutex);
	ctx->latency_ms = latency_ms;
	ctx->decode_fps = fps;
	send_control_locked(ctx, msg);
	double phone_mbps = ctx->stats.valid ? ctx->stats.bitrate / 1e6 : 0.0;
	bool hw = ctx->hw_decoding;
	pthread_mutex_unlock(&ctx->mutex);
	obs_data_release(msg);

	/* 문제를 진단할 수 있게 접속 5초 뒤, 그 뒤로 1분마다 로그에 남긴다. */
	if (++ctx->stats_windows % 60 == 5) {
		blog(LOG_INFO, "[phonecam] latency %.0f ms (capture to decode), %.1f fps, %.1f Mbps, %s decoding",
		     latency_ms, fps, phone_mbps, hw ? "GPU" : "CPU");
		if (ctx->audio && ctx->window_audio_count) {
			float peak = pcam_audio_take_peak(ctx->audio);
			blog(LOG_INFO,
			     "[phonecam] audio %.0f ms (capture to arrival), played %lld ms after capture, "
			     "peak %.0f dBFS, %u late packets dropped",
			     (double)ctx->window_audio_latency_us / ctx->window_audio_count / 1000.0,
			     (long long)(ctx->audio_delay_us / 1000), peak > 0.0f ? 20.0 * log10(peak) : -100.0,
			     ctx->window_audio_late);
		}
	}

	ctx->window_start_ns = now_ns;
	ctx->window_frames = 0;
	ctx->window_latency_us = 0;
	ctx->window_latency_count = 0;
	ctx->window_audio_latency_us = 0;
	ctx->window_audio_count = 0;
	ctx->window_audio_late = 0;
}

/* ------------------------------------------------------------------------- */
/* 소리 출력 */

/*
 * 이 소리를 낼 OBS 시각. 영상은 디코딩되는 즉시 화면에 나오므로 촬영보다 (촬영→디코딩 지연 + OBS 프레임 반 개)만큼
 * 늦게 보인다. 소리도 녹음 시각에서 같은 만큼 늦춰 내야 입 모양과 맞는다. 늦출 양은 소리가 시작될 때 한 번 정한다
 * (OBS는 70ms 미만의 타임스탬프 변화를 무시하고 소리를 이어 붙이므로 도중에 조금씩 바꿔도 소용없다).
 * 시계 차이나 영상 지연을 아직 모르면 0을 돌려주고, 그 소리는 버린다.
 */
static uint64_t audio_timestamp(struct phonecam_source *ctx, int64_t pts_us)
{
	if (!ctx->clock_valid || pts_us <= 0)
		return 0;
	if (!ctx->audio_delay_us) {
		if (ctx->last_latency_us <= 0)
			return 0;
		struct obs_video_info ovi;
		int64_t half_frame_us = 8000;
		if (obs_get_video_info(&ovi) && ovi.fps_num)
			half_frame_us = 500000LL * ovi.fps_den / ovi.fps_num;
		ctx->audio_delay_us = ctx->last_latency_us + half_frame_us;
		blog(LOG_INFO, "[phonecam] audio starts, played %lld ms after capture to match the video",
		     (long long)(ctx->audio_delay_us / 1000));
	}

	int64_t now_us = (int64_t)(os_gettime_ns() / 1000);
	int64_t captured_us = pts_us - ctx->clock_offset_us;
	int64_t latency = now_us - captured_us;
	if (latency > 0 && latency < MAX_LATENCY_US) {
		ctx->window_audio_latency_us += latency;
		ctx->window_audio_count++;
	}

	int64_t play_us = captured_us + ctx->audio_delay_us;
	if (play_us < now_us - MAX_AUDIO_LATE_US || play_us > now_us + MAX_AUDIO_EARLY_US) {
		ctx->window_audio_late++;
		return 0;
	}
	return (uint64_t)play_us * 1000;
}

static void handle_audio(struct phonecam_source *ctx, const struct pcam_packet_header *header)
{
	if (header->flags & PCAM_FLAG_CONFIG) {
		/* 폰에서 마이크를 켤 때마다 온다. 늦출 양도 그때의 영상 지연으로 다시 정한다. */
		pcam_audio_destroy(ctx->audio);
		ctx->audio = pcam_audio_create(ctx->buffer, header->size);
		ctx->audio_delay_us = 0;

		pthread_mutex_lock(&ctx->mutex);
		ctx->audio_desc[0] = 0;
		ctx->audio_channels = 0;
		if (ctx->audio)
			pcam_audio_describe(ctx->audio, ctx->audio_desc, sizeof(ctx->audio_desc), &ctx->audio_channels);
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}
	if (!ctx->audio)
		return;
	uint64_t timestamp = audio_timestamp(ctx, header->pts_us);
	if (timestamp)
		pcam_audio_output(ctx->audio, ctx->buffer, header->size, timestamp, ctx->source);
}

static void output_frame(void *param, const AVFrame *frame)
{
	struct phonecam_source *ctx = param;
	struct obs_source_frame out = {0};

	switch (frame->format) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		out.format = VIDEO_FORMAT_I420;
		break;
	case AV_PIX_FMT_NV12:
		out.format = VIDEO_FORMAT_NV12;
		break;
	case AV_PIX_FMT_YUV420P10LE:
		out.format = VIDEO_FORMAT_I010;
		break;
	case AV_PIX_FMT_P010LE:
		out.format = VIDEO_FORMAT_P010;
		break;
	default:
		if (!ctx->warned_format) {
			blog(LOG_WARNING, "[phonecam] unsupported pixel format %d", frame->format);
			ctx->warned_format = true;
		}
		return;
	}

	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		out.data[i] = frame->data[i];
		out.linesize[i] = (uint32_t)frame->linesize[i];
	}
	out.width = (uint32_t)frame->width;
	out.height = (uint32_t)frame->height;
	/* 받은 즉시 보여 주므로 지금 시각. 소리도 OBS 시계로 맞춰 넣는다 (audio_timestamp). */
	out.timestamp = os_gettime_ns();

	bool full_range = frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P;
	out.full_range = full_range;
	video_format_get_parameters_for_format(colorspace_for(frame), full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL,
					       out.format, out.color_matrix, out.color_range_min,
					       out.color_range_max);

	obs_source_output_video(ctx->source, &out);
	update_stats(ctx, frame);

	if (!ctx->logged_first_frame) {
		blog(LOG_INFO, "[phonecam] first frame decoded: %ux%u, pixel format %d", out.width, out.height,
		     frame->format);
		ctx->logged_first_frame = true;
	}
}

/* ------------------------------------------------------------------------- */
/* 수신 스레드 */

static bool recv_into_buffer(struct phonecam_source *ctx, pcam_socket_t sock, size_t size)
{
	if (size > ctx->buffer_capacity) {
		ctx->buffer = brealloc(ctx->buffer, size);
		ctx->buffer_capacity = size;
	}
	return size == 0 || pcam_recv_all(sock, ctx->buffer, size);
}

static const char *codec_name(uint32_t codec)
{
	return codec == PCAM_CODEC_H265 ? "HEVC" : "H.264";
}

static void reset_session_state(struct phonecam_source *ctx)
{
	pthread_mutex_lock(&ctx->mutex);
	ctx->have_phone_state = false;
	memset(&ctx->caps, 0, sizeof(ctx->caps));
	memset(&ctx->stats, 0, sizeof(ctx->stats));
	ctx->device_name[0] = 0;
	ctx->latency_ms = -1.0;
	ctx->decode_fps = 0.0;
	ctx->hw_decoding = false;
	ctx->have_overlays = false;
	ctx->stream_codec = 0;
	ctx->heat_level = 0;
	ctx->free_limited = false;
	ctx->send_mbps = 0.0;
	ctx->audio_desc[0] = 0;
	ctx->audio_channels = 0;
	pthread_mutex_unlock(&ctx->mutex);

	ctx->logged_first_frame = false;
	ctx->last_latency_us = 0;
	ctx->audio_delay_us = 0;
	ctx->window_audio_latency_us = 0;
	ctx->window_audio_count = 0;
	ctx->window_audio_late = 0;
	ctx->clock_valid = false;
	ctx->last_ping_ns = 0;
	ctx->ping_sent_us = 0;
	ctx->last_view_ns = 0;
	ctx->sent_flip = -1;
	ctx->last_obs_refresh_ns = 0;
	ctx->sent_obs_version = -1;
	ctx->sent_phone_scene[0] = 0;
	ctx->obs_tick = false;
	ctx->window_start_ns = 0;
	ctx->window_frames = 0;
	ctx->window_latency_us = 0;
	ctx->window_latency_count = 0;
	ctx->stats_windows = 0;
}

/* ------------------------------------------------------------------------- */
/* PC 승인 (PROTOCOL.md 1.1) */

/*
 * 승인 결과를 남기고, 바뀌었으면 속성 창을 다시 그린다 (코드 입력 칸을 보이거나 숨김).
 * 틀린 코드·잠김 안내는 새 코드를 넣을 때까지 남긴다. 그 사이에 다시 접속하면 폰이 pair_required로만 답하기 때문.
 */
static void set_auth(struct phonecam_source *ctx, enum auth_status status, int attempts_left, int retry_s)
{
	pthread_mutex_lock(&ctx->mutex);
	bool sticky = status == AUTH_PAIR_REQUIRED && (ctx->auth == AUTH_BAD_CODE || ctx->auth == AUTH_LOCKED) &&
		      ctx->rejected_code[0] && strcmp(ctx->rejected_code, ctx->pair_code) == 0;
	/* 잠김의 남은 시간은 접속할 때마다 줄어드는데, 그때마다 다시 그리면 코드 칸에 입력하던 것이 끊긴다. */
	bool changed = !sticky && (ctx->auth != status || ctx->auth_attempts_left != attempts_left);
	if (!sticky) {
		ctx->auth = status;
		ctx->auth_attempts_left = attempts_left;
		ctx->auth_retry_s = retry_s;
	}
	pthread_mutex_unlock(&ctx->mutex);
	if (changed)
		obs_queue_task(OBS_TASK_UI, refresh_properties_task, obs_source_get_weak_source(ctx->source), false);
}

/* 승인되면 입력한 코드는 쓸모없으므로 지운다 (UI 스레드). */
static void clear_code_task(void *param)
{
	obs_weak_source_t *weak = param;
	obs_source_t *source = obs_weak_source_get_source(weak);
	obs_weak_source_release(weak);
	if (!source)
		return;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_string(settings, S_PAIR_CODE, "");
	obs_data_release(settings);
	obs_source_update_properties(source);
	obs_source_release(source);
}

/* 승인 단계의 제어 메시지 하나. header_buf는 이미 받은 16바이트. 형식이 틀리면 NULL. */
static obs_data_t *recv_json(struct phonecam_source *ctx, pcam_socket_t sock, const uint8_t header_buf[PCAM_HEADER_SIZE])
{
	struct pcam_packet_header header;
	pcam_header_decode(header_buf, &header);
	if (header.stream_id != PCAM_STREAM_CONTROL || header.size == 0 || header.size > MAX_AUTH_MESSAGE_BYTES ||
	    !recv_into_buffer(ctx, sock, header.size))
		return NULL;
	char *json = bstrdup_n((const char *)ctx->buffer, header.size);
	obs_data_t *msg = obs_data_create_from_json(json);
	bfree(json);
	return msg;
}

/*
 * Hello 다음의 승인 단계. 폰이 받아 주면 true (이어서 Handshake가 온다).
 * 거절하면 이유를 ctx->auth에 남기고 false. 코드로 새로 받은 토큰은 폰 ID별로 저장한다.
 */
static bool authenticate(struct phonecam_source *ctx, pcam_socket_t sock)
{
	uint8_t header_buf[PCAM_HEADER_SIZE];
	if (!pcam_recv_all(sock, header_buf, 4)) {
		blog(LOG_WARNING, "[phonecam] the phone closed the connection without answering (old app version?)");
		set_auth(ctx, AUTH_CLOSED, 0, 0);
		return false;
	}
	if (pcam_get_u32(header_buf) == PCAM_MAGIC) {
		/* 버전이 다른 폰이 자기 버전만 담은 Handshake로 답했다. */
		uint8_t rest[PCAM_HANDSHAKE_SIZE - 4];
		uint16_t version = pcam_recv_all(sock, rest, sizeof(rest)) ? pcam_get_u16(rest) : 0;
		blog(LOG_WARNING, "[phonecam] the phone app uses protocol v%u but this plugin uses v%d: update both", version,
		     PCAM_VERSION);
		pthread_mutex_lock(&ctx->mutex);
		ctx->phone_version = version;
		pthread_mutex_unlock(&ctx->mutex);
		set_auth(ctx, AUTH_VERSION, 0, 0);
		return false;
	}

	obs_data_t *challenge = NULL;
	if (pcam_recv_all(sock, header_buf + 4, PCAM_HEADER_SIZE - 4))
		challenge = recv_json(ctx, sock, header_buf);
	uint8_t nonce[PCAM_NONCE_BYTES];
	bool valid = challenge && strcmp(obs_data_get_string(challenge, "type"), "auth_challenge") == 0 &&
		     pcam_hex_decode(obs_data_get_string(challenge, "nonce"), nonce, sizeof(nonce));
	char phone_id[64];
	snprintf(phone_id, sizeof(phone_id), "%s", challenge ? obs_data_get_string(challenge, "phone_id") : "");
	obs_data_release(challenge);
	if (!valid) {
		blog(LOG_WARNING, "[phonecam] the phone did not send an auth challenge");
		set_auth(ctx, AUTH_CLOSED, 0, 0);
		return false;
	}

	char pc_id[64];
	char pc_name[64];
	pcam_pairing_pc_id(pc_id, sizeof(pc_id));
	pcam_pairing_pc_name(pc_name, sizeof(pc_name));
	uint8_t token[PCAM_TOKEN_BYTES];
	bool has_token = pcam_pairing_token(phone_id, token);
	char code[PCAM_PAIR_CODE_LEN + 1];
	pthread_mutex_lock(&ctx->mutex);
	snprintf(code, sizeof(code), "%s", strcmp(ctx->pair_code, ctx->rejected_code) != 0 ? ctx->pair_code : "");
	pthread_mutex_unlock(&ctx->mutex);

	obs_data_t *auth = obs_data_create();
	obs_data_set_string(auth, "type", "auth");
	obs_data_set_string(auth, "pc_id", pc_id);
	obs_data_set_string(auth, "pc_name", pc_name);
	uint8_t proof[PCAM_PROOF_BYTES];
	if (has_token && pcam_pairing_proof(token, nonce, sizeof(nonce), proof)) {
		char hex[PCAM_PROOF_BYTES * 2 + 1];
		pcam_hex_encode(proof, sizeof(proof), hex);
		obs_data_set_string(auth, "proof", hex);
	}
	if (code[0])
		obs_data_set_string(auth, "code", code);
	bool sent = send_json(sock, auth);
	obs_data_release(auth);

	obs_data_t *result = NULL;
	if (sent && pcam_recv_all(sock, header_buf, sizeof(header_buf)))
		result = recv_json(ctx, sock, header_buf);
	if (!result) {
		set_auth(ctx, AUTH_CLOSED, 0, 0);
		return false;
	}

	bool ok = obs_data_get_bool(result, "ok");
	const char *reason = obs_data_get_string(result, "reason");
	if (ok) {
		uint8_t new_token[PCAM_TOKEN_BYTES];
		if (pcam_hex_decode(obs_data_get_string(result, "token"), new_token, sizeof(new_token))) {
			pcam_pairing_save_token(phone_id, new_token);
			blog(LOG_INFO, "[phonecam] the phone accepted the code; this PC is now paired");
			pthread_mutex_lock(&ctx->mutex);
			ctx->pair_code[0] = 0;
			ctx->rejected_code[0] = 0;
			pthread_mutex_unlock(&ctx->mutex);
			obs_queue_task(OBS_TASK_UI, clear_code_task, obs_source_get_weak_source(ctx->source), false);
		}
		set_auth(ctx, AUTH_NONE, 0, 0);
	} else if (strcmp(reason, "pair_required") == 0) {
		if (has_token) {
			/* 폰에서 이 PC를 지웠다. 가진 토큰은 다시 쓸 수 없다. */
			pcam_pairing_forget(phone_id);
			blog(LOG_INFO, "[phonecam] the phone no longer trusts this PC; enter the code shown on the phone");
		}
		set_auth(ctx, AUTH_PAIR_REQUIRED, 0, 0);
	} else {
		bool locked = strcmp(reason, "locked") == 0;
		blog(LOG_WARNING, "[phonecam] the phone rejected the code (%s)", reason);
		if (code[0]) {
			pthread_mutex_lock(&ctx->mutex);
			snprintf(ctx->rejected_code, sizeof(ctx->rejected_code), "%s", code);
			pthread_mutex_unlock(&ctx->mutex);
		}
		set_auth(ctx, locked ? AUTH_LOCKED : AUTH_BAD_CODE, (int)obs_data_get_int(result, "attempts_left"),
			 (int)obs_data_get_int(result, "retry_s"));
	}
	obs_data_release(result);
	return ok;
}

static void receive_stream(struct phonecam_source *ctx, pcam_socket_t sock, const struct connection_settings *settings)
{
	uint8_t hello_buf[PCAM_HELLO_SIZE];
	pcam_hello_encode(&settings->hello, hello_buf);
	if (!pcam_send_all(sock, hello_buf, sizeof(hello_buf)) || !authenticate(ctx, sock))
		return;
	struct pcam_writer *writer = pcam_writer_start(sock);
	if (!writer)
		return;
	pthread_mutex_lock(&ctx->mutex);
	ctx->writer = writer;
	ctx->sock_ready = true;
	pthread_mutex_unlock(&ctx->mutex);

	uint8_t handshake_buf[PCAM_HANDSHAKE_SIZE];
	struct pcam_handshake handshake;
	if (!pcam_recv_all(sock, handshake_buf, sizeof(handshake_buf)))
		return;
	if (!pcam_handshake_decode(handshake_buf, &handshake)) {
		blog(LOG_WARNING, "[phonecam] unexpected handshake: update the phone app and the plugin to the same version");
		return;
	}
	if (!recv_into_buffer(ctx, sock, handshake.name_len))
		return;

	char name[64] = {0};
	memcpy(name, ctx->buffer, handshake.name_len < sizeof(name) ? handshake.name_len : sizeof(name) - 1);
	blog(LOG_INFO, "[phonecam] streaming from '%s': %ux%u @ %u fps, %s, %s", name, handshake.width, handshake.height,
	     handshake.fps, codec_name(handshake.video_codec),
	     handshake.audio_codec == PCAM_AUDIO_OPUS ? "mic: Opus"
	     : handshake.audio_codec == PCAM_AUDIO_PCM ? "mic: PCM"
						       : "no mic permission");

	uint32_t decoder_codec = handshake.video_codec;
	struct pcam_decoder *decoder = pcam_decoder_create(decoder_codec, settings->hw_decode);
	if (!decoder)
		return;

	pthread_mutex_lock(&ctx->mutex);
	snprintf(ctx->device_name, sizeof(ctx->device_name), "%s", name);
	ctx->stream_width = handshake.width;
	ctx->stream_height = handshake.height;
	ctx->stream_fps = handshake.fps;
	ctx->stream_codec = decoder_codec;
	pthread_mutex_unlock(&ctx->mutex);

	uint8_t header_buf[PCAM_HEADER_SIZE];
	struct pcam_packet_header header;
	while (!os_atomic_load_bool(&ctx->stopping)) {
		if (!pcam_recv_all(sock, header_buf, sizeof(header_buf)))
			break;
		pcam_header_decode(header_buf, &header);
		if (header.size > PCAM_MAX_PACKET_SIZE) {
			blog(LOG_WARNING, "[phonecam] packet too large (%u bytes)", header.size);
			break;
		}
		if (!recv_into_buffer(ctx, sock, header.size))
			break;

		if (header.stream_id == PCAM_STREAM_VIDEO) {
			pcam_decoder_decode(decoder, ctx->buffer, header.size, header.pts_us,
					    (header.flags & PCAM_FLAG_CONFIG) != 0, output_frame, ctx);
			if (!ctx->hw_decoding && pcam_decoder_is_hw(decoder)) {
				pthread_mutex_lock(&ctx->mutex);
				ctx->hw_decoding = true;
				pthread_mutex_unlock(&ctx->mutex);
			}
		} else if (header.stream_id == PCAM_STREAM_AUDIO) {
			handle_audio(ctx, &header);
		} else if (header.stream_id == PCAM_STREAM_CONTROL) {
			handle_control(ctx, ctx->buffer, header.size);

			/* 폰이 코덱을 바꾸면 새 인코더의 첫 패킷보다 state가 먼저 오므로 여기서 디코더를 바꾼다. */
			pthread_mutex_lock(&ctx->mutex);
			uint32_t codec = ctx->stream_codec;
			pthread_mutex_unlock(&ctx->mutex);
			if (codec && codec != decoder_codec) {
				blog(LOG_INFO, "[phonecam] codec changed to %s", codec_name(codec));
				pcam_decoder_destroy(decoder);
				decoder_codec = codec;
				decoder = pcam_decoder_create(decoder_codec, settings->hw_decode);
				if (!decoder)
					break;
			}
		}
		maybe_send_ping(ctx);
		maybe_send_view(ctx);
		maybe_send_obs_state(ctx);
	}

	pcam_decoder_destroy(decoder);
	pcam_audio_destroy(ctx->audio);
	ctx->audio = NULL;
	blog(LOG_INFO, "[phonecam] stream from '%s' ended", name);
}

/* 폰에 접속해 스트림을 받는다. 폰에 접속은 됐으면 true (끊긴 이유와 상관없이). */
static bool run_connection(struct phonecam_source *ctx, const struct connection_settings *settings,
			   long generation)
{
	bool usb = settings->type == CONNECTION_USB;
	const char *host = usb ? "127.0.0.1" : settings->host;
	int port = settings->port;
	struct pcam_found_phone phone;
	if (!usb && (!host || !*host)) {
		/* 주소를 비워 두면 같은 네트워크에서 폰을 찾아 그 폰에 접속하고, 주소를 설정에 적어 둔다. */
		if (!search_phones(ctx) || !pick_found(ctx, true, &phone))
			return false;
		adopt_phone(ctx, &phone);
		host = phone.host;
		port = phone.port;
	}
	if (usb && !pcam_adb_forward(settings->adb_path, settings->port))
		return false;

	pcam_socket_t sock = pcam_connect(host, port, CONNECT_TIMEOUT_MS, RECV_TIMEOUT_MS);
	if (sock == PCAM_INVALID_SOCKET) {
		set_auth(ctx, AUTH_NONE, 0, 0); /* 폰에 닿지 않으면 승인 안내 대신 "연결 안 됨"을 보여 준다 */
		return false;
	}

	reset_session_state(ctx);

	/* 접속하는 동안 설정이 바뀌었으면 이 연결은 버린다. */
	pthread_mutex_lock(&ctx->mutex);
	bool current = ctx->generation == generation && !os_atomic_load_bool(&ctx->stopping);
	if (current) {
		ctx->sock = sock;
		ctx->sock_ready = false;
	}
	pthread_mutex_unlock(&ctx->mutex);

	if (current)
		receive_stream(ctx, sock, settings);

	/* 모니터 인코더가 더 넣지 않게 먼저 멈추고, 그다음 보내기 스레드를 멈춘다. */
	pthread_mutex_lock(&ctx->mutex);
	struct pcam_monitor *monitor = ctx->monitor;
	ctx->monitor = NULL;
	pthread_mutex_unlock(&ctx->mutex);
	pcam_monitor_stop(monitor);

	pthread_mutex_lock(&ctx->mutex);
	struct pcam_writer *writer = ctx->writer;
	ctx->writer = NULL;
	ctx->sock_ready = false;
	pthread_mutex_unlock(&ctx->mutex);
	pcam_writer_stop(writer);

	pthread_mutex_lock(&ctx->mutex);
	ctx->sock = PCAM_INVALID_SOCKET;
	ctx->sock_ready = false;
	ctx->have_phone_state = false;
	ctx->have_overlays = false;
	pthread_mutex_unlock(&ctx->mutex);
	pcam_close(sock);
	return true;
}

/* 저장된 주소로 계속 접속하지 못하면 같은 폰(ID)이 다른 주소에 있는지 찾아본다 (공유기가 IP를 바꾼 경우). */
static void rediscover(struct phonecam_source *ctx, const struct connection_settings *settings)
{
	struct pcam_found_phone phone;
	if (!search_phones(ctx) || !pick_found(ctx, false, &phone))
		return;
	if (strcmp(phone.host, settings->host) == 0 && phone.port == settings->port)
		return; /* 같은 주소에 있다. 앱이 송출을 멈췄거나 다른 PC와 연결 중일 수 있다 */
	blog(LOG_INFO, "[phonecam] phone moved from %s:%d", settings->host, settings->port);
	adopt_phone(ctx, &phone);
}

static void *phonecam_thread(void *data)
{
	struct phonecam_source *ctx = data;
	os_set_thread_name("phonecam");

	while (!os_atomic_load_bool(&ctx->stopping)) {
		struct connection_settings settings;
		pthread_mutex_lock(&ctx->mutex);
		settings_copy(&settings, &ctx->settings);
		/* 접속할 때는 OBS에 저장된 송출 형식과 카메라 방향을 요청한다. */
		settings.hello.width = ctx->wanted.width;
		settings.hello.height = ctx->wanted.height;
		settings.hello.fps = (uint16_t)ctx->wanted.fps;
		settings.hello.codec = ctx->wanted.codec;
		settings.hello.cbr = ctx->wanted.cbr;
		settings.hello.front = ctx->wanted.front;
		settings.hello.mic = ctx->wanted.mic;
		settings.hello.bitrate = (uint32_t)ctx->wanted.bitrate_mbps * 1000000u;
		long generation = ctx->generation;
		pthread_mutex_unlock(&ctx->mutex);

		bool connected = run_connection(ctx, &settings, generation);
		ctx->connect_failures = connected ? 0 : ctx->connect_failures + 1;
		if (ctx->connect_failures >= REDISCOVER_AFTER_FAILURES && settings.type == CONNECTION_WIFI &&
		    settings.host && *settings.host && !os_atomic_load_bool(&ctx->stopping)) {
			ctx->connect_failures = 0;
			rediscover(ctx, &settings);
		}
		settings_free(&settings);

		/* 연결이 끊기면 마지막 화면을 지운다. */
		obs_source_output_video(ctx->source, NULL);
		pthread_mutex_lock(&ctx->mutex);
		bool refused = ctx->auth != AUTH_NONE;
		pthread_mutex_unlock(&ctx->mutex);
		os_event_timedwait(ctx->wake, refused ? AUTH_RETRY_DELAY_MS : RETRY_DELAY_MS);
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* 소스 */

static const char *phonecam_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return T_("PhoneCam");
}

static void *phonecam_create(obs_data_t *settings, obs_source_t *source)
{
	struct phonecam_source *ctx = bzalloc(sizeof(*ctx));
	ctx->source = source;
	ctx->sock = PCAM_INVALID_SOCKET;
	ctx->latency_ms = -1.0;

	if (pthread_mutex_init(&ctx->mutex, NULL) != 0 || os_event_init(&ctx->wake, OS_EVENT_TYPE_AUTO) != 0) {
		bfree(ctx);
		return NULL;
	}

	phonecam_update(ctx, settings);
	ctx->thread_created = pthread_create(&ctx->thread, NULL, phonecam_thread, ctx) == 0;
	return ctx;
}

static void phonecam_destroy(void *data)
{
	struct phonecam_source *ctx = data;

	os_atomic_set_bool(&ctx->stopping, true);
	pthread_mutex_lock(&ctx->mutex);
	pcam_shutdown(ctx->sock);
	pthread_mutex_unlock(&ctx->mutex);
	os_event_signal(ctx->wake);
	if (ctx->thread_created)
		pthread_join(ctx->thread, NULL);
	if (ctx->find_thread_created)
		pthread_join(ctx->find_thread, NULL);

	settings_free(&ctx->settings);
	pcam_remote_mode_free(&ctx->remote_mode);
	os_event_destroy(ctx->wake);
	pthread_mutex_destroy(&ctx->mutex);
	bfree(ctx->buffer);
	bfree(ctx);
}

/* Android SDK 위치(Android Studio 기본 위치, C:\Android\Sdk)에 adb가 있으면 그것을, 없으면 PATH의 adb를 쓴다. */
static char *default_adb_path(void)
{
#ifdef _WIN32
	const wchar_t *local_app_data = _wgetenv(L"LOCALAPPDATA");
	if (local_app_data) {
		char *base = NULL;
		os_wcs_to_utf8_ptr(local_app_data, 0, &base);
		struct dstr path = {0};
		dstr_printf(&path, "%s\\Android\\Sdk\\platform-tools\\adb.exe", base ? base : "");
		bfree(base);
		if (os_file_exists(path.array))
			return path.array;
		dstr_free(&path);
	}
	if (os_file_exists("C:\\Android\\Sdk\\platform-tools\\adb.exe"))
		return bstrdup("C:\\Android\\Sdk\\platform-tools\\adb.exe");
#endif
	return bstrdup("adb");
}

static void phonecam_defaults(obs_data_t *settings)
{
	char *adb_path = default_adb_path();

	obs_data_set_default_int(settings, S_CONNECTION, CONNECTION_WIFI);
	obs_data_set_default_string(settings, S_HOST, "");
	obs_data_set_default_int(settings, S_PORT, PCAM_DEFAULT_PORT);
	obs_data_set_default_string(settings, S_DEVICE_ID, "");
	obs_data_set_default_string(settings, S_FOUND, "");
	obs_data_set_default_string(settings, S_PAIR_CODE, "");
	obs_data_set_default_string(settings, S_ADB_PATH, adb_path);

	obs_data_set_default_string(settings, S_RESOLUTION, "1920x1080");
	obs_data_set_default_int(settings, S_FPS, 30);
	obs_data_set_default_int(settings, S_CODEC, PCAM_CODEC_H264);
	obs_data_set_default_string(settings, S_RATE_CONTROL, "cbr");
	obs_data_set_default_int(settings, S_BITRATE, 12);
	obs_data_set_default_bool(settings, S_HW_DECODE, true);
	obs_data_set_default_bool(settings, S_LOW_LATENCY, true);

	obs_data_set_default_int(settings, S_FACING, 0);
	obs_data_set_default_double(settings, S_ZOOM, 1.0);
	obs_data_set_default_string(settings, S_FOCUS_MODE, "continuous");
	obs_data_set_default_double(settings, S_FOCUS_DISTANCE, 0.0);
	obs_data_set_default_double(settings, S_EV, 0.0);
	obs_data_set_default_string(settings, S_WB, "auto");
	obs_data_set_default_bool(settings, S_MIC, true);
	obs_data_set_default_int(settings, S_BEAUTY_SMOOTH, 0);
	obs_data_set_default_int(settings, S_BEAUTY_BRIGHT, 0);
	obs_data_set_default_string(settings, S_PHONE_SCENE, "");
	obs_data_set_default_int(settings, S_MONITOR_TRACK, 6);

	obs_data_set_default_string(settings, S_CHAT_URL, "");
	obs_data_set_default_string(settings, S_ALERT_URL, "");
	obs_data_set_default_bool(settings, S_CHAT_STREAM, false);
	obs_data_set_default_bool(settings, S_ALERT_STREAM, false);

	bfree(adb_path);
}

static bool connection_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	bool usb = obs_data_get_int(settings, S_CONNECTION) == CONNECTION_USB;
	obs_property_set_visible(obs_properties_get(props, S_HOST), !usb);
	obs_property_set_visible(obs_properties_get(props, S_FIND), !usb);
	obs_property_set_visible(obs_properties_get(props, S_FOUND), !usb);
	obs_property_set_visible(obs_properties_get(props, S_ADB_PATH), usb);
	return true;
}

static bool find_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	start_find(data);
	return true; /* "찾는 중..."을 바로 보여 준다. 다 찾으면 다시 새로 그린다 */
}

/* 찾은 폰 목록에서 고르면 그 주소를 넣는다. 설정이 바뀌므로 그 폰으로 다시 접속한다. */
static bool found_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	const char *value = obs_data_get_string(settings, S_FOUND);
	char host[64] = {0};
	char id[64] = {0};
	int port = 0;
	if (sscanf(value, "%63[^:]:%d|%63s", host, &port, id) != 3)
		return false;
	obs_data_set_string(settings, S_HOST, host);
	obs_data_set_int(settings, S_PORT, port);
	obs_data_set_string(settings, S_DEVICE_ID, id);
	obs_data_set_string(settings, S_FOUND, "");
	return true;
}

static void add_found_list(struct phonecam_source *ctx, obs_properties_t *props)
{
	obs_property_t *p = obs_properties_add_list(props, S_FOUND, T_("FoundPhones"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(p, found_modified);

	pthread_mutex_lock(&ctx->mutex);
	if (os_atomic_load_long(&ctx->finding)) {
		obs_property_list_add_string(p, T_("Found.Searching"), "");
	} else if (!ctx->searched) {
		obs_property_list_add_string(p, T_("Found.NotYet"), "");
	} else if (!ctx->found_count) {
		obs_property_list_add_string(p, T_("Found.None"), "");
	} else {
		struct dstr label = {0};
		dstr_printf(&label, T_("Found.Choose"), (int)ctx->found_count);
		obs_property_list_add_string(p, label.array, "");
		for (size_t i = 0; i < ctx->found_count; i++) {
			const struct pcam_found_phone *phone = &ctx->found[i];
			struct dstr value = {0};
			dstr_printf(&label, "%s (%s)", phone->name, phone->host);
			if (phone->busy) {
				dstr_cat(&label, " · ");
				dstr_cat(&label, T_("Found.Busy"));
			}
			dstr_printf(&value, "%s:%d|%s", phone->host, phone->port, phone->id);
			obs_property_list_add_string(p, label.array, value.array);
			dstr_free(&value);
		}
		dstr_free(&label);
	}
	pthread_mutex_unlock(&ctx->mutex);
}

static bool focus_mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	bool manual = strcmp(obs_data_get_string(settings, S_FOCUS_MODE), "manual") == 0;
	obs_property_set_visible(obs_properties_get(props, S_FOCUS_DISTANCE), manual);
	return true;
}

static bool reconnect_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct phonecam_source *ctx = data;

	pthread_mutex_lock(&ctx->mutex);
	ctx->generation++;
	pcam_shutdown(ctx->sock);
	pthread_mutex_unlock(&ctx->mutex);
	os_event_signal(ctx->wake);
	return false;
}

static bool focus_center_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct phonecam_source *ctx = data;

	obs_data_t *msg = obs_data_create();
	obs_data_set_string(msg, "type", "focus_at");
	obs_data_set_double(msg, "x", 0.5);
	obs_data_set_double(msg, "y", 0.5);
	send_control(ctx, msg);
	obs_data_release(msg);
	return false;
}

static void add_status(struct phonecam_source *ctx, obs_properties_t *props)
{
	struct dstr text = {0};

	pthread_mutex_lock(&ctx->mutex);
	bool connected = ctx->have_phone_state;
	bool searching = ctx->settings.type == CONNECTION_WIFI && (!ctx->settings.host || !*ctx->settings.host);
	if (!connected) {
		switch (ctx->auth) {
		case AUTH_PAIR_REQUIRED:
			dstr_copy(&text, T_("Status.PairRequired"));
			break;
		case AUTH_BAD_CODE:
			dstr_printf(&text, T_("Status.BadCode"), ctx->auth_attempts_left);
			break;
		case AUTH_LOCKED:
			dstr_printf(&text, T_("Status.Locked"), ctx->auth_retry_s);
			break;
		case AUTH_VERSION:
			dstr_printf(&text, T_("Status.Version"), (int)ctx->phone_version, PCAM_VERSION);
			break;
		case AUTH_CLOSED:
			dstr_copy(&text, T_("Status.Closed"));
			break;
		default:
			dstr_copy(&text, T_(searching ? "Status.Searching" : "Status.Disconnected"));
		}
	} else {
		dstr_printf(&text, T_("Status.Stream"), ctx->device_name, ctx->stream_width, ctx->stream_height,
			    ctx->stream_fps, ctx->hw_decoding ? "GPU" : "CPU");
		dstr_cat(&text, "\n");
		if (ctx->free_limited) {
			dstr_cat(&text, T_("Status.FreeLimit"));
			dstr_cat(&text, "\n");
		}
		if (ctx->heat_level > 0) {
			dstr_catf(&text, T_("Status.Heat"), ctx->heat_level, ctx->send_mbps);
			dstr_cat(&text, "\n");
		}
		if (ctx->latency_ms >= 0)
			dstr_catf(&text, T_("Status.Latency"), (int)(ctx->latency_ms + 0.5));
		else
			dstr_cat(&text, T_("Status.LatencyUnknown"));
		if (ctx->stats.valid) {
			dstr_cat(&text, "\n");
			dstr_catf(&text, T_("Status.Phone"), ctx->stats.fps, ctx->stats.bitrate / 1e6,
				  ctx->stats.dropped, ctx->stats.encode_ms, ctx->stats.battery, ctx->stats.temp);
		}
		dstr_cat(&text, "\n");
		if (!ctx->phone.mic)
			dstr_cat(&text, T_("Status.AudioOff"));
		else if (!ctx->audio_desc[0])
			dstr_cat(&text, T_("Status.AudioWaiting"));
		else
			dstr_catf(&text, T_("Status.Audio"), ctx->audio_desc,
				  T_(ctx->audio_channels == 1 ? "Audio.Mono" : "Audio.Stereo"));
	}
	pthread_mutex_unlock(&ctx->mutex);

	obs_properties_add_text(props, S_STATUS, text.array, OBS_TEXT_INFO);
	dstr_free(&text);
}

static obs_properties_t *connection_group(struct phonecam_source *ctx)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, S_CONNECTION, T_("Connection"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Connection.WiFi"), CONNECTION_WIFI);
	obs_property_list_add_int(p, T_("Connection.USB"), CONNECTION_USB);
	obs_property_set_modified_callback(p, connection_modified);

	obs_properties_add_text(props, S_HOST, T_("Host"), OBS_TEXT_DEFAULT);
	obs_properties_add_button2(props, S_FIND, T_("FindPhone"), find_clicked, ctx);
	add_found_list(ctx, props);
	obs_properties_add_path(props, S_ADB_PATH, T_("AdbPath"), OBS_PATH_FILE, "adb (adb.exe adb);;*.*", NULL);
	obs_properties_add_int(props, S_PORT, T_("Port"), 1024, 65535, 1);
	return props;
}

static obs_properties_t *quality_group(struct phonecam_source *ctx)
{
	static const char *const resolutions[] = {"1280x720", "1920x1080", "2560x1440", "3840x2160"};
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, S_RESOLUTION, T_("Resolution"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	bool listed = false;
	obs_data_t *settings = obs_source_get_settings(ctx->source);
	const char *current = obs_data_get_string(settings, S_RESOLUTION);
	for (size_t i = 0; i < sizeof(resolutions) / sizeof(resolutions[0]); i++) {
		obs_property_list_add_string(p, resolutions[i], resolutions[i]);
		listed = listed || strcmp(current, resolutions[i]) == 0;
	}
	/* 폰이 요청과 다른 크기를 골랐으면 그 값도 목록에 보여 준다. */
	if (!listed && *current)
		obs_property_list_add_string(p, current, current);
	obs_data_release(settings);

	p = obs_properties_add_list(props, S_FPS, T_("FPS"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "24", 24);
	obs_property_list_add_int(p, "30", 30);
	obs_property_list_add_int(p, "60", 60);

	p = obs_properties_add_list(props, S_CODEC, T_("Codec"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "H.264", PCAM_CODEC_H264);
	obs_property_list_add_int(p, T_("Codec.HEVC"), PCAM_CODEC_H265);

	p = obs_properties_add_list(props, S_RATE_CONTROL, T_("RateControl"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("RateControl.CBR"), "cbr");
	obs_property_list_add_string(p, T_("RateControl.VBR"), "vbr");

	p = obs_properties_add_int_slider(props, S_BITRATE, T_("Bitrate"), 2, 80, 1);
	obs_property_int_set_suffix(p, " Mbps");

	obs_properties_add_bool(props, S_HW_DECODE, T_("HwDecode"));
	obs_properties_add_bool(props, S_LOW_LATENCY, T_("LowLatency"));
	return props;
}

static obs_properties_t *camera_group(struct phonecam_source *ctx, const struct pcam_caps *caps)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, S_FACING, T_("Camera"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Camera.Back"), 0);
	obs_property_list_add_int(p, T_("Camera.Front"), 1);

	/* 범위는 폰에서 지금 열린 카메라 기준. 접속 전에는 일반적인 값으로 보여 준다. */
	p = obs_properties_add_float_slider(props, S_ZOOM, T_("Zoom"), caps->valid ? caps->zoom_min : 1.0,
					    caps->valid ? caps->zoom_max : 10.0, 0.1);
	obs_property_float_set_suffix(p, "x");

	p = obs_properties_add_list(props, S_FOCUS_MODE, T_("FocusMode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("FocusMode.Continuous"), "continuous");
	obs_property_list_add_string(p, T_("FocusMode.Auto"), "auto");
	if (!caps->valid || caps->manual_focus)
		obs_property_list_add_string(p, T_("FocusMode.Manual"), "manual");
	obs_property_set_modified_callback(p, focus_mode_modified);

	obs_properties_add_float_slider(props, S_FOCUS_DISTANCE, T_("FocusDistance"), 0.0, 1.0, 0.01);
	obs_properties_add_button2(props, S_FOCUS_CENTER, T_("FocusCenter"), focus_center_clicked, ctx);

	p = obs_properties_add_float_slider(props, S_EV, T_("Exposure"), caps->valid ? caps->ev_min : -2.0,
					    caps->valid ? caps->ev_max : 2.0, caps->valid ? caps->ev_step : 0.1);
	obs_property_float_set_suffix(p, " EV");

	p = obs_properties_add_list(props, S_WB, T_("WhiteBalance"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (size_t i = 0; i < PCAM_WB_COUNT; i++) {
		if (caps->valid && !strstr(caps->wb_modes, pcam_wb_modes[i]))
			continue;
		struct dstr key = {0};
		dstr_printf(&key, "WB.%s", pcam_wb_modes[i]);
		obs_property_list_add_string(p, T_(key.array), pcam_wb_modes[i]);
		dstr_free(&key);
	}

	obs_properties_add_bool(props, S_AE_LOCK, T_("AeLock"));
	obs_properties_add_bool(props, S_AWB_LOCK, T_("AwbLock"));
	p = obs_properties_add_bool(props, S_TORCH, T_("Torch"));
	obs_property_set_enabled(p, !caps->valid || caps->torch);
	p = obs_properties_add_bool(props, S_STABILIZATION, T_("Stabilization"));
	obs_property_set_enabled(p, !caps->valid || caps->stabilization);
	return props;
}

static obs_properties_t *overlay_group(void)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "overlay_help", T_("Overlay.Help"), OBS_TEXT_INFO);
	obs_properties_add_text(props, S_CHAT_URL, T_("Overlay.ChatUrl"), OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, S_CHAT_STREAM, T_("Overlay.ChatStream"));
	obs_properties_add_text(props, S_ALERT_URL, T_("Overlay.AlertUrl"), OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, S_ALERT_STREAM, T_("Overlay.AlertStream"));
	return props;
}

static obs_properties_t *beauty_group(void)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_int_slider(props, S_BEAUTY_SMOOTH, T_("Beauty.Smooth"), 0, 100, 1);
	obs_property_int_set_suffix(p, " %");
	p = obs_properties_add_int_slider(props, S_BEAUTY_BRIGHT, T_("Beauty.Bright"), 0, 100, 1);
	obs_property_int_set_suffix(p, " %");
	obs_properties_add_text(props, "beauty_help", T_("Beauty.Help"), OBS_TEXT_INFO);
	return props;
}

static obs_properties_t *audio_group(void)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_bool(props, S_MIC, T_("Mic"));
	obs_properties_add_text(props, "audio_help", T_("Audio.Help"), OBS_TEXT_INFO);
	return props;
}

static bool remove_mics_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct phonecam_source *ctx = data;
	obs_data_t *settings = obs_source_get_settings(ctx->source);
	int track = (int)obs_data_get_int(settings, S_MONITOR_TRACK);
	obs_data_release(settings);
	int removed = pcam_remote_remove_mics_from_track(track);
	snprintf(ctx->remove_mics_result, sizeof(ctx->remove_mics_result), T_("Remote.MicsRemoved"), removed, track);
	return true; /* 결과 문구를 보이게 속성을 다시 그린다 */
}

/* 폰에서 방송 진행: 폰 장면, 모니터 소리 트랙 */
static obs_properties_t *remote_group(struct phonecam_source *ctx)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "remote_help", T_("Remote.Help"), OBS_TEXT_INFO);

	obs_property_t *p = obs_properties_add_list(props, S_PHONE_SCENE, T_("Remote.PhoneScene"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Remote.PhoneScene.Auto"), "");
	char **names = obs_frontend_get_scene_names();
	for (char **name = names; name && *name; name++)
		obs_property_list_add_string(p, *name, *name);
	bfree(names);

	p = obs_properties_add_list(props, S_MONITOR_TRACK, T_("Remote.MonitorTrack"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	for (int track = 1; track <= 6; track++) {
		char label[64];
		snprintf(label, sizeof(label), T_("Remote.Track"), track);
		obs_property_list_add_int(p, label, track);
	}
	obs_properties_add_button2(props, S_MONITOR_REMOVE_MICS, T_("Remote.RemoveMics"), remove_mics_clicked, ctx);
	if (ctx->remove_mics_result[0])
		obs_properties_add_text(props, "remote_result", ctx->remove_mics_result, OBS_TEXT_INFO);
	return props;
}

static obs_properties_t *phonecam_properties(void *data)
{
	struct phonecam_source *ctx = data;
	obs_properties_t *props = obs_properties_create();

	pthread_mutex_lock(&ctx->mutex);
	struct pcam_caps caps = ctx->caps;
	bool need_code = ctx->auth == AUTH_PAIR_REQUIRED || ctx->auth == AUTH_BAD_CODE || ctx->auth == AUTH_LOCKED;
	pthread_mutex_unlock(&ctx->mutex);

	add_status(ctx, props);
	/* 폰이 코드를 요구할 때만 상태 바로 아래에 보인다. 승인되면 지우고 숨긴다. */
	if (need_code)
		obs_properties_add_text(props, S_PAIR_CODE, T_("PairCode"), OBS_TEXT_DEFAULT);
	obs_properties_add_button2(props, S_RECONNECT, T_("Reconnect"), reconnect_clicked, ctx);
	obs_properties_add_group(props, "group_camera", T_("Group.Camera"), OBS_GROUP_NORMAL, camera_group(ctx, &caps));
	obs_properties_add_group(props, "group_beauty", T_("Group.Beauty"), OBS_GROUP_NORMAL, beauty_group());
	obs_properties_add_group(props, "group_quality", T_("Group.Quality"), OBS_GROUP_NORMAL, quality_group(ctx));
	obs_properties_add_group(props, "group_audio", T_("Group.Audio"), OBS_GROUP_NORMAL, audio_group());
	obs_properties_add_group(props, "group_remote", T_("Group.Remote"), OBS_GROUP_NORMAL, remote_group(ctx));
	obs_properties_add_group(props, "group_overlay", T_("Group.Overlay"), OBS_GROUP_NORMAL, overlay_group());
	obs_properties_add_group(props, "group_connection", T_("Group.Connection"), OBS_GROUP_NORMAL,
				 connection_group(ctx));
	return props;
}

struct obs_source_info phonecam_source_info = {
	.id = "phonecam_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.icon_type = OBS_ICON_TYPE_CAMERA,
	.get_name = phonecam_get_name,
	.create = phonecam_create,
	.destroy = phonecam_destroy,
	.update = phonecam_update,
	.get_defaults = phonecam_defaults,
	.get_properties = phonecam_properties,
};
