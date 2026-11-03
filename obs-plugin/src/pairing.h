/* PC 승인 (protocol/PROTOCOL.md 1.1)의 PC 쪽: 이 PC의 ID, 폰마다 받은 토큰, HMAC 계산. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCAM_NONCE_BYTES 16
#define PCAM_TOKEN_BYTES 32
#define PCAM_PROOF_BYTES 32
#define PCAM_PAIR_CODE_LEN 6

/*
 * 이 PC(OBS 설치)의 고정 ID. 처음 부를 때 만들어 플러그인 설정 폴더의 pairing.json에 적는다.
 * 토큰도 같은 파일에 폰 ID별로 저장하므로 소스를 지우고 다시 만들어도 다시 승인받지 않아도 된다.
 */
void pcam_pairing_pc_id(char *out, size_t size);

/* 폰 화면에 보여 줄 이 PC의 이름 (컴퓨터 이름). */
void pcam_pairing_pc_name(char *out, size_t size);

/* phone_id 폰에게 받은 토큰. 없으면 false. */
bool pcam_pairing_token(const char *phone_id, uint8_t token[PCAM_TOKEN_BYTES]);
void pcam_pairing_save_token(const char *phone_id, const uint8_t token[PCAM_TOKEN_BYTES]);
void pcam_pairing_forget(const char *phone_id);

/* 폰에 보낼 proof: HMAC-SHA256(key = 토큰, data = nonce). */
bool pcam_pairing_proof(const uint8_t token[PCAM_TOKEN_BYTES], const uint8_t *nonce, size_t nonce_len,
			uint8_t out[PCAM_PROOF_BYTES]);

/* 사용자가 입력한 코드에서 숫자만 뽑는다 ("482 913"도 받음). 정확히 6자리면 true. */
bool pcam_pairing_normalize_code(const char *input, char out[PCAM_PAIR_CODE_LEN + 1]);

/* out에는 2 * len + 1바이트가 필요하다. */
void pcam_hex_encode(const uint8_t *data, size_t len, char *out);

/* 정확히 len바이트로 풀리면 true. */
bool pcam_hex_decode(const char *hex, uint8_t *out, size_t len);
