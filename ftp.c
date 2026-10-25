#include "ftp.h"
#include "ftp_session.h"
#include "ftp_socket.h"
#include "ftp_state.h"
#include "ftp_tree.h"
#include <3ds.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CONTROL_BACKLOG 4
#define PORT_MAX 65535
#define ADDRESS_TEXT_SIZE 32

#define SOC_BUFFER_ALIGN 0x1000
#define SOC_BUFFER_SIZE 0x100000

#define SERVER_STACK_SIZE 0x2000
#define LISTENER_POLL_NAP_MS 10
#define NO_LISTENER_NAP_MS 100
#define ACCEPT_FAILURE_NAP_MS 100
#define ACCEPT_FAILURE_LIMIT 10
#define NETWORK_CHECK_INTERVAL_MS 1000
#define STOP_JOIN_TIMEOUT_MS 2000
#define STOP_SESSION_WAIT_MS 4000 // a closing session may wait a few seconds for its client to hang up
#define STOP_SESSION_POLL_MS 10

static Thread g_serverThread;
static int g_controlListener = -1;
static _Atomic bool g_listening; // g_controlListener is open and g_address says where
static _Atomic bool g_reopenListener; // the console woke up: its sockets are gone
static char g_address[ADDRESS_TEXT_SIZE];
static u32 *g_socketBuffer; // the socket service's memory while the service is up
static bool g_wifiStatusOpen; // ac:u is open for reading the Wi-Fi status
static bool g_exclusiveNetwork;
static aptHookCookie g_aptCookie;

// The control listener and the network

// Listens on the control port of the console's address; false without an address (no network).
static bool open_control_listener(void)
{
	long hostAddress = gethostid();
	if (hostAddress == 0) {
		return false;
	}
	int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (listener < 0) {
		return false;
	}
	int port = ftp_state_config()->port;
	int reuse = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
	if (!ftp_socket_listen(listener, port, CONTROL_BACKLOG)) {
		close(listener);
		return false;
	}
	ftp_socket_set_nonblocking(listener);
	struct in_addr address = {.s_addr = (u32)hostAddress};
	snprintf(g_address, sizeof g_address, "%s:%d", inet_ntoa(address), port);
	g_controlListener = listener;
	g_listening = true;
	return true;
}

static void close_control_listener(void)
{
	g_listening = false;
	if (g_controlListener >= 0) {
		close(g_controlListener);
	}
	g_controlListener = -1;
}

// Assumed connected when ac:u could not be opened.
static bool wifi_connected(void)
{
	if (!g_wifiStatusOpen) {
		return true;
	}
	u32 status = 0;
	return R_SUCCEEDED(ACU_GetWifiStatus(&status)) && status != 0;
}

// The listener follows the network, which a sleep or leaving the access point's range drops.
static void follow_network(void)
{
	bool online = wifi_connected();
	if (g_reopenListener || !online) {
		g_reopenListener = false;
		close_control_listener();
	}
	if (g_controlListener < 0 && online) {
		open_control_listener();
	}
}

// Waking from sleep drops the Wi-Fi connection and every socket with it.
static void on_apt_event(APT_HookType type, void *unused)
{
	(void)unused;
	if (type == APTHOOK_ONWAKEUP) {
		g_reopenListener = true;
	}
}

// A listener that keeps failing is closed; the next network check opens a new one.
static void accept_waiting_client(int *failuresInRow)
{
	struct pollfd request = {.fd = g_controlListener, .events = POLLIN, .revents = 0};
	int ready = poll(&request, 1, 0);
	if (ready == 0) {
		ftp_socket_nap(LISTENER_POLL_NAP_MS);
		return;
	}
	int connection = -1;
	if (ready > 0 && (request.revents & POLLIN)) {
		connection = accept(g_controlListener, NULL, NULL);
	}
	if (connection < 0) {
		(*failuresInRow)++;
		if (*failuresInRow >= ACCEPT_FAILURE_LIMIT) {
			*failuresInRow = 0;
			close_control_listener();
		}
		ftp_socket_nap(ACCEPT_FAILURE_NAP_MS);
		return;
	}
	*failuresInRow = 0;
	ftp_session_start(connection);
}

static void serve_connections(void *unused)
{
	(void)unused;
	u64 nextNetworkCheck = 0;
	int acceptFailures = 0;
	while (ftp_state_running()) {
		u64 now = osGetTime();
		if (now >= nextNetworkCheck) {
			nextNetworkCheck = now + NETWORK_CHECK_INTERVAL_MS;
			follow_network();
		}
		if (g_controlListener < 0) {
			ftp_socket_nap(NO_LISTENER_NAP_MS);
			continue;
		}
		accept_waiting_client(&acceptFailures);
	}
	close_control_listener();
}

// System services

static bool socket_service_up(void)
{
	if (g_socketBuffer) {
		return true;
	}
	u32 *buffer = memalign(SOC_BUFFER_ALIGN, SOC_BUFFER_SIZE);
	if (!buffer) {
		return false;
	}
	if (R_FAILED(socInit(buffer, SOC_BUFFER_SIZE))) {
		free(buffer);
		return false;
	}
	g_socketBuffer = buffer;
	return true;
}

// Unless forced, the service stays up while a session thread is still closing: it may be in a socket call.
static void socket_service_down(bool force)
{
	if (!g_socketBuffer) {
		return;
	}
	if (!force && ftp_state_session_count() > 0) {
		return;
	}
	socExit();
	free(g_socketBuffer);
	g_socketBuffer = NULL;
}

static void open_wifi_status(void)
{
	g_wifiStatusOpen = R_SUCCEEDED(acInit());
}

static void close_wifi_status(void)
{
	if (g_wifiStatusOpen) {
		acExit();
	}
	g_wifiStatusOpen = false;
}

// Keeps the radio to this app while serving: StreetPass, SpotPass and the friends service stay off it.
static void enter_exclusive_network(void)
{
	if (R_FAILED(ndmuInit())) {
		return;
	}
	g_exclusiveNetwork = R_SUCCEEDED(NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE));
	if (g_exclusiveNetwork) {
		NDMU_LockState();
	} else {
		ndmuExit();
	}
}

static void leave_exclusive_network(void)
{
	if (!g_exclusiveNetwork) {
		return;
	}
	NDMU_UnlockState();
	NDMU_LeaveExclusiveState();
	ndmuExit();
	g_exclusiveNetwork = false;
}

// Starting and stopping

static bool config_is_usable(const FtpConfig *config)
{
	if (!config || !config->greeting || !config->rootPath) {
		return false;
	}
	bool rootFits = strlen(config->rootPath) < FTP_ROOT_PATH_MAX;
	int lastDataPort = config->port + FTP_DATA_PORT_COUNT;
	bool portsFit = config->port > 0 && lastDataPort <= PORT_MAX;
	return rootFits && portsFit;
}

static void wait_for_sessions_to_end(void)
{
	int waitedMs = 0;
	while (waitedMs < STOP_SESSION_WAIT_MS && ftp_state_session_count() > 0) {
		ftp_socket_nap(STOP_SESSION_POLL_MS);
		waitedMs += STOP_SESSION_POLL_MS;
	}
}

static void abandon_start(void)
{
	close_control_listener();
	socket_service_down(false);
	close_wifi_status();
}

bool ftp_start(const FtpConfig *config)
{
	if (g_serverThread) {
		return true;
	}
	if (!config_is_usable(config)) {
		return false;
	}
	ftp_state_setup(config);
	open_wifi_status();
	if (!wifi_connected() || !socket_service_up() || !open_control_listener()) {
		abandon_start();
		return false;
	}
	g_reopenListener = false;
	ftp_state_set_running(true);
	int priority = ftp_state_priority(FTP_PRIORITY_ABOVE_MAIN);
	g_serverThread =
	        threadCreate(serve_connections, NULL, SERVER_STACK_SIZE, priority, FTP_THREAD_CORE, false);
	if (!g_serverThread) {
		ftp_state_set_running(false);
		abandon_start();
		return false;
	}
	aptHook(&g_aptCookie, on_apt_event, NULL);
	enter_exclusive_network();
	return true;
}

void ftp_stop(void)
{
	if (g_serverThread) {
		ftp_state_set_running(false);
		threadJoin(g_serverThread, STOP_JOIN_TIMEOUT_MS * NANOSECONDS_PER_MILLISECOND);
		threadFree(g_serverThread);
		g_serverThread = NULL;
		aptUnhook(&g_aptCookie);
		close_wifi_status();
		wait_for_sessions_to_end();
		leave_exclusive_network();
	}
	socket_service_down(false); // also finishes a teardown that an earlier stop left for a closing session
}

void ftp_exit(void)
{
	ftp_stop();
	socket_service_down(true);
}

bool ftp_running(void)
{
	return g_serverThread != NULL;
}

const char *ftp_address(void)
{
	return g_listening ? g_address : NULL;
}
