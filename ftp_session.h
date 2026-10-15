// One FTP client's control connection, on a thread of its own: commands in, replies out.
#pragma once

// Starts a session thread for the client, or turns the client away when every session slot is taken.
// The session owns the connection from here on.
void ftp_session_start(int connection);
