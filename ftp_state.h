// What the server thread and the session threads share while the FTP server runs.
#pragma once
#include "ftp.h"
#include <stdbool.h>

// Thread priorities relative to the thread that called ftp_start; on the 3DS a lower number runs first.
#define FTP_PRIORITY_ABOVE_MAIN (-1)
#define FTP_PRIORITY_BELOW_MAIN 1
#define FTP_THREAD_CORE (-2) // libctru: the application's default core

// Copies the config and notes the calling thread's priority.
void ftp_state_setup(const FtpConfig *config);

const FtpConfig *ftp_state_config(void);

// false makes every server loop and every socket wait end.
void ftp_state_set_running(bool running);

bool ftp_state_running(void);

int ftp_state_priority(int offsetFromMain);

// false when every session slot is taken.
bool ftp_state_reserve_session(void);

void ftp_state_release_session(void);

// Session threads still running, closing ones included.
int ftp_state_session_count(void);

// The passive data ports after the control port, in rotation.
int ftp_state_next_data_port(void);
