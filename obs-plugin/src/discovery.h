/* 같은 네트워크의 폰 찾기 (UDP 브로드캐스트). protocol/PROTOCOL.md 8장. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define PCAM_DISCOVERY_PORT 9610
#define PCAM_MAX_FOUND 8

struct pcam_found_phone {
	char id[64];   /* 폰 앱이 처음 실행될 때 만든 고정 ID */
	char name[64]; /* 폰에 설정된 기기 이름 */
	char host[64]; /* 답이 온 IPv4 주소 */
	int port;      /* 송출 TCP 포트 */
	bool busy;     /* 다른 PC가 연결 중 */
};

/* 모든 네트워크 어댑터로 "폰 찾기"를 브로드캐스트하고 timeout_ms 동안 답을 모은다. 같은 폰은 한 번만 넣는다. */
size_t pcam_discover(struct pcam_found_phone *out, size_t max, int timeout_ms);
