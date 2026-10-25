#include "ftp_socket.h"
#include "ftp_state.h"
#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define READ_WAIT_NAP_MS 10
// poll(POLLOUT) on the 3DS wakes up late: a full send buffer is waited out in short naps.
#define WRITE_WAIT_NAP_MS 1
#define SEND_CHUNK_MAX (16 * 1024)
#define SEND_STALL_TIMEOUT_MS (15 * 1000)
#define CLOSE_LINGER_MS 3000
#define CLOSE_DRAIN_SIZE 256

void ftp_socket_nap(int milliseconds)
{
	svcSleepThread((s64)milliseconds * NANOSECONDS_PER_MILLISECOND);
}

void ftp_socket_set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool ftp_socket_would_block(void)
{
	return errno == EAGAIN || errno == EWOULDBLOCK;
}

bool ftp_socket_listen(int fd, int port, int backlog)
{
	struct sockaddr_in address;
	memset(&address, 0, sizeof address);
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = INADDR_ANY;
	bool bound = bind(fd, (struct sockaddr *)&address, sizeof address) == 0;
	return bound && listen(fd, backlog) == 0;
}

bool ftp_socket_wait(int fd, short events, int timeoutMs)
{
	u64 deadline = osGetTime() + (u64)timeoutMs;
	int napMs = (events & POLLOUT) ? WRITE_WAIT_NAP_MS : READ_WAIT_NAP_MS;
	while (ftp_state_running()) {
		struct pollfd request = {.fd = fd, .events = events, .revents = 0};
		int ready = poll(&request, 1, 0);
		if (ready > 0) {
			return true;
		}
		if (ready < 0 || osGetTime() >= deadline) {
			return false;
		}
		ftp_socket_nap(napMs);
	}
	return false;
}

bool ftp_socket_send_all(int fd, const char *bytes, size_t length)
{
	int stalledMs = 0;
	while (length > 0) {
		size_t chunk = length < SEND_CHUNK_MAX ? length : SEND_CHUNK_MAX;
		int sent = send(fd, bytes, chunk, 0);
		if (sent < 0 && ftp_socket_would_block()) {
			stalledMs += WRITE_WAIT_NAP_MS;
			if (!ftp_state_running() || stalledMs > SEND_STALL_TIMEOUT_MS) {
				return false;
			}
			ftp_socket_nap(WRITE_WAIT_NAP_MS);
			continue;
		}
		if (sent <= 0) {
			return false;
		}
		stalledMs = 0;
		bytes += sent;
		length -= sent;
	}
	return true;
}

// close() right after the last send drops what the 3DS has not sent yet, so the client hangs up first.
void ftp_socket_close_gently(int fd)
{
	shutdown(fd, SHUT_WR);
	char drained[CLOSE_DRAIN_SIZE];
	u64 deadline = osGetTime() + CLOSE_LINGER_MS;
	for (;;) {
		u64 now = osGetTime();
		if (now >= deadline || !ftp_socket_wait(fd, POLLIN, (int)(deadline - now))) {
			break;
		}
		int received = recv(fd, drained, sizeof drained, 0);
		bool clientHungUp = received == 0;
		bool failed = received < 0 && !ftp_socket_would_block();
		if (clientHungUp || failed) {
			break;
		}
	}
	close(fd);
}
