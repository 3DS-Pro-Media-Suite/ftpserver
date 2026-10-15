#include "ftp_session.h"
#include "ftp_data.h"
#include "ftp_socket.h"
#include "ftp_state.h"
#include "ftp_tree.h"
#include <3ds.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

#define SESSION_STACK_SIZE 0x4000
#define IDLE_TIMEOUT_MS (5 * 60 * 1000)
#define CONTROL_BUFFER_SIZE 512
#define COMMAND_LINE_MAX 300
#define REPLY_LINE_MAX 200
#define LINE_END "\r\n"
#define LINE_END_LENGTH 2
#define FEATURE_BLOCK_SIZE 128
#define MDTM_TIME_SIZE 24
#define MDTM_FORMAT "%Y%m%d%H%M%S"
#define MDTM_UNKNOWN "19700101000000"

// FEAT's list: one feature per line, each line starting with a space.
#define FEATURE_LINES " EPSV\r\n MDTM\r\n SIZE\r\n REST STREAM\r\n XCRC\r\n UTF8\r\n"

enum FtpReply {
	FTP_REPLY_OPENING_DATA = 150,
	FTP_REPLY_OK = 200,
	FTP_REPLY_SYSTEM_STATUS = 211,
	FTP_REPLY_FILE_STATUS = 213,
	FTP_REPLY_SYSTEM_TYPE = 215,
	FTP_REPLY_READY = 220,
	FTP_REPLY_GOODBYE = 221,
	FTP_REPLY_TRANSFER_DONE = 226,
	FTP_REPLY_PASSIVE = 227,
	FTP_REPLY_EXTENDED_PASSIVE = 229,
	FTP_REPLY_LOGGED_IN = 230,
	FTP_REPLY_FILE_ACTION_DONE = 250,
	FTP_REPLY_PATH_NAME = 257,
	FTP_REPLY_NEED_PASSWORD = 331,
	FTP_REPLY_PENDING = 350,
	FTP_REPLY_TOO_MANY_CONNECTIONS = 421,
	FTP_REPLY_NO_DATA_CONNECTION = 425,
	FTP_REPLY_TRANSFER_ABORTED = 426,
	FTP_REPLY_FILE_BUSY = 450,
	FTP_REPLY_BAD_ARGUMENT = 501,
	FTP_REPLY_NOT_IMPLEMENTED = 502,
	FTP_REPLY_UNAVAILABLE = 550,
};

typedef struct {
	int control; // the control connection
	int passiveListener; // opened by PASV or EPSV, used by the next transfer; -1 when none
	char input[CONTROL_BUFFER_SIZE]; // received on the control connection
	int inputStart; // the unread bytes are input[inputStart .. inputStart + inputLength)
	int inputLength;
	char folder[FTP_NAME_MAX]; // "" for the root, else the subfolder the client is in
	off_t restartOffset; // REST: where the next RETR starts
	bool quitRequested;
} Session;

typedef void (*CommandHandler)(Session *session, const char *argument);

typedef struct {
	const char *name;
	CommandHandler handler;
} Command;

static void reply(int connection, enum FtpReply code, const char *format, ...)
        __attribute__((format(printf, 3, 4)));

static void reply(int connection, enum FtpReply code, const char *format, ...)
{
	char line[REPLY_LINE_MAX];
	int length = snprintf(line, sizeof line, "%d ", (int)code);
	va_list arguments;
	va_start(arguments, format);
	int textLength = vsnprintf(line + length, sizeof line - length - LINE_END_LENGTH, format, arguments);
	va_end(arguments);
	if (textLength < 0) {
		return;
	}
	// a reply that is too long is cut, keeping room for its line end
	int longest = (int)sizeof line - LINE_END_LENGTH - 1;
	length += textLength;
	if (length > longest) {
		length = longest;
	}
	memcpy(line + length, LINE_END, LINE_END_LENGTH);
	ftp_socket_send_all(connection, line, length + LINE_END_LENGTH);
}

// Reading commands

// Moves the next buffered line into `line`, cut to its size; false when no whole line is buffered yet.
static bool take_buffered_line(Session *session, char *line, size_t lineSize)
{
	char *start = session->input + session->inputStart;
	char *newline = memchr(start, '\n', session->inputLength);
	if (!newline) {
		return false;
	}
	size_t lineLength = newline - start;
	size_t copied = lineLength < lineSize - 1 ? lineLength : lineSize - 1;
	memcpy(line, start, copied);
	while (copied > 0 && line[copied - 1] == '\r') {
		copied--;
	}
	line[copied] = '\0';
	session->inputStart += lineLength + 1;
	session->inputLength -= lineLength + 1;
	return true;
}

// false when the client closed the connection, was idle too long, or the server is stopping.
static bool receive_more_input(Session *session)
{
	memmove(session->input, session->input + session->inputStart, session->inputLength);
	session->inputStart = 0;
	if (session->inputLength == CONTROL_BUFFER_SIZE) {
		session->inputLength = 0; // a line longer than the buffer is garbage
	}
	if (!ftp_socket_wait(session->control, POLLIN, IDLE_TIMEOUT_MS)) {
		return false;
	}
	char *space = session->input + session->inputLength;
	int received = recv(session->control, space, CONTROL_BUFFER_SIZE - session->inputLength, 0);
	if (received > 0) {
		session->inputLength += received;
		return true;
	}
	return received < 0 && ftp_socket_would_block();
}

static bool read_command_line(Session *session, char *line, size_t lineSize)
{
	while (!take_buffered_line(session, line, lineSize)) {
		if (!receive_more_input(session)) {
			return false;
		}
	}
	return true;
}

// Paths in the served tree, from the session's folder

static int resolve(const Session *session, const char *clientPath, FtpPath *path)
{
	return ftp_tree_resolve(ftp_state_config()->rootPath, session->folder, clientPath, path);
}

static bool resolve_file(const Session *session, const char *clientPath, FtpPath *path)
{
	return resolve(session, clientPath, path) >= FTP_PATH_ROOT_ENTRY && ftp_tree_is_file(path->cardPath);
}

// A folder a client can enter or list: the root or one of its subfolders.
static bool resolve_folder(const Session *session, const char *clientPath, FtpPath *path)
{
	int depth = resolve(session, clientPath, path);
	bool listable = depth == FTP_PATH_ROOT || depth == FTP_PATH_ROOT_ENTRY;
	return listable && ftp_tree_is_folder(path->cardPath);
}

// A subfolder of the root, the only kind of folder that can be removed.
static bool resolve_subfolder(const Session *session, const char *clientPath, FtpPath *path)
{
	return resolve(session, clientPath, path) == FTP_PATH_ROOT_ENTRY && ftp_tree_is_folder(path->cardPath);
}

static FILE *open_file(const Session *session, const char *clientPath, off_t offset)
{
	FtpPath path;
	if (!resolve_file(session, clientPath, &path)) {
		return NULL;
	}
	return ftp_tree_open_file(path.cardPath, offset);
}

// The app's say on deleting a card path; false after telling the client why not.
static bool deletion_allowed(Session *session, const char *cardPath)
{
	const FtpConfig *config = ftp_state_config();
	if (config->canDelete && !config->canDelete(cardPath)) {
		reply(session->control, FTP_REPLY_UNAVAILABLE, "Deleting that is not allowed");
		return false;
	}
	if (config->isBusy && config->isBusy(cardPath)) {
		reply(session->control, FTP_REPLY_FILE_BUSY, "Still being saved: try again");
		return false;
	}
	return true;
}

// zlib's CRC-32 of the whole file. It is CPU work, so the thread drops below the main thread meanwhile.
static bool file_crc32(FILE *file, uLong *crc)
{
	u8 *chunk = malloc(FTP_CARD_READ_SIZE);
	if (!chunk) {
		return false;
	}
	svcSetThreadPriority(CUR_THREAD_HANDLE, ftp_state_priority(FTP_PRIORITY_BELOW_MAIN));
	*crc = crc32(0L, Z_NULL, 0);
	size_t length;
	while ((length = fread(chunk, 1, FTP_CARD_READ_SIZE, file)) > 0) {
		*crc = crc32(*crc, chunk, (uInt)length);
	}
	svcSetThreadPriority(CUR_THREAD_HANDLE, ftp_state_priority(FTP_PRIORITY_ABOVE_MAIN));
	free(chunk);
	return !ferror(file);
}

// "LIST -al": ls options are skipped; a folder may follow them.
static const char *skip_list_options(const char *argument)
{
	while (*argument == '-') {
		argument += strcspn(argument, " ");
		argument += strspn(argument, " ");
	}
	return argument;
}

static void transfer_listing(Session *session, const char *argument, bool longFormat)
{
	session->restartOffset = 0;
	FtpPath folder;
	if (!resolve_folder(session, skip_list_options(argument), &folder)) {
		ftp_data_close_listener(&session->passiveListener);
		reply(session->control, FTP_REPLY_UNAVAILABLE, "No such folder");
		return;
	}
	reply(session->control, FTP_REPLY_OPENING_DATA, "Here comes the listing");
	int connection = ftp_data_accept(&session->passiveListener);
	if (connection < 0) {
		reply(session->control, FTP_REPLY_NO_DATA_CONNECTION, "Can't open data connection");
		return;
	}
	bool sent = ftp_data_send_listing(connection, folder.cardPath, longFormat);
	ftp_socket_close_gently(connection);
	if (sent) {
		reply(session->control, FTP_REPLY_TRANSFER_DONE, "Done");
	} else {
		reply(session->control, FTP_REPLY_TRANSFER_ABORTED, "Listing aborted");
	}
}

// Command handlers, one per command (aliases share one)

static void cmd_user(Session *session, const char *argument)
{
	(void)argument;
	reply(session->control, FTP_REPLY_NEED_PASSWORD, "Any password will do");
}

static void cmd_password(Session *session, const char *argument)
{
	(void)argument;
	reply(session->control, FTP_REPLY_LOGGED_IN, "Logged in");
}

static void cmd_system(Session *session, const char *argument)
{
	(void)argument;
	reply(session->control, FTP_REPLY_SYSTEM_TYPE, "UNIX Type: L8");
}

static void cmd_features(Session *session, const char *argument)
{
	(void)argument;
	char block[FEATURE_BLOCK_SIZE];
	int length =
	        snprintf(block, sizeof block, "%d-Features:" LINE_END FEATURE_LINES, FTP_REPLY_SYSTEM_STATUS);
	if (length > 0 && (size_t)length < sizeof block) {
		ftp_socket_send_all(session->control, block, length);
	}
	reply(session->control, FTP_REPLY_SYSTEM_STATUS, "End");
}

// Settings with only one possible value (TYPE, MODE, STRU, OPTS, CLNT) and NOOP.
static void cmd_accept(Session *session, const char *argument)
{
	(void)argument;
	reply(session->control, FTP_REPLY_OK, "OK");
}

static void cmd_print_folder(Session *session, const char *argument)
{
	(void)argument;
	reply(session->control, FTP_REPLY_PATH_NAME, "\"/%s\"", session->folder);
}

static void cmd_change_folder(Session *session, const char *argument)
{
	FtpPath folder;
	if (!resolve_folder(session, argument, &folder)) {
		reply(session->control, FTP_REPLY_UNAVAILABLE, "No such folder");
		return;
	}
	snprintf(session->folder, sizeof session->folder, "%s", folder.name);
	reply(session->control, FTP_REPLY_FILE_ACTION_DONE, "Now in /%s", session->folder);
}

// The tree has one level of subfolders, so up is always the root.
static void cmd_folder_up(Session *session, const char *argument)
{
	(void)argument;
	session->folder[0] = '\0';
	reply(session->control, FTP_REPLY_FILE_ACTION_DONE, "Now in /");
}

static void cmd_delete(Session *session, const char *argument)
{
	FtpPath file;
	if (!resolve_file(session, argument, &file)) {
		reply(session->control, FTP_REPLY_UNAVAILABLE, "No such file");
		return;
	}
	if (!deletion_allowed(session, file.cardPath)) {
		return;
	}
	if (remove(file.cardPath) != 0) {
		reply(session->control, FTP_REPLY_UNAVAILABLE, "Could not delete it");
		return;
	}
	reply(session->control, FTP_REPLY_FILE_ACTION_DONE, "Deleted");
}

static void cmd_remove_folder(Session *session, const char *argument)
{
	FtpPath folder;
	if (!resolve_subfolder(session, argument, &folder)) {
		reply(session->control, FTP_REPLY_UNAVAILABLE, "No such folder");
		return;
	}
	if (!deletion_allowed(session, folder.cardPath)) {
		return;
	}
	if (!ftp_state_config()->removeFolderContents && !ftp_tree_folder_is_empty(folder.cardPath)) {
		reply(session->control, FTP_REPLY_UNAVAILABLE, "Folder is not empty");
		return;
	}
	if (!ftp_tree_remove_folder(folder.cardPath)) {
		reply(session->control, FTP_REPLY_UNAVAILABLE, "Could not delete all of it");
		return;
	}
	reply(session->control, FTP_REPLY_FILE_ACTION_DONE, "Deleted");
}

static void cmd_passive(Session *session, const char *argument)
{
	(void)argument;
	int port = ftp_data_listen(&session->passiveListener);
	if (port < 0) {
		reply(session->control, FTP_REPLY_NO_DATA_CONNECTION, "Can't open passive port");
		return;
	}
	struct in_addr hostAddress = {.s_addr = (u32)gethostid()};
	const u8 *address = (const u8 *)&hostAddress.s_addr; // network order: the first byte is the first number
	// PASV gives the port as two bytes, high byte first
	reply(session->control, FTP_REPLY_PASSIVE, "Entering Passive Mode (%u,%u,%u,%u,%u,%u)", address[0],
	      address[1], address[2], address[3], (unsigned)(port >> 8), (unsigned)(port & 0xFF));
}

static void cmd_extended_passive(Session *session, const char *argument)
{
	(void)argument;
	int port = ftp_data_listen(&session->passiveListener);
	if (port < 0) {
		reply(session->control, FTP_REPLY_NO_DATA_CONNECTION, "Can't open passive port");
		return;
	}
	reply(session->control, FTP_REPLY_EXTENDED_PASSIVE, "Entering Extended Passive Mode (|||%d|)", port);
}

static void cmd_active(Session *session, const char *argument)
{
	(void)argument;
	reply(session->control, FTP_REPLY_NOT_IMPLEMENTED, "Passive mode only: switch the client to passive");
}

static void cmd_restart(Session *session, const char *argument)
{
	char *end = NULL;
	errno = 0;
	long long offset = strtoll(argument, &end, 10);
	bool isNumber = end != argument && *end == '\0' && errno != ERANGE;
	if (!isNumber || offset < 0) {
		reply(session->control, FTP_REPLY_BAD_ARGUMENT, "REST needs a byte offset");
		return;
	}
	session->restartOffset = (off_t)offset;
	reply(session->control, FTP_REPLY_PENDING, "Restarting at %lld", offset);
}

static void cmd_list(Session *session, const char *argument)
{
	transfer_listing(session, argument, true);
}

static void cmd_name_list(Session *session, const char *argument)
{
	transfer_listing(session, argument, false);
}

static void cmd_retrieve(Session *session, const char *argument)
{
	off_t startOffset = session->restartOffset;
	session->restartOffset = 0;
	FILE *file = open_file(session, argument, startOffset);
	if (!file) {
		ftp_data_close_listener(&session->passiveListener);
		reply(session->control, FTP_REPLY_UNAVAILABLE, "No such file");
		return;
	}
	reply(session->control, FTP_REPLY_OPENING_DATA, "Sending");
	int connection = ftp_data_accept(&session->passiveListener);
	if (connection < 0) {
		fclose(file);
		reply(session->control, FTP_REPLY_NO_DATA_CONNECTION, "Can't open data connection");
		return;
	}
	bool sent = ftp_data_send_file(connection, file);
	fclose(file);
	ftp_socket_close_gently(connection);
	if (sent) {
		reply(session->control, FTP_REPLY_TRANSFER_DONE, "Transfer complete");
	} else {
		reply(session->control, FTP_REPLY_TRANSFER_ABORTED, "Transfer aborted");
	}
}

// XCRC: lets a client check a download, resumed ones included, before it deletes the file.
static void cmd_checksum(Session *session, const char *argument)
{
	FILE *file = open_file(session, argument, 0);
	if (!file) {
		reply(session->control, FTP_REPLY_UNAVAILABLE, "No such file");
		return;
	}
	uLong crc = 0;
	bool readAll = file_crc32(file, &crc);
	fclose(file);
	if (!readAll) {
		reply(session->control, FTP_REPLY_UNAVAILABLE, "Could not read it");
		return;
	}
	reply(session->control, FTP_REPLY_FILE_ACTION_DONE, "%08lX", (unsigned long)crc);
}

static void cmd_size(Session *session, const char *argument)
{
	FtpPath file;
	struct stat info;
	if (!resolve_file(session, argument, &file) || stat(file.cardPath, &info) != 0) {
		reply(session->control, FTP_REPLY_UNAVAILABLE, "No such file");
		return;
	}
	reply(session->control, FTP_REPLY_FILE_STATUS, "%llu", (unsigned long long)info.st_size);
}

// The console's local time as it is: clients take it for the time the file was made.
static void cmd_modified_time(Session *session, const char *argument)
{
	FtpPath file;
	if (!resolve_file(session, argument, &file)) {
		reply(session->control, FTP_REPLY_UNAVAILABLE, "No such file");
		return;
	}
	char text[MDTM_TIME_SIZE] = MDTM_UNKNOWN;
	time_t modified = ftp_tree_modified_time(file.cardPath);
	struct tm parts;
	if (gmtime_r(&modified, &parts)) {
		strftime(text, sizeof text, MDTM_FORMAT, &parts);
	}
	reply(session->control, FTP_REPLY_FILE_STATUS, "%s", text);
}

static void cmd_quit(Session *session, const char *argument)
{
	(void)argument;
	reply(session->control, FTP_REPLY_GOODBYE, "Bye");
	session->quitRequested = true;
}

// Uploads, new folders and renames.
static void cmd_read_only(Session *session, const char *argument)
{
	(void)argument;
	reply(session->control, FTP_REPLY_UNAVAILABLE, "Downloading and deleting only");
}

// clang-format off
static const Command COMMANDS[] = {
	{"USER", cmd_user},
	{"PASS", cmd_password},
	{"SYST", cmd_system},
	{"FEAT", cmd_features},
	{"OPTS", cmd_accept},
	{"TYPE", cmd_accept},
	{"MODE", cmd_accept},
	{"STRU", cmd_accept},
	{"NOOP", cmd_accept},
	{"CLNT", cmd_accept},
	{"PWD", cmd_print_folder},
	{"XPWD", cmd_print_folder},
	{"CWD", cmd_change_folder},
	{"XCWD", cmd_change_folder},
	{"CDUP", cmd_folder_up},
	{"XCUP", cmd_folder_up},
	{"DELE", cmd_delete},
	{"RMD", cmd_remove_folder},
	{"XRMD", cmd_remove_folder},
	{"PASV", cmd_passive},
	{"EPSV", cmd_extended_passive},
	{"PORT", cmd_active},
	{"EPRT", cmd_active},
	{"REST", cmd_restart},
	{"LIST", cmd_list},
	{"NLST", cmd_name_list},
	{"RETR", cmd_retrieve},
	{"XCRC", cmd_checksum},
	{"SIZE", cmd_size},
	{"MDTM", cmd_modified_time},
	{"QUIT", cmd_quit},
	{"STOR", cmd_read_only},
	{"APPE", cmd_read_only},
	{"MKD", cmd_read_only},
	{"RNFR", cmd_read_only},
	{"RNTO", cmd_read_only},
};
// clang-format on

static const Command *find_command(const char *name)
{
	for (size_t i = 0; i < ARRAY_COUNT(COMMANDS); i++) {
		if (strcmp(COMMANDS[i].name, name) == 0) {
			return &COMMANDS[i];
		}
	}
	return NULL;
}

// Splits "COMMAND argument" in place and returns the argument. The command word is upper-cased; the argument
// keeps its case and spaces.
static const char *split_command_line(char *line)
{
	char *argument = strchr(line, ' ');
	if (argument) {
		*argument = '\0';
		argument++;
	} else {
		argument = line + strlen(line);
	}
	for (char *letter = line; *letter != '\0'; letter++) {
		*letter = (char)toupper((unsigned char)*letter);
	}
	return argument;
}

// The session

static void run_session(Session *session)
{
	char line[COMMAND_LINE_MAX];
	reply(session->control, FTP_REPLY_READY, "%s", ftp_state_config()->greeting);
	while (ftp_state_running() && !session->quitRequested && read_command_line(session, line, sizeof line)) {
		const char *argument = split_command_line(line);
		const Command *command = find_command(line);
		if (command) {
			command->handler(session, argument);
		} else {
			reply(session->control, FTP_REPLY_NOT_IMPLEMENTED, "Not supported");
		}
	}
	ftp_data_close_listener(&session->passiveListener);
}

static void session_thread(void *argument)
{
	Session *session = argument;
	run_session(session);
	ftp_socket_close_gently(session->control);
	free(session);
	ftp_state_release_session();
}

void ftp_session_start(int connection)
{
	ftp_socket_set_nonblocking(connection);
	Session *session = calloc(1, sizeof *session);
	bool reserved = session && ftp_state_reserve_session();
	Thread thread = NULL;
	if (reserved) {
		session->control = connection;
		session->passiveListener = -1;
		// Above the main thread: a session mostly waits, and what it has to send should not queue behind the
		// UI's frame.
		int priority = ftp_state_priority(FTP_PRIORITY_ABOVE_MAIN);
		thread = threadCreate(session_thread, session, SESSION_STACK_SIZE, priority, FTP_THREAD_CORE, true);
	}
	if (thread) {
		return;
	}
	if (reserved) {
		ftp_state_release_session();
	}
	free(session);
	reply(connection, FTP_REPLY_TOO_MANY_CONNECTIONS, "Too many connections");
	ftp_socket_close_gently(connection);
}
