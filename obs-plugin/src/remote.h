/*
 * 폰에서 방송 진행: OBS 장면·방송 상태·마이크를 폰에 알리고, 폰의 명령(장면 바꾸기, 마이크, 폰으로 전환)을 OBS에 적용한다
 * (protocol/PROTOCOL.md 9.1, 9.2). OBS 화면 쪽 API(obs-frontend-api)는 UI 스레드에서만 부른다.
 */
#pragma once

#include <obs-module.h>

struct saved_mute;

/* 폰으로 전환할 때 기억해 둔 장면과 마이크 상태 (소스마다, UI 스레드에서만 쓴다). */
struct pcam_remote_mode {
	bool phone;
	char pc_scene[256];
	struct saved_mute *saved;
	size_t saved_count;
};

void pcam_remote_init(void);
void pcam_remote_free(void);

/* UI 스레드에 OBS 상태를 다시 모으게 한다. 이미 대기 중이면 아무것도 하지 않는다. 어느 스레드에서나 부를 수 있다. */
void pcam_remote_request_refresh(void);

/* 지금 상태를 바꾸면 늘어나는 번호. 방송·녹화 시간은 포함하지 않는다. */
long pcam_remote_version(void);

/* 방송이나 녹화 중이라 시간이 흐르는 중인지 */
bool pcam_remote_live(void);

/* 모아 둔 상태로 obs 메시지(JSON)를 만든다. bfree로 해제. [phone_scene]은 정해 둔 폰 장면(비면 자동으로 고른 장면). */
char *pcam_remote_state_json(obs_source_t *self, bool phone_mode, const char *phone_scene);

/* 폰의 obs_cmd를 실행한다. UI 스레드에서 부른다. */
void pcam_remote_execute(obs_source_t *self, struct pcam_remote_mode *mode, const char *phone_scene_setting,
			 obs_data_t *cmd);

/* 폰 장면: 설정 값이 있으면 그것, 없으면 이 소스가 가장 크게 보이는 장면. UI 스레드에서 부른다. bfree로 해제. */
char *pcam_remote_phone_scene(obs_source_t *self, const char *phone_scene_setting);

/*
 * 폰 연결이 끊기면(영상이 멈추면) 지금 장면을 [return_scene]에 기억하고 [lost_scene]으로 바꾼다. 이미 그 장면이면 기억하지 않는다.
 * UI 스레드에서 부른다.
 */
void pcam_remote_enter_lost(const char *lost_scene, char *return_scene, size_t size);

/* 다시 이어지면 그사이 사용자가 다른 장면으로 바꾸지 않았을 때만 [return_scene]으로 되돌린다. UI 스레드에서 부른다. */
void pcam_remote_leave_lost(const char *lost_scene, char *return_scene);

/* 마이크(전역 마이크/보조 장치, 장면 안의 오디오 입력 캡처, 캠뉴런 소스)가 [track](1~6)에 들어 있는지 */
bool pcam_remote_mics_on_track(int track);

/* [track]에서 마이크를 뺀다 (OBS 고급 오디오 속성의 트랙 체크를 끔). 뺀 수를 돌려준다. UI 스레드에서 부른다. */
int pcam_remote_remove_mics_from_track(int track);

void pcam_remote_mode_free(struct pcam_remote_mode *mode);
