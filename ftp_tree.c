#include "ftp_tree.h"
#include <3ds.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLIENT_PATH_MAX 200

static bool name_equals(const char *name, size_t length, const char *expected)
{
	return length == strlen(expected) && strncmp(name, expected, length) == 0;
}

static bool is_dot_entry(const char *name)
{
	return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

// Returns the depth, or FTP_PATH_OUTSIDE when the path does not fit.
static int build_card_path(const char *rootPath, char names[][FTP_NAME_MAX], int depth, FtpPath *path)
{
	int written = 0;
	if (depth == FTP_PATH_ROOT) {
		written = snprintf(path->cardPath, sizeof path->cardPath, "%s", rootPath);
	} else if (depth == FTP_PATH_ROOT_ENTRY) {
		written = snprintf(path->cardPath, sizeof path->cardPath, "%s/%s", rootPath, names[0]);
	} else {
		written = snprintf(path->cardPath, sizeof path->cardPath, "%s/%s/%s", rootPath, names[0], names[1]);
	}
	if (written < 0 || (size_t)written >= sizeof path->cardPath) {
		return FTP_PATH_OUTSIDE;
	}
	if (depth > FTP_PATH_ROOT) {
		snprintf(path->name, sizeof path->name, "%s", names[depth - 1]);
	}
	return depth;
}

int ftp_tree_resolve(const char *rootPath, const char *folder, const char *clientPath, FtpPath *path)
{
	path->cardPath[0] = '\0';
	path->name[0] = '\0';
	bool unsafe = strchr(clientPath, '\\') || strchr(clientPath, ':') || strlen(clientPath) > CLIENT_PATH_MAX;
	if (unsafe) {
		return FTP_PATH_OUTSIDE;
	}
	char names[FTP_TREE_DEPTH_MAX][FTP_NAME_MAX];
	int depth = FTP_PATH_ROOT;
	bool relative = clientPath[0] != '/';
	if (relative && folder[0] != '\0') {
		snprintf(names[depth], FTP_NAME_MAX, "%s", folder);
		depth++;
	}
	const char *name = clientPath + strspn(clientPath, "/");
	while (*name != '\0') {
		size_t length = strcspn(name, "/");
		if (name_equals(name, length, "..")) {
			if (depth > FTP_PATH_ROOT) {
				depth--;
			}
		} else if (!name_equals(name, length, ".")) {
			if (depth >= FTP_TREE_DEPTH_MAX || length >= FTP_NAME_MAX) {
				return FTP_PATH_OUTSIDE;
			}
			memcpy(names[depth], name, length);
			names[depth][length] = '\0';
			depth++;
		}
		name += length;
		name += strspn(name, "/");
	}
	return build_card_path(rootPath, names, depth, path);
}

bool ftp_tree_is_file(const char *cardPath)
{
	struct stat info;
	return stat(cardPath, &info) == 0 && S_ISREG(info.st_mode);
}

bool ftp_tree_is_folder(const char *cardPath)
{
	struct stat info;
	return stat(cardPath, &info) == 0 && S_ISDIR(info.st_mode);
}

bool ftp_tree_folder_is_empty(const char *cardPath)
{
	DIR *folder = opendir(cardPath);
	if (!folder) {
		return false;
	}
	bool empty = true;
	struct dirent *entry;
	while ((entry = readdir(folder)) != NULL) {
		if (!is_dot_entry(entry->d_name)) {
			empty = false;
			break;
		}
	}
	closedir(folder);
	return empty;
}

// The tree has no deeper levels, so a subfolder holds only files.
bool ftp_tree_remove_folder(const char *cardPath)
{
	DIR *folder = opendir(cardPath);
	if (!folder) {
		return false;
	}
	bool removedAll = true;
	struct dirent *entry;
	while ((entry = readdir(folder)) != NULL) {
		if (is_dot_entry(entry->d_name)) {
			continue;
		}
		char entryPath[FTP_PATH_MAX];
		bool joined = ftp_tree_join(entryPath, sizeof entryPath, cardPath, entry->d_name);
		bool removed = joined && remove(entryPath) == 0;
		removedAll = removedAll && removed;
	}
	closedir(folder);
	bool folderRemoved = rmdir(cardPath) == 0;
	return removedAll && folderRemoved;
}

// stat() leaves st_mtime at 0 on the SD card.
time_t ftp_tree_modified_time(const char *cardPath)
{
	u64 modified = 0;
	if (R_FAILED(archive_getmtime(cardPath, &modified))) {
		return 0;
	}
	return (time_t)modified;
}

FILE *ftp_tree_open_file(const char *cardPath, off_t offset)
{
	FILE *file = fopen(cardPath, "rb");
	if (!file) {
		return NULL;
	}
	// stdio's card buffer is 1 KB: unbuffered, every read is one card read
	setvbuf(file, NULL, _IONBF, 0);
	if (offset > 0 && fseeko(file, offset, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	return file;
}

bool ftp_tree_join(char *out, size_t outSize, const char *folder, const char *name)
{
	int written = snprintf(out, outSize, "%s/%s", folder, name);
	return written >= 0 && (size_t)written < outSize;
}
