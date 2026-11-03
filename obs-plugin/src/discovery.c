#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <stdio.h>

#include <obs-module.h>
#include <util/platform.h>

#include "discovery.h"

#define REQUEST "{\"type\":\"discover\",\"version\":3}"
#define MAX_TARGETS 16
#define RESEND_MS 300

/*
 * 보낼 브로드캐스트 주소. 255.255.255.255는 Windows가 기본 경로의 어댑터로만 보내므로
 * 어댑터마다 서브넷 브로드캐스트(예: 192.168.0.255)도 함께 보낸다.
 */
static size_t broadcast_targets(ULONG *targets, size_t max)
{
	size_t count = 0;
	targets[count++] = INADDR_BROADCAST;

	ULONG size = 16 * 1024;
	IP_ADAPTER_ADDRESSES *adapters = bmalloc(size);
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG ret = GetAdaptersAddresses(AF_INET, flags, NULL, adapters, &size);
	if (ret == ERROR_BUFFER_OVERFLOW) {
		adapters = brealloc(adapters, size);
		ret = GetAdaptersAddresses(AF_INET, flags, NULL, adapters, &size);
	}
	if (ret == NO_ERROR) {
		for (IP_ADAPTER_ADDRESSES *a = adapters; a && count < max; a = a->Next) {
			if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
				continue;
			for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && count < max; u = u->Next) {
				if (u->Address.lpSockaddr->sa_family != AF_INET || u->OnLinkPrefixLength >= 31 ||
				    u->OnLinkPrefixLength == 0)
					continue;
				ULONG ip = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
				ULONG mask = 0xFFFFFFFFu << (32 - u->OnLinkPrefixLength);
				targets[count++] = htonl(ip | ~mask);
			}
		}
	}
	bfree(adapters);
	return count;
}

static void send_requests(SOCKET sock, const ULONG *targets, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		struct sockaddr_in to = {0};
		to.sin_family = AF_INET;
		to.sin_port = htons(PCAM_DISCOVERY_PORT);
		to.sin_addr.s_addr = targets[i];
		sendto(sock, REQUEST, (int)strlen(REQUEST), 0, (struct sockaddr *)&to, sizeof(to));
	}
}

/*
 * 답 하나를 읽어 목록에 넣는다. 같은 폰이 여러 경로(Wi-Fi와 USB 테더링 등)로 답하면 한 번만 넣고,
 * 폰이 알려 준 Wi-Fi 주소로 온 답이 있으면 그 주소를 쓴다 (케이블을 뽑아도 끊기지 않게).
 */
static size_t add_reply(struct pcam_found_phone *out, size_t count, size_t max, const char *json,
			const struct sockaddr_in *from)
{
	obs_data_t *msg = obs_data_create_from_json(json);
	if (!msg)
		return count;
	const char *id = obs_data_get_string(msg, "id");
	if (strcmp(obs_data_get_string(msg, "type"), "phone") != 0 || !*id) {
		obs_data_release(msg);
		return count;
	}
	char host[64];
	inet_ntop(AF_INET, &from->sin_addr, host, sizeof(host));
	bool from_wifi = strcmp(host, obs_data_get_string(msg, "wifi_ip")) == 0;
	for (size_t i = 0; i < count; i++) {
		if (strcmp(out[i].id, id) == 0) {
			if (from_wifi)
				snprintf(out[i].host, sizeof(out[i].host), "%s", host);
			obs_data_release(msg);
			return count;
		}
	}
	if (count >= max) {
		obs_data_release(msg);
		return count;
	}

	struct pcam_found_phone *phone = &out[count];
	memset(phone, 0, sizeof(*phone));
	snprintf(phone->id, sizeof(phone->id), "%s", id);
	snprintf(phone->name, sizeof(phone->name), "%s", obs_data_get_string(msg, "name"));
	snprintf(phone->host, sizeof(phone->host), "%s", host);
	phone->port = (int)obs_data_get_int(msg, "port");
	if (phone->port <= 0 || phone->port > 65535)
		phone->port = PCAM_DISCOVERY_PORT;
	phone->busy = obs_data_get_bool(msg, "busy");
	obs_data_release(msg);
	return count + 1;
}

size_t pcam_discover(struct pcam_found_phone *out, size_t max, int timeout_ms)
{
	SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET)
		return 0;
	BOOL on = TRUE;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
	struct sockaddr_in any = {0};
	any.sin_family = AF_INET;
	if (bind(sock, (struct sockaddr *)&any, sizeof(any)) != 0) {
		closesocket(sock);
		return 0;
	}

	ULONG targets[MAX_TARGETS];
	size_t target_count = broadcast_targets(targets, MAX_TARGETS);
	send_requests(sock, targets, target_count);

	size_t count = 0;
	uint64_t start = os_gettime_ns();
	uint64_t deadline = start + (uint64_t)timeout_ms * 1000000ULL;
	bool resent = false;
	char buffer[1024];
	for (;;) {
		uint64_t now = os_gettime_ns();
		if (now >= deadline)
			break;
		/* UDP는 잃어버릴 수 있으므로 한 번 더 보낸다. */
		if (!resent && now - start >= RESEND_MS * 1000000ULL) {
			send_requests(sock, targets, target_count);
			resent = true;
		}
		uint64_t wait_us = (deadline - now) / 1000;
		if (!resent && wait_us > RESEND_MS * 1000)
			wait_us = RESEND_MS * 1000 - (now - start) / 1000;
		struct timeval tv = {(long)(wait_us / 1000000), (long)(wait_us % 1000000)};
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(sock, &fds);
		if (select(0, &fds, NULL, NULL, &tv) <= 0)
			continue;

		struct sockaddr_in from;
		int from_len = sizeof(from);
		int received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&from, &from_len);
		if (received <= 0)
			continue;
		buffer[received] = 0;
		count = add_reply(out, count, max, buffer, &from);
	}
	closesocket(sock);
	return count;
}
