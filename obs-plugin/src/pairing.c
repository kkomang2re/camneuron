#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include "pairing.h"

#define PAIRING_FILE "pairing.json"
#define PC_ID_BYTES 16

/* 소스 여러 개의 연결 스레드가 같은 파일을 읽고 쓴다. */
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *pairing_path(void)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	return obs_module_config_path(PAIRING_FILE);
}

/* file_mutex를 잡은 상태에서 부른다. 파일이 없으면 빈 객체. */
static obs_data_t *load_locked(void)
{
	char *path = pairing_path();
	obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : NULL;
	bfree(path);
	return data ? data : obs_data_create();
}

static void save_locked(obs_data_t *data)
{
	char *path = pairing_path();
	if (path && !obs_data_save_json_safe(data, path, "tmp", "bak"))
		blog(LOG_WARNING, "[phonecam] could not save %s", path);
	bfree(path);
}

static bool random_bytes(uint8_t *out, size_t len)
{
	return BCRYPT_SUCCESS(BCryptGenRandom(NULL, out, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

void pcam_pairing_pc_id(char *out, size_t size)
{
	pthread_mutex_lock(&file_mutex);
	obs_data_t *data = load_locked();
	const char *id = obs_data_get_string(data, "pc_id");
	if (!*id) {
		uint8_t raw[PC_ID_BYTES];
		char hex[PC_ID_BYTES * 2 + 1];
		if (random_bytes(raw, sizeof(raw))) {
			pcam_hex_encode(raw, sizeof(raw), hex);
			obs_data_set_string(data, "pc_id", hex);
			save_locked(data);
			id = obs_data_get_string(data, "pc_id");
		}
	}
	snprintf(out, size, "%s", id);
	obs_data_release(data);
	pthread_mutex_unlock(&file_mutex);
}

void pcam_pairing_pc_name(char *out, size_t size)
{
	wchar_t name[256];
	DWORD len = (DWORD)(sizeof(name) / sizeof(name[0]));
	char *utf8 = NULL;
	if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &len))
		os_wcs_to_utf8_ptr(name, len, &utf8);
	snprintf(out, size, "%s", utf8 && *utf8 ? utf8 : "PC");
	bfree(utf8);
}

bool pcam_pairing_token(const char *phone_id, uint8_t token[PCAM_TOKEN_BYTES])
{
	if (!phone_id || !*phone_id)
		return false;
	pthread_mutex_lock(&file_mutex);
	obs_data_t *data = load_locked();
	obs_data_t *tokens = obs_data_get_obj(data, "tokens");
	bool found = tokens && pcam_hex_decode(obs_data_get_string(tokens, phone_id), token, PCAM_TOKEN_BYTES);
	obs_data_release(tokens);
	obs_data_release(data);
	pthread_mutex_unlock(&file_mutex);
	return found;
}

static void set_token(const char *phone_id, const char *hex)
{
	if (!phone_id || !*phone_id)
		return;
	pthread_mutex_lock(&file_mutex);
	obs_data_t *data = load_locked();
	obs_data_t *tokens = obs_data_get_obj(data, "tokens");
	if (!tokens) {
		tokens = obs_data_create();
		obs_data_set_obj(data, "tokens", tokens);
	}
	if (hex)
		obs_data_set_string(tokens, phone_id, hex);
	else
		obs_data_erase(tokens, phone_id);
	save_locked(data);
	obs_data_release(tokens);
	obs_data_release(data);
	pthread_mutex_unlock(&file_mutex);
}

void pcam_pairing_save_token(const char *phone_id, const uint8_t token[PCAM_TOKEN_BYTES])
{
	char hex[PCAM_TOKEN_BYTES * 2 + 1];
	pcam_hex_encode(token, PCAM_TOKEN_BYTES, hex);
	set_token(phone_id, hex);
}

void pcam_pairing_forget(const char *phone_id)
{
	set_token(phone_id, NULL);
}

bool pcam_pairing_proof(const uint8_t token[PCAM_TOKEN_BYTES], const uint8_t *nonce, size_t nonce_len,
			uint8_t out[PCAM_PROOF_BYTES])
{
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	bool ok = BCRYPT_SUCCESS(
			  BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG)) &&
		  BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, NULL, 0, (PUCHAR)token, PCAM_TOKEN_BYTES, 0)) &&
		  BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)nonce, (ULONG)nonce_len, 0)) &&
		  BCRYPT_SUCCESS(BCryptFinishHash(hash, out, PCAM_PROOF_BYTES, 0));
	if (hash)
		BCryptDestroyHash(hash);
	if (alg)
		BCryptCloseAlgorithmProvider(alg, 0);
	return ok;
}

bool pcam_pairing_normalize_code(const char *input, char out[PCAM_PAIR_CODE_LEN + 1])
{
	size_t n = 0;
	for (const char *p = input ? input : ""; *p; p++) {
		if (isspace((unsigned char)*p) || *p == '-')
			continue;
		if (!isdigit((unsigned char)*p) || n == PCAM_PAIR_CODE_LEN)
			return false;
		out[n++] = *p;
	}
	out[n] = 0;
	return n == PCAM_PAIR_CODE_LEN;
}

void pcam_hex_encode(const uint8_t *data, size_t len, char *out)
{
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < len; i++) {
		out[2 * i] = digits[data[i] >> 4];
		out[2 * i + 1] = digits[data[i] & 0x0F];
	}
	out[2 * len] = 0;
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

bool pcam_hex_decode(const char *hex, uint8_t *out, size_t len)
{
	if (!hex || strlen(hex) != 2 * len)
		return false;
	for (size_t i = 0; i < len; i++) {
		int high = hex_value(hex[2 * i]);
		int low = hex_value(hex[2 * i + 1]);
		if (high < 0 || low < 0)
			return false;
		out[i] = (uint8_t)(high * 16 + low);
	}
	return true;
}
