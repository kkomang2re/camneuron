/*
 * 폰으로 보내는 패킷을 한 스레드에서 보낸다. 다른 스레드(OBS UI, 인코더)는 넣기만 하고 기다리지 않는다.
 * 제어 메시지는 버리지 않고 먼저 보내고, 방송 모니터 패킷은 Wi-Fi가 느려 밀리면 버린다 (protocol/PROTOCOL.md 9.3).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net.h"

struct pcam_writer;

struct pcam_writer *pcam_writer_start(pcam_socket_t sock);

/* 보내기를 멈추고 스레드를 기다린다. 보내는 중이면 소켓을 shutdown해 깨운다 (닫지는 않음). */
void pcam_writer_stop(struct pcam_writer *w);

/* 제어 메시지(JSON). 보내지 못하면 소켓을 shutdown해 받는 스레드가 다시 접속하게 한다. */
void pcam_writer_control(struct pcam_writer *w, const char *json, size_t len);

/*
 * 모니터 패킷. 가장 오래 기다린 모니터 패킷이 [PCAM_WRITER_MAX_DELAY_MS]를 넘으면 쌓인 것을 모두 버리고,
 * 다음 영상 CONFIG(키프레임 바로 앞에 옴)부터 다시 보낸다.
 */
void pcam_writer_media(struct pcam_writer *w, uint8_t stream_id, uint8_t flags, int64_t pts_us, const uint8_t *data,
		       size_t size);

#define PCAM_WRITER_MAX_DELAY_MS 500
