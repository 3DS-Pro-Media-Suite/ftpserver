// FTP server for one folder on the SD card and one level of subfolders below it.
// A listener thread plus one thread per client. Anonymous login, passive mode only, download and delete only.
// Inside: ftp.c starts, stops and listens; ftp_session.c runs the commands; ftp_data.c sends listings and
// files; ftp_tree.c maps client paths to the card; ftp_socket.c and ftp_state.c hold what they share.
#pragma once
#include <stdbool.h>

#define FTP_DATA_PORT_COUNT 64 // passive data ports: the ports right after the control port, in rotation

// What an app serves. The struct is copied; its strings must stay valid while the server runs.
typedef struct {
	const char *greeting; // sent after "220 " when a client connects
	const char *rootPath; // the served folder on the card, without a trailing slash
	int port; // the control port; the data ports follow it
	bool removeFolderContents; // RMD deletes a subfolder with its files; false: only empty subfolders
	// Optional, called from session threads with the card path of the file or folder about to be deleted.
	bool (*canDelete)(const char *cardPath); // false: DELE and RMD answer 550. NULL: everything may go
	bool (*isBusy)(const char *cardPath); // true: DELE and RMD answer 450, try again. NULL: never busy
} FtpConfig;

// Starts serving; false when Wi-Fi, the socket service or the port is unavailable.
bool ftp_start(const FtpConfig *config);

// Ends every session; the socket service stays up while a session is still closing.
void ftp_stop(void);

// At app exit: ftp_stop, then the socket service goes down even if a session is still closing.
void ftp_exit(void);

// True between a successful ftp_start and ftp_stop, also while waiting for the network.
bool ftp_running(void);

// "192.168.x.y:port" while listening; NULL while waiting for the network (after a sleep) or stopped.
const char *ftp_address(void);
