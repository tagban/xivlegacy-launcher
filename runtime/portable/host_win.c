/* The game host on Windows: Winsock, a crash report, then the shared start (xiv_boot.c).
 *
 *   xivlegacy-host.exe --module <xivlegacy-game.dll> <1.23b install folder> [session id]
 *
 * The macOS and Linux host is tests/boot.c, which adds POSIX debugging tools (the control FIFO,
 * the profilers) around the same start. */
#include <winsock2.h>
#include <windows.h>
#include <timeapi.h>

#include <stdio.h>

#include "plat.h"
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
    /* Timeouts on Windows round up to the system timer's tick, 15.6 ms by default, and since
     * Windows 10 2004 each program sets its own: the game's 1 ms waits (WaitForSingleObject and
     * the rest, through WaitOnAddress) take 15.6 without this, enough to hold the frame rate down
     * and make the audio engine late. */
    timeBeginPeriod(1);
    plat_no_throttling();
    /* a check that lands in the log: what a 2 ms sleep costs here */
    {
        uint64_t t0 = rt_monotonic_ns();
        for (int i = 0; i < 5; ++i)
            plat_sleep_ms(2);
        printf("[host] 5 sleeps of 2 ms took %.1f ms\n", (double)(rt_monotonic_ns() - t0) * 1e-6);
    }
    SetUnhandledExceptionFilter(on_crash);
    setvbuf(stdout, NULL, _IONBF, 0);
    int rc = xiv_boot_prepare(&argc, argv);
    if (rc)
        return rc;
    uint32_t r = xiv_boot_run();
    printf("mainCRTStartup returned %u\n", r);
    return 0;
}
