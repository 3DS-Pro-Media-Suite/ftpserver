// Socket helpers for the FTP server. The app's threads share one libctru socket service session, which
// serves one request at a time, so nothing waits inside it: poll() only checks, and the thread naps.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define NANOSECONDS_PER_MILLISECOND 1000000LL

void ftp_socket_nap(int milliseconds);

void ftp_socket_set_nonblocking(int fd);

// errno says the call would have blocked: try again later.
bool ftp_socket_would_block(void);

// Binds to the port on every address and listens.
bool ftp_socket_listen(int fd, int port, int backlog);

// Waits until poll() reports the events; false when the wait timed out or the server is stopping.
bool ftp_socket_wait(int fd, short events, int timeoutMs);

// false when the connection failed, stalled too long, or the server is stopping.
bool ftp_socket_send_all(int fd, const char *bytes, size_t length);

// Sends a FIN after the data and closes once the client has hung up (or after a short linger).
void ftp_socket_close_gently(int fd);
