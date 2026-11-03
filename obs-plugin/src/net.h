/* 플랫폼별 소켓 차이를 감추는 최소한의 TCP 클라이언트. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Windows의 SOCKET(UINT_PTR)과 POSIX의 int를 모두 담는다. 헤더에서 winsock2.h를 피하려는 목적. */
typedef intptr_t pcam_socket_t;
#define PCAM_INVALID_SOCKET ((pcam_socket_t)-1)

bool pcam_net_init(void);
void pcam_net_free(void);

/* 접속에 실패하면 PCAM_INVALID_SOCKET. recv_timeout_ms 동안 받은 게 없으면 recv가 실패한다. */
pcam_socket_t pcam_connect(const char *host, int port, int connect_timeout_ms, int recv_timeout_ms);
bool pcam_send_all(pcam_socket_t sock, const void *data, size_t size);
bool pcam_recv_all(pcam_socket_t sock, void *data, size_t size);

/* 다른 스레드에서 막혀 있는 recv를 깨운다. */
void pcam_shutdown(pcam_socket_t sock);
void pcam_close(pcam_socket_t sock);
