// The tree the FTP server serves: one root folder on the card and one level of subfolders below it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

#define FTP_PATH_MAX 300 // a card path, with its terminator
#define FTP_NAME_MAX 64 // one file or folder name, with its terminator
#define FTP_CARD_READ_SIZE (64 * 1024) // files are read in chunks this big, one card read each

// Where a client path points.
enum FtpPathDepth {
	FTP_PATH_OUTSIDE = -1, // malformed, too long, or deeper than the tree
	FTP_PATH_ROOT = 0,
	FTP_PATH_ROOT_ENTRY = 1, // a file or subfolder in the root
	FTP_PATH_SUBFOLDER_ENTRY = 2, // a file in a subfolder
};

#define FTP_TREE_DEPTH_MAX FTP_PATH_SUBFOLDER_ENTRY
// A root shorter than this leaves room for two names below it.
#define FTP_ROOT_PATH_MAX (FTP_PATH_MAX - FTP_TREE_DEPTH_MAX * FTP_NAME_MAX)

typedef struct {
	char cardPath[FTP_PATH_MAX];
	char name[FTP_NAME_MAX]; // the last name in the path; "" for the root
} FtpPath;

// A client path, absolute or relative to `folder` ("" for the root), as a path below rootPath.
// Returns an FtpPathDepth. "\", ":", over-long paths and names are outside; ".." stops at the root.
int ftp_tree_resolve(const char *rootPath, const char *folder, const char *clientPath, FtpPath *path);

bool ftp_tree_is_file(const char *cardPath);

bool ftp_tree_is_folder(const char *cardPath);

bool ftp_tree_folder_is_empty(const char *cardPath);

// The folder's files, then the folder; false when anything is left.
bool ftp_tree_remove_folder(const char *cardPath);

// Seconds since 1970 in the console's local time; 0 when unknown.
time_t ftp_tree_modified_time(const char *cardPath);

// Opened for reading at offset, unbuffered: read it in FTP_CARD_READ_SIZE chunks. NULL on failure.
FILE *ftp_tree_open_file(const char *cardPath, off_t offset);

// folder + "/" + name; false when it does not fit.
bool ftp_tree_join(char *out, size_t outSize, const char *folder, const char *name);
