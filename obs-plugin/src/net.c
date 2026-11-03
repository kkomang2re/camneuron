#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET raw_socket_t;
#define RAW_INVALID INVALID_SOCKET
#define close_raw closesocket
#define SHUTDOWN_BOTH SD_BOTH
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int raw_socket_t;
#define RAW_INVALID (-1)
#define close_raw close
#define SHUTDOWN_BOTH SHUT_RDWR
#endif

#include <limits.h>
#include <stdio.h>

#include "net.h"

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#define RECV_BUFFER_BYTES (2 * 1024 * 1024)

bool pcam_net_init(void)
{
#ifdef _WIN32
	WSADATA wsa;
	return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
	return true;
#endif
}

void pcam_net_free(void)
{
#ifdef _WIN32
	WSACleanup();
#endif
}

static bool set_blocking(raw_socket_t sock, bool blocking)
{
#ifdef _WIN32
	u_long non_blocking = blocking ? 0 : 1;
	return ioctlsocket(sock, FIONBIO, &non_blocking) == 0;
#else
	int flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0)
		return false;
	flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
	return fcntl(sock, F_SETFL, flags) == 0;
#endif
}

static bool connect_in_progress(void)
{
#ifdef _WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EINPROGRESS;
#endif
}

static bool wait_connected(raw_socket_t sock, int timeout_ms)
{
	fd_set write_fds, error_fds;
	FD_ZERO(&write_fds);
	FD_ZERO(&error_fds);
	FD_SET(sock, &write_fds);
	FD_SET(sock, &error_fds);

	struct timeval timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
	if (select((int)sock + 1, NULL, &write_fds, &error_fds, &timeout) <= 0)
		return false;

	int error = 0;
	socklen_t len = sizeof(error);
	if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&error, &len) != 0)
		return false;
	return error == 0;
}

static void set_recv_timeout(raw_socket_t sock, int timeout_ms)
{
#ifdef _WIN32
	DWORD timeout = (DWORD)timeout_ms;
#else
	struct timeval timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
#endif
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
}

pcam_socket_t pcam_connect(const char *host, int port, int connect_timeout_ms, int recv_timeout_ms)
{
	struct addrinfo hints = {0};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	char port_str[8];
	snprintf(port_str, sizeof(port_str), "%d", port);

	struct addrinfo *addresses = NULL;
	if (getaddrinfo(host, port_str, &hints, &addresses) != 0)
		return PCAM_INVALID_SOCKET;

	pcam_socket_t result = PCAM_INVALID_SOCKET;
	for (struct addrinfo *ai = addresses; ai; ai = ai->ai_next) {
		raw_socket_t sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (sock == RAW_INVALID)
			continue;

		/* 연결 시간 제한을 걸기 위해 non-blocking으로 접속한 뒤 다시 blocking으로 돌린다. */
		bool connected = false;
		if (set_blocking(sock, false)) {
			int ret = connect(sock, ai->ai_addr, (int)ai->ai_addrlen);
			connected = ret == 0 || (connect_in_progress() && wait_connected(sock, connect_timeout_ms));
		}

		if (connected && set_blocking(sock, true)) {
			int one = 1;
			int recv_buffer = RECV_BUFFER_BYTES;
			setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
			setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&recv_buffer, sizeof(recv_buffer));
			set_recv_timeout(sock, recv_timeout_ms);
			result = (pcam_socket_t)sock;
			break;
		}
		close_raw(sock);
	}

	freeaddrinfo(addresses);
	return result;
}

bool pcam_send_all(pcam_socket_t sock, const void *data, size_t size)
{
	const char *ptr = data;
	while (size > 0) {
		int chunk = size > INT_MAX ? INT_MAX : (int)size;
		int sent = (int)send((raw_socket_t)sock, ptr, chunk, SEND_FLAGS);
		if (sent <= 0)
			return false;
		ptr += sent;
		size -= (size_t)sent;
	}
	return true;
}

bool pcam_recv_all(pcam_socket_t sock, void *data, size_t size)
{
	char *ptr = data;
	while (size > 0) {
		int chunk = size > INT_MAX ? INT_MAX : (int)size;
		int received = (int)recv((raw_socket_t)sock, ptr, chunk, 0);
		if (received <= 0)
			return false; /* 0 = 상대가 닫음, 음수 = 오류 또는 시간 초과 */
		ptr += received;
		size -= (size_t)received;
	}
	return true;
}

void pcam_shutdown(pcam_socket_t sock)
{
	if (sock != PCAM_INVALID_SOCKET)
		shutdown((raw_socket_t)sock, SHUTDOWN_BOTH);
}

void pcam_close(pcam_socket_t sock)
{
	if (sock != PCAM_INVALID_SOCKET)
		close_raw((raw_socket_t)sock);
}
