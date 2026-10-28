/* Starting the game, shared by every host (tests/boot.c on macOS and Linux, host_win.c on
 * Windows): the game module (split hosts), the guest window, the folders the game sees, the
 * Win32 stand-ins, the image, and the launch argument. argv: [--module <file>] <1.23b install
 * folder> [56-character session id]. 0 when ready for xiv_boot_run, else an exit code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#endif

#include "dinput.h"
#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "pe.h"
#include "plat.h"
#include "sqex_args.h"
#include "thunk.h"
#include "user32.h"
#include "vfs.h"
#include "ws2.h"
#include "xiv_boot.h"
#include "bug_report.h"
#include "xiv_icon.h"
#include "xiv_logtabs.h"

#define GAME_DIR "C:\\Program Files\\SquareEnix\\FINAL FANTASY XIV"
/* The user profile the game sees (Documents\\My Games\\..., AppData) lives in a folder of ours. */
#define USER_DIR "C:\\users\\Player"

int xiv_boot_prepare(int* argcp, char** argv)
{
#ifdef XIVLEGACY_SPLIT
    /* --module <file>: the game, made on this machine from its own ffxivgame.exe (xiv_game.h) */
    const char* module = NULL;
    for (int i = 1; i + 1 < (*argcp); ++i)
        if (!strcmp(argv[i], "--module"))
        {
            module = argv[i + 1];
            memmove(&argv[i], &argv[i + 2], (size_t)((*argcp) - i - 1) * sizeof *argv);
            (*argcp) -= 2;
            break;
        }
    if (!module)
    {
        printf("usage: xivlegacy-host --module <game module> <1.23b install folder> [session id]\n");
        return 2;
    }
    char module_err[512];
    if (!xiv_game_load(module, module_err, sizeof module_err))
    {
        printf("%s\n", module_err);
        return 1;
    }
#endif
    if ((*argcp) < 2)
    {
        printf("usage: boot <1.23b install folder>\n");
        return 2;
    }
    char exe[1024];
    snprintf(exe, sizeof exe, "%s%cffxivgame.exe", argv[1], plat_path_sep);
    xiv_icon_source(exe);

    if (!gwin_init())
    {
        printf("cannot reserve the guest window\n");
        return 1;
    }
    printf("guest window at %p\n", (void*)rt_guest_base);
#ifdef XIVLEGACY_SPLIT
    xiv_game_bind();
#endif
    gt_init();
    vfs_mount(GAME_DIR, argv[1]);
    /* XIVLEGACY_USER: the host folder behind C:\\users\\Player (default ./xiv-user) */
    const char* user = getenv("XIVLEGACY_USER") ? getenv("XIVLEGACY_USER") : "xiv-user";
    char d[1024];
    static const char* const subs[] = { "", "/Documents", "/AppData", "/AppData/Roaming", "/AppData/Local", "/Desktop",
                                        "/ProgramData" };
    for (unsigned i = 0; i < sizeof subs / sizeof subs[0]; ++i)
    {
        snprintf(d, sizeof d, "%s%s", user, subs[i]);
        plat_mkdir(d);
    }
    vfs_mount(USER_DIR, user);
    snprintf(d, sizeof d, "%s/Documents/My Games", user);
    plat_mkdir(d);
    snprintf(d, sizeof d, "%s/Documents/My Games/FINAL FANTASY XIV", user);
    plat_mkdir(d);
    snprintf(d, sizeof d, "%s/Documents/My Games/FINAL FANTASY XIV/config.pad", user);
    dinput_default_pad_config(d);
    xiv_logtabs_migrate(user);
    snprintf(d, sizeof d, "%s/ProgramData", user);
    vfs_mount("C:\\ProgramData", d);
    k32_init(GAME_DIR, GAME_DIR "\\ffxivgame.exe");
    k32_io_init();
    k32_misc_init();
    xiv_k32_init();
    xiv_misc_init();
    d3d9_init();
    d3dx9_init();
    xiv_fiber_init();
    xiv_net_init();
    xaudio2_init();
    user32_init();
    dinput_init();
    ws2_init();
    /* XIVLEGACY_LOBBY: the server, by name or IPv4 address (default 127.0.0.1, a local server) */
    {
        const char* lobby = getenv("XIVLEGACY_LOBBY") ? getenv("XIVLEGACY_LOBBY") : "127.0.0.1";
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        if (getaddrinfo(lobby, NULL, &hints, &res) == 0 && res)
        {
            uint32_t ip = ntohl(((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
            ws2_map_host("lobby01.ffxiv.com", ip);
            printf("lobby: %s (%u.%u.%u.%u)\n", lobby, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
            freeaddrinfo(res);
        }
        else
            printf("lobby: can't find the server %s\n", lobby);
    }
    ole_init();
    vfs_init(GAME_DIR);
    reg_init(NULL, 0, NULL);
    rt_set_native_handler(thunk_dispatch);
    if (!pe_load(exe))
        return 1;
    dinput_setup(); /* its COM vtables are thunks, made once the import table is bound */
    xiv_patches_apply();
    unsigned missing = thunk_report_missing();
    printf("imports with no shim yet: %u (the run stops at the first one called)\n", missing);

    /* What ffxivboot would pass: the encrypted session argument, keyed by the tick the game will
     * read back through GetTickCount (the same clock). A placeholder session for now. */
    char arg[512];
    const char* session = (*argcp) > 2 ? argv[2] : "00000000000000000000000000000000000000000000000000000000";
    if ((*argcp) > 2)
        bug_report_init(argv[2]);
    uint32_t tick = (uint32_t)(rt_monotonic_ns() / 1000000u);
    if (!sqex_launch_arg(session, tick, arg, sizeof arg))
    {
        printf("session id must be 56 characters\n");
        return 2;
    }
    k32_set_args(arg);
    printf("launch argument (tick %u): %s\n", tick, arg);
    return 0;
}

uint32_t xiv_boot_run(void)
{
    /* mainCRTStartup takes no arguments and ends the process through ExitProcess. */
    return guest_call(rt_image_oep, 0, NULL);
}
