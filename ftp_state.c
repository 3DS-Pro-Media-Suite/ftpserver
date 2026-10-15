#include "ftp_state.h"
#include <3ds.h>

#define MAX_SESSIONS 4 // FileZilla, WinSCP and Explorer browse on one connection and download on another
#define DEFAULT_MAIN_PRIORITY 0x30
#define DATA_PORT_OFFSET 1 // the data ports start right after the control port

static FtpConfig g_config;
static _Atomic bool g_running;
static s32 g_mainPriority;
static LightLock g_lock; // guards g_sessionCount and g_nextDataPort
static bool g_lockReady;
static int g_sessionCount;
static int g_nextDataPort; // index of the next data port to try

void ftp_state_setup(const FtpConfig *config)
{
	if (!g_lockReady) {
		LightLock_Init(&g_lock); // once: a session still closing from the last stop may hold it
		g_lockReady = true;
	}
	g_config = *config;
	g_mainPriority = DEFAULT_MAIN_PRIORITY;
	svcGetThreadPriority(&g_mainPriority, CUR_THREAD_HANDLE);
}

const FtpConfig *ftp_state_config(void)
{
	return &g_config;
}

void ftp_state_set_running(bool running)
{
	g_running = running;
}

bool ftp_state_running(void)
{
	return g_running;
}

int ftp_state_priority(int offsetFromMain)
{
	return g_mainPriority + offsetFromMain;
}

bool ftp_state_reserve_session(void)
{
	LightLock_Lock(&g_lock);
	bool reserved = g_sessionCount < MAX_SESSIONS;
	if (reserved) {
		g_sessionCount++;
	}
	LightLock_Unlock(&g_lock);
	return reserved;
}

void ftp_state_release_session(void)
{
	LightLock_Lock(&g_lock);
	g_sessionCount--;
	LightLock_Unlock(&g_lock);
}

int ftp_state_session_count(void)
{
	if (!g_lockReady) {
		return 0;
	}
	LightLock_Lock(&g_lock);
	int count = g_sessionCount;
	LightLock_Unlock(&g_lock);
	return count;
}

// A fresh port for every passive listener: sessions run side by side, and a port just closed may still be in
// TIME_WAIT.
int ftp_state_next_data_port(void)
{
	LightLock_Lock(&g_lock);
	int port = g_config.port + DATA_PORT_OFFSET + g_nextDataPort;
	g_nextDataPort = (g_nextDataPort + 1) % FTP_DATA_PORT_COUNT;
	LightLock_Unlock(&g_lock);
	return port;
}
