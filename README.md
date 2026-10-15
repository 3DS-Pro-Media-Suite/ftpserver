# ftpserver

A small FTP server for 3DS apps written in C.
It serves one folder on the SD card and one level of subfolders, so people can copy an app's files to a computer.

## Usage

```c
#include "ftp.h"

static const FtpConfig config = {
    .greeting = "My App",          // sent to clients when they connect
    .rootPath = "sdmc:/MyApp",     // the served folder, without a trailing slash
    .port = 5000,                  // the next 64 ports are the passive data ports
    .removeFolderContents = false, // true: RMD also deletes the files in a subfolder
    .canDelete = NULL,             // optional: return false to refuse a delete
    .isBusy = NULL,                // optional: return true while a file is still being written
};

if (ftp_start(&config)) {
    const char *address = ftp_address(); // "192.168.x.y:5000", or NULL while waiting for Wi-Fi
}

ftp_stop(); // when the user turns the server off
ftp_exit(); // at app exit
```

The server runs on its own threads. Nothing needs to be called each frame.
`canDelete` and `isBusy` are called from those threads.

After a sleep or a Wi-Fi drop, the server listens again once Wi-Fi is back.
While it runs, StreetPass and SpotPass stay off the radio.

## Add it to your project

Add the repository as a git submodule:

```sh
git submodule add https://github.com/3DS-Pro-Media-Suite/ftpserver ftpserver
```

Add the folder to your Makefile and link zlib:

```make
SOURCES  := source ftpserver
INCLUDES := source ftpserver
LIBS     := -lz -lctru -lm
```

You need devkitPro with `3ds-dev` and `3ds-zlib`. zlib is used for XCRC checksums.

## Commands

| What | Commands |
|---|---|
| Log in | USER, PASS (anything is accepted) |
| Browse | PWD, CWD, CDUP, LIST, NLST, SIZE, MDTM |
| Download | PASV, EPSV, RETR, REST (resume) |
| Check a download | XCRC |
| Delete | DELE, RMD |

Uploads, renames, new folders and active mode are refused.
Up to 4 clients can connect at once.
Paths cannot leave the served folder.

## Limits

* No password and no encryption. Use it only on a network you trust.
* File times are sent as the console's local time.
* The socket service takes 1 MB of memory while the server runs.

## License

MIT. See `LICENSE`.
