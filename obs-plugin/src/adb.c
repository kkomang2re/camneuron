#include <obs-module.h>
#include <util/dstr.h>
#include <util/pipe.h>

#include "adb.h"

static void read_all(os_process_pipe_t *pipe, struct dstr *out, bool from_stderr)
{
	uint8_t buf[256];
	size_t len;
	while ((len = from_stderr ? os_process_pipe_read_err(pipe, buf, sizeof(buf))
				  : os_process_pipe_read(pipe, buf, sizeof(buf))) > 0)
		dstr_ncat(out, (const char *)buf, len);
}

bool pcam_adb_forward(const char *adb_path, int port)
{
	struct os_process_args *args = os_process_args_create(adb_path && *adb_path ? adb_path : "adb");
	os_process_args_add_arg(args, "forward");
	os_process_args_add_argf(args, "tcp:%d", port);
	os_process_args_add_argf(args, "tcp:%d", port);

	os_process_pipe_t *pipe = os_process_pipe_create2(args, "r");
	os_process_args_destroy(args);
	if (!pipe) {
		blog(LOG_WARNING, "[phonecam] failed to run adb: %s", adb_path);
		return false;
	}

	/* 출력을 끝까지 읽어야 adb가 막히지 않고 종료된다. */
	struct dstr out = {0};
	struct dstr err = {0};
	read_all(pipe, &out, false);
	read_all(pipe, &err, true);
	int code = os_process_pipe_destroy(pipe);

	if (code != 0) {
		dstr_depad(&err);
		blog(LOG_WARNING, "[phonecam] adb forward failed (code %d): %s", code, err.array ? err.array : "");
	}
	dstr_free(&out);
	dstr_free(&err);
	return code == 0;
}
