/* The game host on Windows: Winsock, a crash report, then the shared start (xiv_boot.c).
 *
 *   xivlegacy-host.exe --module <xivlegacy-game.dll> <1.23b install folder> [session id]
 *
 * The macOS and Linux host is tests/boot.c, which adds POSIX debugging tools (the control FIFO,
 * the profilers) around the same start. */
#include <winsock2.h>
#include <windows.h>

#include <stdio.h>

#include "runtime.h"
#include "xiv_boot.h"

static LONG WINAPI on_crash(EXCEPTION_POINTERS* e)
{
    fprintf(stderr, "[host] crash: exception %#lx at %p\n", (unsigned long)e->ExceptionRecord->ExceptionCode,
        e->ExceptionRecord->ExceptionAddress);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SetUnhandledExceptionFilter(on_crash);
    setvbuf(stdout, NULL, _IONBF, 0);
    int rc = xiv_boot_prepare(&argc, argv);
    if (rc)
        return rc;
    uint32_t r = xiv_boot_run();
    printf("mainCRTStartup returned %u\n", r);
    return 0;
}
