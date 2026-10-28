/* The bug-report key: Ctrl+F12, or both sticks pressed in (L3+R3) and held for a second on a gamepad.
 * It takes a screenshot, sends it with the ends of the logs to the server's web front
 * (POST http://<server>:54993/bug/attach/<file>, signed in with the game session id), then opens the
 * chat line with "!bug " typed, so the player only writes what happened and presses Enter. The
 * server files the report with the character's place (the bugreport module of XIVLegacy Server).
 *
 * XIVLEGACY_LOGS: the folder of game.log and game.prev.log (the launcher sets it)
 * XIVLEGACY_LAUNCHER_LOG: the launcher's own log
 * XIVLEGACY_WEB: the web front as host[:port] (default: XIVLEGACY_LOBBY, port 54993) */
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bug_report.h"
#include "gfx9.h"
#include "plat.h"
#include "runtime.h"

#define WEB_PORT 54993
#define LOG_TAIL (1024 * 1024) /* the server keeps no more of each log */
#define HOLD_MS 1000

static char g_session[57];
static volatile int g_busy;
static uint64_t g_chord_since; /* when L3+R3 went down together; 0 while not */
static int g_chord_fired;
static int g_l3, g_r3;

void bug_report_init(const char* session_id)
{
    snprintf(g_session, sizeof g_session, "%s", session_id ? session_id : "");
}

static const char* logs_dir(void)
{
    const char* d = getenv("XIVLEGACY_LOGS");
    return d && *d ? d : ".";
}

/* the end of the file at `path` (at most LOG_TAIL bytes, from a line start), or NULL */
static char* tail_of(const char* path, size_t* len)
{
    size_t n = 0;
    unsigned char* all = plat_read_file(path, &n);
    if (!all)
        return NULL;
    size_t from = n > LOG_TAIL ? n - LOG_TAIL : 0;
    if (from)
        while (from < n && all[from - 1] != '\n')
            ++from;
    char* out = (char*)malloc(n - from + 1);
    memcpy(out, all + from, n - from);
    out[n - from] = 0;
    *len = n - from;
    free(all);
    return out;
}

/* `s` with every `what` replaced by `with` (a new string) */
static char* replace_all(char* s, size_t* len, const char* what, const char* with)
{
    size_t wl = strlen(what), nl = strlen(with);
    if (!wl)
        return s;
    size_t count = 0;
    for (char* p = s; (p = strstr(p, what)); p += wl)
        ++count;
    if (!count)
        return s;
    char* out = (char*)malloc(*len + count * (nl > wl ? nl - wl : 0) + 1);
    char *o = out, *p = s, *q;
    while ((q = strstr(p, what)))
    {
        memcpy(o, p, (size_t)(q - p)), o += q - p;
        memcpy(o, with, nl), o += nl;
        p = q + wl;
    }
    size_t rest = strlen(p);
    memcpy(o, p, rest + 1);
    *len = (size_t)(o - out) + rest;
    free(s);
    return out;
}

/* a log as it may leave this computer: no session id, and the home folder (the player's user name) as ~ */
static char* scrubbed(char* s, size_t* len)
{
    if (g_session[0])
        s = replace_all(s, len, g_session, "[session]");
    const char* home = getenv("HOME");
    if (!home || !*home)
        home = getenv("USERPROFILE");
    if (home && strlen(home) > 3)
        s = replace_all(s, len, home, "~");
    return s;
}

static int send_all(int fd, const void* buf, size_t n)
{
    const char* p = (const char*)buf;
    while (n)
    {
        long k = (long)send(fd, p, n > 65536 ? 65536 : (int)n, 0);
        if (k <= 0)
            return 0;
        p += k, n -= (size_t)k;
    }
    return 1;
}

/* POST the bytes as <name>; 1 when the server answered 200 */
static int post(const char* name, const void* data, size_t len)
{
    char host[256];
    const char* web = getenv("XIVLEGACY_WEB");
    const char* lobby = getenv("XIVLEGACY_LOBBY");
    snprintf(host, sizeof host, "%s", web && *web ? web : lobby && *lobby ? lobby : "127.0.0.1");
    char port[16];
    snprintf(port, sizeof port, "%d", WEB_PORT);
    char* colon = strrchr(host, ':');
    if (colon)
        *colon = 0, snprintf(port, sizeof port, "%s", colon + 1);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC, hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
    {
        rt_log("[bug] can't find the server %s\n", host);
        return 0;
    }
    int fd = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    int ok = fd >= 0 && connect(fd, res->ai_addr, (int)res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (ok)
    {
        char head[512];
        int hl = snprintf(head, sizeof head,
            "POST /bug/attach/%s HTTP/1.1\r\nHost: %s:%s\r\nAuthorization: Bearer %s\r\n"
            "Content-Type: application/octet-stream\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
            name, host, port, g_session, len);
        ok = send_all(fd, head, (size_t)hl) && send_all(fd, data, len);
    }
    char reply[64] = { 0 };
    if (ok)
    {
        long k = (long)recv(fd, reply, sizeof reply - 1, 0);
        ok = k > 12 && !strncmp(reply + 9, "200", 3);
    }
    if (fd >= 0)
#if defined(_WIN32)
        closesocket(fd);
#else
        close(fd);
#endif
    rt_log("[bug] %s: %s\n", name, ok ? "sent" : reply[0] ? reply : "not sent");
    return ok;
}

static void post_file(const char* name, const char* path, int is_log)
{
    size_t n = 0;
    char* data = is_log ? tail_of(path, &n) : (char*)plat_read_file(path, &n);
    if (!data)
        return;
    if (is_log)
        data = scrubbed(data, &n);
    post(name, data, n);
    free(data);
}

/* the chat line opened with "!bug " in it */
static void open_chat(void)
{
    int n = 0;
    SDL_Window** ws = SDL_GetWindows(&n);
    SDL_WindowID id = ws && n ? SDL_GetWindowID(ws[0]) : 0;
    SDL_free(ws);
    for (int down = 1; down >= 0; --down) /* Space opens the chat line */
    {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.windowID = id, e.key.scancode = SDL_SCANCODE_SPACE, e.key.key = SDLK_SPACE, e.key.down = down;
        SDL_PushEvent(&e);
        plat_sleep_ms(150);
    }
    for (const char* p = "!bug "; *p; ++p)
    {
        SDL_Event t;
        memset(&t, 0, sizeof t);
        t.type = SDL_EVENT_TEXT_INPUT;
        t.text.windowID = id;
        char c[2] = { *p, 0 };
        t.text.text = SDL_strdup(c);
        SDL_PushEvent(&t);
        plat_sleep_ms(30);
    }
}

static void capture(void* arg)
{
    (void)arg;
    char shot[1100], path[1100];
    snprintf(shot, sizeof shot, "%s%cbug-report.png", logs_dir(), plat_path_sep);
    plat_unlink(shot);
    g9_snap_next(shot);
    /* written at the next present, on the drawing thread: wait until its size stops changing */
    PlatStat st;
    int64_t last = -1;
    for (int i = 0; i < 50; ++i)
    {
        plat_sleep_ms(100);
        if (plat_stat(shot, &st) && st.size > 0)
        {
            if ((int64_t)st.size == last)
                break;
            last = (int64_t)st.size;
        }
    }
    rt_log("[bug] bug report: screenshot %s\n", last > 0 ? "taken" : "not taken");
    fflush(stdout);
    fflush(stderr);
    if (last > 0)
        post_file("screenshot.png", shot, 0);
    snprintf(path, sizeof path, "%s%cgame.log", logs_dir(), plat_path_sep);
    post_file("game.log", path, 1);
    snprintf(path, sizeof path, "%s%cgame.prev.log", logs_dir(), plat_path_sep);
    post_file("game.prev.log", path, 1);
    const char* ll = getenv("XIVLEGACY_LAUNCHER_LOG");
    if (ll && *ll)
        post_file("launcher.log", ll, 1);
    char info[512];
    int w = 0, h = 0;
    int n = 0;
    SDL_Window** ws = SDL_GetWindows(&n);
    if (ws && n)
        SDL_GetWindowSizeInPixels(ws[0], &w, &h);
    SDL_free(ws);
    int il = snprintf(info, sizeof info, "platform: %s\nwindow: %dx%d\ngamepads: %d\n", SDL_GetPlatform(), w, h,
        SDL_HasGamepad() ? 1 : 0);
    post("client.txt", info, (size_t)il);
    open_chat();
    g_busy = 0;
}

static void start(const char* why)
{
    if (g_busy)
        return;
    if (!g_session[0])
    {
        rt_log("[bug] no session: not signed in through the launcher\n");
        return;
    }
    g_busy = 1;
    rt_log("[bug] bug report (%s)\n", why);
    if (!plat_thread_start(capture, NULL))
        g_busy = 0;
}

int bug_report_event(const SDL_Event* e)
{
    switch (e->type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e->key.scancode != SDL_SCANCODE_F12 || !(e->key.mod & SDL_KMOD_CTRL))
            return 0;
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat)
            start("Ctrl+F12");
        return 1; /* the game never sees it */
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    {
        int down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK)
            g_l3 = down;
        else if (e->gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK)
            g_r3 = down;
        else
            return 0;
        if (g_l3 && g_r3 && !g_chord_since)
            g_chord_since = plat_wall_ms(), g_chord_fired = 0;
        else if (!(g_l3 && g_r3))
            g_chord_since = 0;
        return 0;
    }
    default: return 0;
    }
}

void bug_report_tick(void)
{
    if (g_chord_since && !g_chord_fired && plat_wall_ms() - g_chord_since >= HOLD_MS)
    {
        g_chord_fired = 1;
        start("L3+R3");
    }
}
