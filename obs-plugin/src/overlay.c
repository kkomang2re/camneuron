#include <math.h>
#include <stdio.h>

#include <util/threading.h>

#include "overlay.h"

#define DEFAULT_REFERENCE_WIDTH 720.0
#define SLOT_COUNT 2

/* 브라우저 소스 하나와 그것을 그릴 위치. */
struct browser_slot {
	obs_source_t *browser;
	char url[PCAM_URL_MAX];
	int width;
	int height;
	char css[256];
	float x;
	float y;
	float scale;
};

struct overlay_filter {
	obs_source_t *self;
	pthread_mutex_t mutex; /* slots: UI 스레드가 바꾸고 그래픽 스레드가 읽는다 */
	struct browser_slot slots[SLOT_COUNT];
};

static const char *item_key(char *buf, size_t size, const char *prefix, const char *name)
{
	snprintf(buf, size, "%s_%s", prefix, name);
	return buf;
}

static void parse_item(struct pcam_overlay_item *item, obs_data_t *msg, const char *prefix)
{
	char key[32];
	snprintf(item->url, sizeof(item->url), "%s", obs_data_get_string(msg, item_key(key, sizeof(key), prefix, "url")));
	item->stream = obs_data_get_bool(msg, item_key(key, sizeof(key), prefix, "stream"));
	item->x = obs_data_get_double(msg, item_key(key, sizeof(key), prefix, "x"));
	item->y = obs_data_get_double(msg, item_key(key, sizeof(key), prefix, "y"));
	item->w = obs_data_get_double(msg, item_key(key, sizeof(key), prefix, "w"));
	item->h = obs_data_get_double(msg, item_key(key, sizeof(key), prefix, "h"));
	item->zoom = obs_data_get_double(msg, item_key(key, sizeof(key), prefix, "zoom"));
	item->opacity = obs_data_get_double(msg, item_key(key, sizeof(key), prefix, "opacity"));
	if (item->zoom <= 0.0)
		item->zoom = 1.0;
	if (item->opacity <= 0.0)
		item->opacity = 1.0;
}

void pcam_overlays_from_json(struct pcam_overlays *overlays, obs_data_t *msg)
{
	overlays->valid = true;
	overlays->reference_width = obs_data_get_double(msg, "reference_width");
	if (overlays->reference_width <= 0.0)
		overlays->reference_width = DEFAULT_REFERENCE_WIDTH;
	parse_item(&overlays->chat, msg, "chat");
	parse_item(&overlays->alert, msg, "alert");
}

static bool item_streamed(const struct pcam_overlay_item *item)
{
	return item->stream && item->url[0];
}

bool pcam_overlays_streamed(const struct pcam_overlays *overlays)
{
	return overlays->valid && (item_streamed(&overlays->chat) || item_streamed(&overlays->alert));
}

/* ------------------------------------------------------------------------- */
/* 필터 */

static const char *filter_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("OverlayFilter");
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct overlay_filter *filter = bzalloc(sizeof(*filter));
	filter->self = source;
	pthread_mutex_init(&filter->mutex, NULL);
	return filter;
}

static void release_browser(obs_source_t *browser)
{
	if (!browser)
		return;
	obs_source_dec_showing(browser);
	obs_source_release(browser);
}

static void filter_destroy(void *data)
{
	struct overlay_filter *filter = data;
	for (size_t i = 0; i < SLOT_COUNT; i++)
		release_browser(filter->slots[i].browser);
	pthread_mutex_destroy(&filter->mutex);
	bfree(filter);
}

/* 카메라 영상을 그대로 그린 뒤, 브라우저 화면을 폰에서 정한 위치에 겹쳐 그린다. */
static void filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct overlay_filter *filter = data;
	obs_source_skip_video_filter(filter->self);

	pthread_mutex_lock(&filter->mutex);
	for (size_t i = 0; i < SLOT_COUNT; i++) {
		struct browser_slot *slot = &filter->slots[i];
		if (!slot->browser)
			continue;
		gs_matrix_push();
		gs_matrix_translate3f(slot->x, slot->y, 0.0f);
		gs_matrix_scale3f(slot->scale, slot->scale, 1.0f);
		obs_source_video_render(slot->browser);
		gs_matrix_pop();
	}
	pthread_mutex_unlock(&filter->mutex);
}

struct obs_source_info pcam_overlay_filter_info = {
	.id = PCAM_OVERLAY_FILTER_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	/* 필터 추가 메뉴에는 보이지 않게 한다. 캠뉴런 소스가 필요할 때 직접 단다. */
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CAP_DISABLED,
	.get_name = filter_get_name,
	.create = filter_create,
	.destroy = filter_destroy,
	.video_render = filter_render,
};

/* ------------------------------------------------------------------------- */
/* 브라우저 소스 맞추기 (UI 스레드) */

static void apply_slot(struct overlay_filter *filter, size_t index, const struct pcam_overlay_item *item,
		       double reference_width, uint32_t stream_width, uint32_t stream_height, const char *name)
{
	struct browser_slot *slot = &filter->slots[index];
	if (!item_streamed(item) || !stream_width || !stream_height) {
		pthread_mutex_lock(&filter->mutex);
		obs_source_t *old = slot->browser;
		memset(slot, 0, sizeof(*slot));
		pthread_mutex_unlock(&filter->mutex);
		/* 그래픽 스레드가 그리는 중일 수 있으므로 잠금 밖에서 놓는다. */
		if (old)
			blog(LOG_INFO, "[phonecam] %s removed from stream", name);
		release_browser(old);
		return;
	}

	/* 폰 화면의 CSS 크기 그대로 브라우저를 만들고, 영상 크기에 맞춰 확대해서 그린다 (폰과 같은 모양). */
	int width = (int)lround(item->w * reference_width);
	int height = (int)lround(item->h * reference_width * stream_height / stream_width);
	char css[256];
	snprintf(css, sizeof(css),
		 "body { background-color: rgba(0, 0, 0, 0); margin: 0; overflow: hidden; } "
		 "html { zoom: %.2f; opacity: %.2f; }",
		 item->zoom, item->opacity);

	bool changed = !slot->browser || strcmp(slot->url, item->url) != 0 || slot->width != width ||
		       slot->height != height || strcmp(slot->css, css) != 0;
	obs_source_t *created = NULL;
	if (changed) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "url", item->url);
		obs_data_set_int(settings, "width", width);
		obs_data_set_int(settings, "height", height);
		obs_data_set_string(settings, "css", css);
		obs_data_set_bool(settings, "shutdown", false);
		obs_data_set_bool(settings, "restart_when_active", false);
		if (slot->browser) {
			obs_source_update(slot->browser, settings);
		} else {
			created = obs_source_create_private("browser_source", name, settings);
			if (created) {
				obs_source_inc_showing(created);
				blog(LOG_INFO, "[phonecam] %s added to stream (%dx%d)", name, width, height);
			} else {
				blog(LOG_WARNING, "[phonecam] browser source is not available; cannot show %s", name);
			}
		}
		obs_data_release(settings);
	}

	pthread_mutex_lock(&filter->mutex);
	if (created)
		slot->browser = created;
	snprintf(slot->url, sizeof(slot->url), "%s", item->url);
	snprintf(slot->css, sizeof(slot->css), "%s", css);
	slot->width = width;
	slot->height = height;
	slot->x = (float)(item->x * stream_width);
	slot->y = (float)(item->y * stream_height);
	slot->scale = (float)(stream_width / reference_width);
	pthread_mutex_unlock(&filter->mutex);
}

void pcam_overlay_filter_apply(obs_source_t *filter_source, const struct pcam_overlays *overlays, uint32_t stream_width,
			       uint32_t stream_height)
{
	struct overlay_filter *filter = obs_obj_get_data(filter_source);
	if (!filter)
		return;
	double reference = overlays->reference_width > 0.0 ? overlays->reference_width : DEFAULT_REFERENCE_WIDTH;
	apply_slot(filter, 0, &overlays->chat, reference, stream_width, stream_height, "CamNeuron chat overlay");
	apply_slot(filter, 1, &overlays->alert, reference, stream_width, stream_height, "CamNeuron alert overlay");
}
