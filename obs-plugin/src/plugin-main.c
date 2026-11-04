#include <obs-module.h>

#include "monitor.h"
#include "net.h"
#include "overlay.h"
#include "remote.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-phonecam", "en-US")

extern struct obs_source_info phonecam_source_info;

/* OBS의 플러그인 목록에 보이는 이름. 모듈(폴더·DLL) 이름 obs-phonecam은 설치된 장면과 맞추려고 그대로 둔다. */
MODULE_EXPORT const char *obs_module_name(void)
{
	return "CamNeuron";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Use an Android phone camera as a video source (CamNeuron)";
}

bool obs_module_load(void)
{
	if (!pcam_net_init()) {
		blog(LOG_ERROR, "[phonecam] failed to initialize networking");
		return false;
	}
	pcam_remote_init();
	obs_register_source(&phonecam_source_info);
	obs_register_source(&pcam_overlay_filter_info);
	obs_register_output(&pcam_monitor_output_info);
	blog(LOG_INFO, "[phonecam] plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	pcam_remote_free();
	pcam_net_free();
}
