#include "ftp_data.h"
#include "ftp_socket.h"
#include "ftp_state.h"
#include "ftp_tree.h"
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PASSIVE_PORT_TRIES 8
#define PASSIVE_BACKLOG 1
#define DATA_SOCKET_BUFFER_SIZE (32 * 1024)
#define DATA_CONNECT_TIMEOUT_MS (30 * 1000) // a lossy link can need a few SYN retries (1, 3, 7, 15 s)

#define LISTING_BUFFER_SIZE (8 * 1024)
#define LISTING_LINE_MAX 320
#define LISTING_LINE_END "\r\n"
#define LISTING_FOLDER_MODE "drwxr-xr-x"
#define LISTING_FILE_MODE "-rw-r--r--"
#define LISTING_OWNERSHIP "1 3ds 3ds" // link count, owner and group: expected by clients, used by none
#define LISTING_DATE_SIZE 24
#define LISTING_DATE_UNKNOWN "Jan  1  1970"
#define LISTING_RECENT_FORMAT "%b %e %H:%M"
#define LISTING_OLD_FORMAT "%b %e  %Y"
#define SECONDS_PER_DAY (24 * 60 * 60)
#define LISTING_RECENT_SECONDS (180 * SECONDS_PER_DAY) // ls shows the time of day for the last six months

typedef struct {
	int connection;
	char *bytes;
	size_t used;
} ListingBuffer;

void ftp_data_close_listener(int *listener)
{
	if (*listener >= 0) {
		close(*listener);
		*listener = -1;
	}
}

int ftp_data_listen(int *listener)
{
	ftp_data_close_listener(listener);
	for (int attempt = 0; attempt < PASSIVE_PORT_TRIES; attempt++) {
		int port = ftp_state_next_data_port();
		int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
		if (fd < 0) {
			return -1;
		}
		// set before bind, so the accepted data connection inherits them
		int bufferSize = DATA_SOCKET_BUFFER_SIZE;
		setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof bufferSize);
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof bufferSize);
		if (ftp_socket_listen(fd, port, PASSIVE_BACKLOG)) {
			ftp_socket_set_nonblocking(fd);
			*listener = fd;
			return port;
		}
		close(fd);
	}
	return -1;
}

int ftp_data_accept(int *listener)
{
	if (*listener < 0) {
		return -1;
	}
	int connection = -1;
	if (ftp_socket_wait(*listener, POLLIN, DATA_CONNECT_TIMEOUT_MS)) {
		connection = accept(*listener, NULL, NULL);
	}
	ftp_data_close_listener(listener);
	if (connection >= 0) {
		ftp_socket_set_nonblocking(connection);
	}
	return connection;
}

static void format_listing_date(time_t modified, time_t now, char *text, size_t textSize)
{
	snprintf(text, textSize, "%s", LISTING_DATE_UNKNOWN);
	struct tm parts;
	if (!gmtime_r(&modified, &parts)) {
		return;
	}
	bool recent = now - modified < LISTING_RECENT_SECONDS && modified <= now + SECONDS_PER_DAY;
	strftime(text, textSize, recent ? LISTING_RECENT_FORMAT : LISTING_OLD_FORMAT, &parts);
}

// One listing line for a folder entry; 0 when the entry is not shown.
static size_t format_listing_line(const char *folderPath, const char *name, bool longFormat, time_t now,
                                  char *line, size_t lineSize)
{
	if (name[0] == '.') {
		return 0; // hidden, and "." and ".."
	}
	char entryPath[FTP_PATH_MAX];
	struct stat info;
	if (!ftp_tree_join(entryPath, sizeof entryPath, folderPath, name) || stat(entryPath, &info) != 0) {
		return 0;
	}
	bool isFolder = S_ISDIR(info.st_mode);
	if (!isFolder && !S_ISREG(info.st_mode)) {
		return 0;
	}
	int length = 0;
	if (longFormat) {
		char date[LISTING_DATE_SIZE];
		format_listing_date(ftp_tree_modified_time(entryPath), now, date, sizeof date);
		const char *mode = isFolder ? LISTING_FOLDER_MODE : LISTING_FILE_MODE;
		unsigned long long size = isFolder ? 0 : (unsigned long long)info.st_size;
		length = snprintf(line, lineSize, "%s " LISTING_OWNERSHIP " %llu %s %s" LISTING_LINE_END, mode, size,
		                  date, name);
	} else {
		length = snprintf(line, lineSize, "%s" LISTING_LINE_END, name);
	}
	if (length <= 0 || (size_t)length >= lineSize) {
		return 0;
	}
	return (size_t)length;
}

static bool listing_flush(ListingBuffer *listing)
{
	bool sent = ftp_socket_send_all(listing->connection, listing->bytes, listing->used);
	listing->used = 0;
	return sent;
}

// Sends what is buffered first when the line does not fit; false when sending failed.
static bool listing_add(ListingBuffer *listing, const char *line, size_t length)
{
	bool fits = listing->used + length <= LISTING_BUFFER_SIZE;
	if (!fits && !listing_flush(listing)) {
		return false;
	}
	memcpy(listing->bytes + listing->used, line, length);
	listing->used += length;
	return true;
}

static bool add_folder_entries(ListingBuffer *listing, DIR *folder, const char *folderPath, bool longFormat)
{
	time_t now = time(NULL);
	struct dirent *entry;
	while ((entry = readdir(folder)) != NULL) {
		char line[LISTING_LINE_MAX];
		size_t length = format_listing_line(folderPath, entry->d_name, longFormat, now, line, sizeof line);
		if (length > 0 && !listing_add(listing, line, length)) {
			return false;
		}
	}
	return true;
}

// A folder that cannot be opened is listed as empty.
bool ftp_data_send_listing(int connection, const char *folderPath, bool longFormat)
{
	ListingBuffer listing = {.connection = connection, .bytes = malloc(LISTING_BUFFER_SIZE), .used = 0};
	if (!listing.bytes) {
		return false;
	}
	bool sent = true;
	DIR *folder = opendir(folderPath);
	if (folder) {
		sent = add_folder_entries(&listing, folder, folderPath, longFormat);
		closedir(folder);
	}
	if (sent) {
		sent = listing_flush(&listing);
	}
	free(listing.bytes);
	return sent;
}

bool ftp_data_send_file(int connection, FILE *file)
{
	char *chunk = malloc(FTP_CARD_READ_SIZE);
	if (!chunk) {
		return false;
	}
	size_t length;
	while ((length = fread(chunk, 1, FTP_CARD_READ_SIZE, file)) > 0) {
		if (!ftp_socket_send_all(connection, chunk, length)) {
			free(chunk);
			return false;
		}
	}
	free(chunk);
	return !ferror(file);
}
