// FTP data connections, passive mode only, and the listings and files sent over them.
#pragma once
#include <stdbool.h>
#include <stdio.h>

// Opens a passive listener on the next free data port into *listener, closing the one before.
// Returns the port, or -1.
int ftp_data_listen(int *listener);

// Waits for the client to connect to *listener, which is closed either way. Returns the connection, or -1.
int ftp_data_accept(int *listener);

// Closes *listener if it is open and marks it closed (-1).
void ftp_data_close_listener(int *listener);

// An ls-style listing (longFormat) or the names only. false when sending failed.
bool ftp_data_send_listing(int connection, const char *folderPath, bool longFormat);

// The rest of the file. false when reading or sending failed.
bool ftp_data_send_file(int connection, FILE *file);
