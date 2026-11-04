#include "remote.h"

#include <math.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#define MIC_SOURCE_ID "wasapi_input_capture"
#define PHONECAM_SOURCE_ID "phonecam_source"
#define UUID_LEN 40

struct mic_info {
	char *name;
	char uuid[UUID_LEN];
	bool muted;
	bool phonecam;
};

struct saved_mute {
	char uuid[UUID_LEN];
	bool muted;
};

/* UI 스레드가 1초마다(요청이 있을 때) 모아 두는 OBS 상태 */
struct snapshot {
	DARRAY(char *) scenes;
	char *scene;
	bool studio;
	bool streaming;
	bool recording;
	long long stream_s;
	long long dropped;
	long long total;
	long long record_s;
	DARRAY(struct mic_info) mics;
};

static pthread_mutex_t snap_mutex;
static struct snapshot snap;
static volatile long snap_version;
static volatile long refresh_queued;

static void snapshot_free(struct snapshot *s)
{
	for (size_t i = 0; i < s->scenes.num; i++)
		bfree(s->scenes.array[i]);
	da_free(s->scenes);
	for (size_t i = 0; i < s->mics.num; i++)
		bfree(s->mics.array[i].name);
	da_free(s->mics);
	bfree(s->scene);
	memset(s, 0, sizeof(*s));
}

/* 시간과 버린 프레임 수는 빼고 비교한다 (그것만 바뀌면 방송 중일 때 1초마다 보내므로). */
static bool same_state(const struct snapshot *a, const struct snapshot *b)
{
	if (a->studio != b->studio || a->streaming != b->streaming || a->recording != b->recording)
		return false;
	if (strcmp(a->scene ? a->scene : "", b->scene ? b->scene : "") != 0)
		return false;
	if (a->scenes.num != b->scenes.num || a->mics.num != b->mics.num)
		return false;
	for (size_t i = 0; i < a->scenes.num; i++)
		if (strcmp(a->scenes.array[i], b->scenes.array[i]) != 0)
			return false;
	for (size_t i = 0; i < a->mics.num; i++) {
		const struct mic_info *x = &a->mics.array[i], *y = &b->mics.array[i];
		if (strcmp(x->name, y->name) != 0 || strcmp(x->uuid, y->uuid) != 0 || x->muted != y->muted)
			return false;
	}
	return true;
}

/* ------------------------------------------------------------------------- */
/* 마이크 찾기 (어느 스레드에서나) */

static bool is_mic(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	return id && (strcmp(id, MIC_SOURCE_ID) == 0 || strcmp(id, PHONECAM_SOURCE_ID) == 0);
}

static bool is_phonecam(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	return id && strcmp(id, PHONECAM_SOURCE_ID) == 0;
}

typedef DARRAY(obs_source_t *) source_list_t;

static void add_unique(source_list_t *list, obs_source_t *source)
{
	for (size_t i = 0; i < list->num; i++)
		if (list->array[i] == source)
			return;
	obs_source_t *ref = obs_source_get_ref(source);
	if (ref)
		da_push_back(*list, &ref);
}

static bool enum_mic(void *param, obs_source_t *source)
{
	if (is_mic(source))
		add_unique(param, source);
	return true;
}

/* 전역 마이크/보조 장치(설정 → 오디오)와 장면 안의 오디오 입력 캡처, 캠뉴런 소스 */
static void collect_mics(source_list_t *list)
{
	for (uint32_t channel = 1; channel <= 6; channel++) {
		obs_source_t *source = obs_get_output_source(channel);
		if (source && is_mic(source))
			add_unique(list, source);
		obs_source_release(source);
	}
	obs_enum_sources(enum_mic, list);
}

static void release_list(source_list_t *list)
{
	for (size_t i = 0; i < list->num; i++)
		obs_source_release(list->array[i]);
	da_free(*list);
}

/* ------------------------------------------------------------------------- */
/* 상태 모으기 (UI 스레드) */

static long long output_seconds(obs_output_t *output, double fps)
{
	return output && fps > 0 ? (long long)(obs_output_get_total_frames(output) / fps) : 0;
}

static void refresh_now(void)
{
	struct snapshot next = {0};
	char **names = obs_frontend_get_scene_names();
	for (char **name = names; name && *name; name++) {
		char *copy = bstrdup(*name);
		da_push_back(next.scenes, &copy);
	}
	bfree(names);

	obs_source_t *current = obs_frontend_get_current_scene();
	next.scene = bstrdup(current ? obs_source_get_name(current) : "");
	obs_source_release(current);
	next.studio = obs_frontend_preview_program_mode_active();

	struct obs_video_info ovi;
	double fps = obs_get_video_info(&ovi) && ovi.fps_den ? (double)ovi.fps_num / ovi.fps_den : 0;
	next.streaming = obs_frontend_streaming_active();
	if (next.streaming) {
		obs_output_t *output = obs_frontend_get_streaming_output();
		next.stream_s = output_seconds(output, fps);
		next.total = output ? obs_output_get_total_frames(output) : 0;
		next.dropped = output ? obs_output_get_frames_dropped(output) : 0;
		obs_output_release(output);
	}
	next.recording = obs_frontend_recording_active();
	if (next.recording) {
		obs_output_t *output = obs_frontend_get_recording_output();
		next.record_s = output_seconds(output, fps);
		obs_output_release(output);
	}

	source_list_t mics = {0};
	collect_mics(&mics);
	for (size_t i = 0; i < mics.num; i++) {
		struct mic_info info = {0};
		info.name = bstrdup(obs_source_get_name(mics.array[i]));
		snprintf(info.uuid, sizeof(info.uuid), "%s", obs_source_get_uuid(mics.array[i]));
		info.muted = obs_source_muted(mics.array[i]);
		info.phonecam = is_phonecam(mics.array[i]);
		da_push_back(next.mics, &info);
	}
	release_list(&mics);

	pthread_mutex_lock(&snap_mutex);
	bool changed = !same_state(&snap, &next);
	snapshot_free(&snap);
	snap = next;
	pthread_mutex_unlock(&snap_mutex);
	if (changed)
		os_atomic_inc_long(&snap_version);
}

static void refresh_task(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_atomic_set_long(&refresh_queued, 0);
	refresh_now();
}

void pcam_remote_init(void)
{
	pthread_mutex_init(&snap_mutex, NULL);
}

void pcam_remote_free(void)
{
	pthread_mutex_lock(&snap_mutex);
	snapshot_free(&snap);
	pthread_mutex_unlock(&snap_mutex);
	pthread_mutex_destroy(&snap_mutex);
}

void pcam_remote_request_refresh(void)
{
	if (os_atomic_exchange_long(&refresh_queued, 1) == 0)
		obs_queue_task(OBS_TASK_UI, refresh_task, NULL, false);
}

long pcam_remote_version(void)
{
	return os_atomic_load_long(&snap_version);
}

bool pcam_remote_live(void)
{
	pthread_mutex_lock(&snap_mutex);
	bool live = snap.streaming || snap.recording;
	pthread_mutex_unlock(&snap_mutex);
	return live;
}

/* ------------------------------------------------------------------------- */
/* obs 메시지 */

static void json_string(struct dstr *d, const char *s)
{
	dstr_cat_ch(d, '"');
	for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
		if (*p == '"')
			dstr_cat(d, "\\\"");
		else if (*p == '\\')
			dstr_cat(d, "\\\\");
		else if (*p < 0x20)
			dstr_catf(d, "\\u%04x", *p);
		else
			dstr_cat_ch(d, (char)*p);
	}
	dstr_cat_ch(d, '"');
}

char *pcam_remote_state_json(obs_source_t *self, bool phone_mode, const char *phone_scene)
{
	const char *self_uuid = obs_source_get_uuid(self);
	struct dstr d = {0};
	pthread_mutex_lock(&snap_mutex);
	dstr_copy(&d, "{\"type\":\"obs\",\"scenes\":[");
	for (size_t i = 0; i < snap.scenes.num; i++) {
		if (i)
			dstr_cat_ch(&d, ',');
		json_string(&d, snap.scenes.array[i]);
	}
	dstr_cat(&d, "],\"scene\":");
	json_string(&d, snap.scene);
	dstr_catf(&d,
		  ",\"studio\":%s,\"streaming\":%s,\"stream_s\":%lld,\"dropped\":%lld,\"total\":%lld,"
		  "\"recording\":%s,\"record_s\":%lld,\"mics\":[",
		  snap.studio ? "true" : "false", snap.streaming ? "true" : "false", snap.stream_s, snap.dropped,
		  snap.total, snap.recording ? "true" : "false", snap.record_s);
	bool first = true;
	for (size_t i = 0; i < snap.mics.num; i++) {
		const struct mic_info *mic = &snap.mics.array[i];
		bool mine = self_uuid && strcmp(mic->uuid, self_uuid) == 0;
		if (mic->phonecam && !mine)
			continue; /* 다른 폰의 캠뉴런 소스는 그 폰에서 다룬다 */
		if (!first)
			dstr_cat_ch(&d, ',');
		first = false;
		dstr_cat(&d, "{\"name\":");
		json_string(&d, mic->name);
		dstr_catf(&d, ",\"muted\":%s%s}", mic->muted ? "true" : "false", mine ? ",\"phone\":true" : "");
	}
	pthread_mutex_unlock(&snap_mutex);
	dstr_catf(&d, "],\"mode\":\"%s\",\"phone_scene\":", phone_mode ? "phone" : "pc");
	json_string(&d, phone_scene);
	dstr_cat_ch(&d, '}');
	return d.array;
}

/* ------------------------------------------------------------------------- */
/* 폰 장면 고르기 */

struct area_search {
	obs_source_t *self;
	float area;
};

static bool find_self_area(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct area_search *search = param;
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, find_self_area, param);
		return true;
	}
	if (obs_sceneitem_get_source(item) != search->self || !obs_sceneitem_visible(item))
		return true;
	struct vec2 v;
	float area;
	if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
		obs_sceneitem_get_bounds(item, &v);
		area = fabsf(v.x * v.y);
	} else {
		/* 연결 직후 첫 프레임 전에는 크기가 0이라 방송 화면 크기로 본다 */
		uint32_t width = obs_source_get_width(search->self);
		uint32_t height = obs_source_get_height(search->self);
		struct obs_video_info ovi;
		if ((!width || !height) && obs_get_video_info(&ovi)) {
			width = ovi.base_width;
			height = ovi.base_height;
		}
		obs_sceneitem_get_scale(item, &v);
		area = fabsf(v.x * v.y) * (float)width * (float)height;
	}
	if (area > search->area)
		search->area = area;
	return true;
}

char *pcam_remote_phone_scene(obs_source_t *self, const char *phone_scene_setting)
{
	if (phone_scene_setting && *phone_scene_setting)
		return bstrdup(phone_scene_setting);

	/* 이 폰 카메라가 가장 크게 보이는 장면 (같으면 목록에서 앞에 있는 장면) */
	pthread_mutex_lock(&snap_mutex);
	DARRAY(char *) names = {0};
	for (size_t i = 0; i < snap.scenes.num; i++) {
		char *copy = bstrdup(snap.scenes.array[i]);
		da_push_back(names, &copy);
	}
	pthread_mutex_unlock(&snap_mutex);

	char *best = NULL;
	float best_area = 0.0f;
	for (size_t i = 0; i < names.num; i++) {
		obs_source_t *source = obs_get_source_by_name(names.array[i]);
		obs_scene_t *scene = obs_scene_from_source(source);
		if (scene) {
			struct area_search search = {.self = self};
			obs_scene_enum_items(scene, find_self_area, &search);
			if (search.area > best_area) {
				best_area = search.area;
				bfree(best);
				best = bstrdup(names.array[i]);
			}
		}
		obs_source_release(source);
		bfree(names.array[i]);
	}
	da_free(names);
	return best ? best : bstrdup("");
}

/* ------------------------------------------------------------------------- */
/* 명령 (UI 스레드) */

static void set_scene(const char *name)
{
	obs_source_t *source = obs_get_source_by_name(name);
	if (source && obs_scene_from_source(source))
		obs_frontend_set_current_scene(source);
	else
		blog(LOG_WARNING, "[phonecam] remote: no scene named '%s'", name);
	obs_source_release(source);
}

static void enter_phone_mode(obs_source_t *self, struct pcam_remote_mode *mode, const char *setting)
{
	if (mode->phone)
		return;
	obs_source_t *current = obs_frontend_get_current_scene();
	snprintf(mode->pc_scene, sizeof(mode->pc_scene), "%s", current ? obs_source_get_name(current) : "");
	obs_source_release(current);

	/* PC 앞에 없으니 PC 마이크는 끄고 이 폰의 소리를 켠다. 돌아올 때 원래대로 되돌리려고 기억한다. */
	source_list_t mics = {0};
	collect_mics(&mics);
	bfree(mode->saved);
	mode->saved = bzalloc(sizeof(*mode->saved) * (mics.num ? mics.num : 1));
	mode->saved_count = 0;
	for (size_t i = 0; i < mics.num; i++) {
		obs_source_t *mic = mics.array[i];
		if (mic == self) {
			obs_source_set_muted(mic, false);
			continue;
		}
		if (is_phonecam(mic))
			continue;
		struct saved_mute *saved = &mode->saved[mode->saved_count++];
		snprintf(saved->uuid, sizeof(saved->uuid), "%s", obs_source_get_uuid(mic));
		saved->muted = obs_source_muted(mic);
		obs_source_set_muted(mic, true);
	}
	release_list(&mics);

	char *target = pcam_remote_phone_scene(self, setting);
	if (*target && strcmp(target, mode->pc_scene) != 0)
		set_scene(target);
	blog(LOG_INFO, "[phonecam] remote: switched to the phone (scene '%s', %zu PC mics muted)", target,
	     mode->saved_count);
	bfree(target);
	mode->phone = true;
}

static void leave_phone_mode(struct pcam_remote_mode *mode)
{
	if (!mode->phone)
		return;
	if (*mode->pc_scene)
		set_scene(mode->pc_scene);
	for (size_t i = 0; i < mode->saved_count; i++) {
		obs_source_t *mic = obs_get_source_by_uuid(mode->saved[i].uuid);
		if (mic)
			obs_source_set_muted(mic, mode->saved[i].muted);
		obs_source_release(mic);
	}
	blog(LOG_INFO, "[phonecam] remote: back to the PC (scene '%s')", mode->pc_scene);
	bfree(mode->saved);
	mode->saved = NULL;
	mode->saved_count = 0;
	mode->phone = false;
}

void pcam_remote_execute(obs_source_t *self, struct pcam_remote_mode *mode, const char *phone_scene_setting,
			 obs_data_t *cmd)
{
	const char *what = obs_data_get_string(cmd, "cmd");
	if (strcmp(what, "scene") == 0) {
		set_scene(obs_data_get_string(cmd, "name"));
	} else if (strcmp(what, "mute") == 0) {
		obs_source_t *source = obs_get_source_by_name(obs_data_get_string(cmd, "name"));
		if (source && is_mic(source))
			obs_source_set_muted(source, obs_data_get_bool(cmd, "muted"));
		obs_source_release(source);
	} else if (strcmp(what, "mode") == 0) {
		if (strcmp(obs_data_get_string(cmd, "mode"), "phone") == 0)
			enter_phone_mode(self, mode, phone_scene_setting);
		else
			leave_phone_mode(mode);
	} else if (strcmp(what, "stream") == 0) {
		bool on = obs_data_get_bool(cmd, "on");
		if (on && !obs_frontend_streaming_active())
			obs_frontend_streaming_start();
		else if (!on && obs_frontend_streaming_active())
			obs_frontend_streaming_stop();
	} else if (strcmp(what, "record") == 0) {
		bool on = obs_data_get_bool(cmd, "on");
		if (on && !obs_frontend_recording_active())
			obs_frontend_recording_start();
		else if (!on && obs_frontend_recording_active())
			obs_frontend_recording_stop();
	} else {
		return;
	}
	refresh_now(); /* 결과를 바로 폰에 알리려고 */
}

/* ------------------------------------------------------------------------- */
/* 모니터 오디오 트랙 */

bool pcam_remote_mics_on_track(int track)
{
	if (track < 1 || track > 6)
		return false;
	source_list_t mics = {0};
	collect_mics(&mics);
	bool found = false;
	for (size_t i = 0; i < mics.num && !found; i++)
		found = (obs_source_get_audio_mixers(mics.array[i]) & (1u << (track - 1))) != 0;
	release_list(&mics);
	return found;
}

int pcam_remote_remove_mics_from_track(int track)
{
	if (track < 1 || track > 6)
		return 0;
	source_list_t mics = {0};
	collect_mics(&mics);
	int removed = 0;
	for (size_t i = 0; i < mics.num; i++) {
		uint32_t mixers = obs_source_get_audio_mixers(mics.array[i]);
		if (mixers & (1u << (track - 1))) {
			obs_source_set_audio_mixers(mics.array[i], mixers & ~(1u << (track - 1)));
			removed++;
		}
	}
	release_list(&mics);
	blog(LOG_INFO, "[phonecam] monitor: removed %d mic(s) from audio track %d", removed, track);
	return removed;
}

void pcam_remote_mode_free(struct pcam_remote_mode *mode)
{
	bfree(mode->saved);
	mode->saved = NULL;
	mode->saved_count = 0;
}
