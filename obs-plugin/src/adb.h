#pragma once

#include <stdbool.h>

/* `adb forward tcp:port tcp:port`로 USB 연결된 폰의 포트를 PC의 127.0.0.1:port로 연결한다. */
bool pcam_adb_forward(const char *adb_path, int port);
