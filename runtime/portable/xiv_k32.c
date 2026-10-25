/* KERNEL32 for ffxivgame.exe, beyond k32.c and k32_io.c. The wide-character (W) file functions
 * convert their strings and run the existing A shims (k32_io.c), so paths behave the same way;
 * the rest are small. Written against plat.h and the guest helpers only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "plat.h"
#include "thunk.h"
#include "vfs.h"

#define ERROR_CALL_NOT_IMPLEMENTED 120u
#define ERROR_INVALID_PARAMETER 87u

/* --- calling other shims ------------------------------------------------------------------------ */

/* Runs dll!name with `args` as if the guest had called it (stdcall: the shim pops them). */
static uint32_t call_shim(Guest* g, const char* dll, const char* name, unsigned nargs, const uint32_t* args)
{
    Shim s = thunk_find_shim(dll, name);
    if (!s)
    {
        rt_log("[recomp] xiv_k32: %s!%s is not available\n", dll, name);
        return 0;
    }
    uint32_t esp = g->esp;
    g->esp -= 4u * (nargs + 1u);
    wr32(g->esp, 0); /* return address: never used, the shim only pops it */
    for (unsigned i = 0; i < nargs; ++i)
        wr32(g->esp + 4u + 4u * i, args[i]);
    s(g);
    g->esp = esp;
    return g->eax;
}

/* --- UTF-16 <-> UTF-8 ----------------------------------------------------------------------------- */

static size_t wlen(uint32_t w)
{
    size_t n = 0;
    while (rd16(w + 2u * n))
        n++;
    return n;
}

/* A guest UTF-16 string as a new guest UTF-8 string (gheap; free with gheap_free). 0 for NULL. */
static uint32_t w_to_guest_utf8(uint32_t w)
{
    if (!w)
        return 0;
    size_t n = wlen(w);
    char* s = (char*)malloc(n * 3 + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; ++i)
    {
        uint32_t c = rd16(w + 2u * i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n)
        {
            uint32_t lo = rd16(w + 2u * (i + 1));
            if (lo >= 0xDC00 && lo < 0xE000)
            {
                c = 0x10000u + ((c - 0xD800u) << 10) + (lo - 0xDC00u);
                i++;
            }
        }
        if (c < 0x80)
            s[o++] = (char)c;
        else if (c < 0x800)
        {
            s[o++] = (char)(0xC0 | (c >> 6));
            s[o++] = (char)(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            s[o++] = (char)(0xE0 | (c >> 12));
            s[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            s[o++] = (char)(0x80 | (c & 0x3F));
        }
        else
        {
            s[o++] = (char)(0xF0 | (c >> 18));
            s[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            s[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            s[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    s[o] = 0;
    uint32_t a = gheap_strdup(s);
    free(s);
    return a;
}

/* Writes UTF-8 `s` as UTF-16 into the guest buffer w (cap characters, NUL included). Returns the
 * characters written without the NUL; if it does not fit, the size needed with the NUL. */
static uint32_t utf8_to_guest_w(const char* s, uint32_t w, uint32_t cap)
{
    uint16_t tmp[1024];
    uint32_t n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p && n < 1022;)
    {
        uint32_t c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2])
        {
            c = ((c & 7u) << 18) | ((p[0] & 0x3Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
            p += 3;
        }
        else if (c >= 0xE0 && p[0] && p[1])
        {
            c = ((c & 15u) << 12) | ((p[0] & 0x3Fu) << 6) | (p[1] & 0x3Fu);
            p += 2;
        }
        else if (c >= 0xC0 && p[0])
            c = ((c & 31u) << 6) | (*p++ & 0x3Fu);
        if (c >= 0x10000)
        {
            c -= 0x10000;
            tmp[n++] = (uint16_t)(0xD800 + (c >> 10));
            tmp[n++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        }
        else
            tmp[n++] = (uint16_t)c;
    }
    if (!w || n + 1 > cap)
        return n + 1;
    for (uint32_t i = 0; i < n; ++i)
        wr16(w + 2u * i, tmp[i]);
    wr16(w + 2u * n, 0);
    return n;
}

/* W shim = A shim with the string arguments in `mask` (bit i: argument i) converted. */
static void wide_call(Guest* g, const char* a_name, unsigned nargs, unsigned mask)
{
    uint32_t args[12], tmp[12] = { 0 };
    for (unsigned i = 0; i < nargs; ++i)
    {
        args[i] = ARG(i);
        if (mask & (1u << i))
            args[i] = tmp[i] = w_to_guest_utf8(args[i]);
    }
    uint32_t r = call_shim(g, "kernel32.dll", a_name, nargs, args);
    for (unsigned i = 0; i < nargs; ++i)
        if (tmp[i])
            gheap_free(tmp[i]);
    RET(r, nargs);
}

#define WIDE(wname, aname, nargs, mask) \
    static void sh_##wname(Guest* g) { wide_call(g, aname, nargs, mask); }

WIDE(SetCurrentDirectoryW, "SetCurrentDirectoryA", 1, 1u)
WIDE(RemoveDirectoryW, "RemoveDirectoryA", 1, 1u)
WIDE(CreateDirectoryW, "CreateDirectoryA", 2, 1u)
static void sh_GetFileAttributesW(Guest* g)
{
    uint32_t path = w_to_guest_utf8(ARG(0));
    uint32_t a[1] = { path };
    uint32_t r = call_shim(g, "kernel32.dll", "GetFileAttributesA", 1, a);
    if (r == 0xFFFFFFFFu && path)
        rt_log("[recomp] GetFileAttributesW(%s): not found\n", (const char*)GUEST_PTR(path));
    if (path)
        gheap_free(path);
    RET(r, 1);
}
WIDE(SetFileAttributesW, "SetFileAttributesA", 2, 1u)
WIDE(DeleteFileW, "DeleteFileA", 1, 1u)
WIDE(GetDriveTypeW, "GetDriveTypeA", 1, 1u)
WIDE(LoadLibraryW, "LoadLibraryA", 1, 1u)
WIDE(CreateMutexW, "CreateMutexA", 3, 4u)

/* GetCurrentDirectoryW(n, buf) */
static void sh_GetCurrentDirectoryW(Guest* g)
{
    uint32_t n = ARG(0), buf = ARG(1), t = gheap_alloc(1024, 1);
    uint32_t a[2] = { 1024, t };
    call_shim(g, "kernel32.dll", "GetCurrentDirectoryA", 2, a);
    uint32_t r = utf8_to_guest_w((const char*)GUEST_PTR(t), buf, n);
    gheap_free(t);
    RET(r, 2);
}

/* GetModuleFileNameW(h, buf, n) */
static void sh_GetModuleFileNameW(Guest* g)
{
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), t = gheap_alloc(1024, 1);
    uint32_t a[3] = { h, t, 1024 };
    uint32_t len = call_shim(g, "kernel32.dll", "GetModuleFileNameA", 3, a);
    uint32_t r = len ? utf8_to_guest_w((const char*)GUEST_PTR(t), buf, n) : 0;
    if (len && r > n) /* truncated, as Windows does */
    {
        r = n ? n - 1 : 0;
        if (n)
            wr16(buf + 2u * r, 0);
    }
    gheap_free(t);
    RET(r, 3);
}

/* GetFullPathNameW(name, n, buf, filepart) */
static void sh_GetFullPathNameW(Guest* g)
{
    uint32_t name = w_to_guest_utf8(ARG(0)), n = ARG(1), buf = ARG(2), part = ARG(3), t = gheap_alloc(1024, 1);
    uint32_t a[4] = { name, 1024, t, 0 };
    uint32_t len = call_shim(g, "kernel32.dll", "GetFullPathNameA", 4, a);
    uint32_t r = len ? utf8_to_guest_w((const char*)GUEST_PTR(t), buf, n) : 0;
    if (part && len && r < n)
    {
        uint32_t last = 0;
        for (uint32_t i = 0; rd16(buf + 2u * i); ++i)
            if (rd16(buf + 2u * i) == '\\')
                last = i + 1;
        wr32(part, buf + 2u * last);
    }
    gheap_free(t);
    gheap_free(name);
    RET(r, 4);
}

/* WIN32_FIND_DATAA (320 bytes) -> WIN32_FIND_DATAW (592): the 44-byte head is the same. */
static void find_data_widen(uint32_t a, uint32_t w)
{
    memcpy(GUEST_PTR(w), GUEST_PTR(a), 44);
    utf8_to_guest_w((const char*)GUEST_PTR(a + 44), w + 44, 260);
    utf8_to_guest_w((const char*)GUEST_PTR(a + 44 + 260), w + 44 + 520, 14);
}

static void sh_FindFirstFileW(Guest* g)
{
    uint32_t name = w_to_guest_utf8(ARG(0)), out = ARG(1), t = gheap_alloc(320, 1);
    uint32_t a[2] = { name, t };
    uint32_t h = call_shim(g, "kernel32.dll", "FindFirstFileA", 2, a);
    if (h != 0xFFFFFFFFu)
        find_data_widen(t, out);
    gheap_free(t);
    gheap_free(name);
    RET(h, 2);
}

static void sh_FindNextFileW(Guest* g)
{
    uint32_t h = ARG(0), out = ARG(1), t = gheap_alloc(320, 1);
    uint32_t a[2] = { h, t };
    uint32_t ok = call_shim(g, "kernel32.dll", "FindNextFileA", 2, a);
    if (ok)
        find_data_widen(t, out);
    gheap_free(t);
    RET(ok, 2);
}

/* MoveFileExW(from, to, flags): the host rename (replaces when MOVEFILE_REPLACE_EXISTING). */
static void sh_MoveFileExW(Guest* g)
{
    uint32_t from = w_to_guest_utf8(ARG(0)), to = w_to_guest_utf8(ARG(1));
    char hf[1024], ht[1024];
    int ok = from && to && vfs_host_path((const char*)GUEST_PTR(from), hf, sizeof hf) &&
             vfs_host_path((const char*)GUEST_PTR(to), ht, sizeof ht) && rename(hf, ht) == 0;
    if (from)
        gheap_free(from);
    if (to)
        gheap_free(to);
    if (!ok)
        gt_set_error(2);
    RET(ok, 3);
}

/* GetDiskFreeSpaceExW(root, avail*, total*, free*): plenty, whatever the root. */
static void sh_GetDiskFreeSpaceExW(Guest* g)
{
    const uint64_t free_bytes = 100ull << 30, total = 500ull << 30;
    if (ARG(1))
        wr64(ARG(1), free_bytes);
    if (ARG(2))
        wr64(ARG(2), total);
    if (ARG(3))
        wr64(ARG(3), free_bytes);
    RET(1, 4);
}

/* GetDiskFreeSpaceW(root, sectors_per_cluster*, bytes_per_sector*, free_clusters*, total_clusters*) */
static void sh_GetDiskFreeSpaceW(Guest* g)
{
    if (ARG(1))
        wr32(ARG(1), 8);
    if (ARG(2))
        wr32(ARG(2), 512);
    if (ARG(3))
        wr32(ARG(3), 0x01900000u); /* ~100 GB in 4 KB clusters */
    if (ARG(4))
        wr32(ARG(4), 0x07D00000u);
    RET(1, 5);
}

/* GetVolumeInformationW(root, name, name_n, serial*, max_component*, flags*, fs, fs_n) */
static void sh_GetVolumeInformationW(Guest* g)
{
    if (ARG(1))
        utf8_to_guest_w("XIVLEGACY", ARG(1), ARG(2));
    if (ARG(3))
        wr32(ARG(3), 0x58495631u);
    if (ARG(4))
        wr32(ARG(4), 255);
    if (ARG(5))
        wr32(ARG(5), 0x00000003u); /* case preserved, case sensitive search */
    if (ARG(6))
        utf8_to_guest_w("NTFS", ARG(6), ARG(7));
    RET(1, 8);
}

/* SetFilePointerEx(h, distance (2 dwords), new_pos*, method) */
static void sh_SetFilePointerEx(Guest* g)
{
    uint32_t hi_var = gheap_alloc(4, 0);
    wr32(hi_var, ARG(2));
    uint32_t a[4] = { ARG(0), ARG(1), hi_var, ARG(4) };
    uint32_t lo = call_shim(g, "kernel32.dll", "SetFilePointer", 4, a);
    uint32_t hi = rd32(hi_var);
    gheap_free(hi_var);
    int ok = !(lo == 0xFFFFFFFFu && gt_get_error());
    if (ok && ARG(3))
        wr64(ARG(3), ((uint64_t)hi << 32) | lo);
    RET(ok, 5);
}

/* GetFileSizeEx(h, LARGE_INTEGER*) */
static void sh_GetFileSizeEx(Guest* g)
{
    uint32_t hi_var = gheap_alloc(4, 1);
    uint32_t a[2] = { ARG(0), hi_var };
    gt_set_error(0);
    uint32_t lo = call_shim(g, "kernel32.dll", "GetFileSize", 2, a);
    uint32_t hi = rd32(hi_var);
    gheap_free(hi_var);
    int ok = !(lo == 0xFFFFFFFFu && gt_get_error());
    if (ok)
        wr64(ARG(1), ((uint64_t)hi << 32) | lo);
    RET(ok, 2);
}

static void sh_SetFileTime(Guest* g) { RET(1, 4); }
static void sh_CreatePipe(Guest* g) { gt_set_error(ERROR_CALL_NOT_IMPLEMENTED); RET(0, 4); }

/* --- strings ------------------------------------------------------------------------------------- */

static void sh_lstrcpyW(Guest* g)
{
    uint32_t d = ARG(0), s = ARG(1), i = 0;
    do
        wr16(d + 2u * i, rd16(s + 2u * i));
    while (rd16(s + 2u * i++));
    RET(d, 2);
}

static void sh_lstrcatW(Guest* g)
{
    uint32_t d = ARG(0), s = ARG(1), e = d + 2u * (uint32_t)wlen(d), i = 0;
    do
        wr16(e + 2u * i, rd16(s + 2u * i));
    while (rd16(s + 2u * i++));
    RET(d, 2);
}

static void sh_lstrcmpW(Guest* g)
{
    uint32_t a = ARG(0), b = ARG(1);
    for (uint32_t i = 0;; ++i)
    {
        uint16_t x = rd16(a + 2u * i), y = rd16(b + 2u * i);
        if (x != y || !x)
            RET(x < y ? (uint32_t)-1 : x > y ? 1u : 0u, 2);
    }
}

/* FormatMessageA(flags, source, id, lang, buf, size, args): a plain "error N" for system codes. */
static void sh_FormatMessageA(Guest* g)
{
    uint32_t flags = ARG(0), id = ARG(2), buf = ARG(4), size = ARG(5);
    char text[64];
    int n = snprintf(text, sizeof text, "Error %u.\r\n", id);
    if (flags & 0x100) /* FORMAT_MESSAGE_ALLOCATE_BUFFER: buf points at the pointer to set */
    {
        uint32_t p = gheap_strdup(text);
        wr32(buf, p);
        RET(n, 7);
    }
    if (!buf || (uint32_t)n + 1 > size)
    {
        gt_set_error(122);
        RET(0, 7);
    }
    memcpy(GUEST_PTR(buf), text, (size_t)n + 1);
    RET(n, 7);
}

/* --- process and threads ------------------------------------------------------------------------- */

static void sh_GetUserDefaultLangID(Guest* g) { RET(0x0409, 0); }
static void sh_GetSystemDefaultLCID(Guest* g) { RET(0x0409, 0); }
static void sh_SetProcessAffinityMask(Guest* g) { RET(1, 2); }
static void sh_SetThreadAffinityMask(Guest* g) { RET(0xF, 2); }
static void sh_SetThreadIdealProcessor(Guest* g) { RET(0, 2); }
static void sh_IsDebuggerPresent(Guest* g) { RET(0, 0); }
static void sh_GetExitCodeProcess(Guest* g) { gt_set_error(6); RET(0, 2); }

/* GetProcessAffinityMask(h, process_mask*, system_mask*): four processors. The game sizes its
 * worker threads by this; one guest thread runs at a time anyway (the guest lock). */
static void sh_GetProcessAffinityMask(Guest* g)
{
    if (ARG(1))
        wr32(ARG(1), 0xF);
    if (ARG(2))
        wr32(ARG(2), 0xF);
    RET(1, 3);
}

static void sh_SleepEx(Guest* g)
{
    uint32_t ms = ARG(0);
    gt_unlock();
    if (ms)
        plat_sleep_ms(ms);
    else
        plat_yield();
    gt_lock();
    RET(0, 2);
}

/* SwitchToThread: the guest lock goes to whoever waits. A thread that calls it in a tight loop (a
 * job worker waiting for work) naps instead after a few calls, or its lock traffic slows the thread
 * doing the work. */
static void sh_SwitchToThread(Guest* g)
{
    /* a spin: the previous call returned moments ago (the time spent waiting for the lock is not
     * counted, it is what makes a spinning thread's calls look far apart) */
    static _Thread_local uint64_t last_return;
    static _Thread_local unsigned streak;
    uint64_t now = rt_monotonic_ns();
    streak = now - last_return < 100000u ? streak + 1 : 0;
    gt_unlock();
    if (streak > 4)
        plat_sleep_ms(1);
    else
        plat_yield();
    gt_lock();
    last_return = rt_monotonic_ns();
    RET(1, 0);
}

static void sh_GlobalFree(Guest* g)
{
    if (ARG(0))
        gheap_free(ARG(0));
    RET(0, 1);
}

static void sh_GetSystemDirectoryA(Guest* g)
{
    static const char dir[] = "C:\\Windows\\system32";
    uint32_t buf = ARG(0), n = ARG(1);
    if (!buf || n < sizeof dir)
        RET(sizeof dir, 2);
    memcpy(GUEST_PTR(buf), dir, sizeof dir);
    RET(sizeof dir - 1, 2);
}

/* VirtualQuery(addr, MEMORY_BASIC_INFORMATION*, size): the guest window's commit state, page
 * by page, as one region of like pages. */
static void sh_VirtualQuery(Guest* g)
{
    uint32_t a = ARG(0) & ~0xFFFu, out = ARG(1);
    if (ARG(2) < 28)
        RET(0, 3);
    int committed = gwin_is_committed(a);
    uint32_t end = a + 0x1000;
    while (end && end - a < 0x1000000u && gwin_is_committed(end) == committed)
        end += 0x1000;
    int image = a >= rt_image_base && a < rt_image_base + rt_image_size;
    int text = a >= rt_image_base + rt_image_text_rva && a < rt_image_base + rt_image_text_rva + rt_image_text_size;
    wr32(out + 0, a);
    wr32(out + 4, image ? rt_image_base : a);
    wr32(out + 8, 0x40);                                    /* AllocationProtect */
    wr32(out + 12, end - a);                                /* RegionSize */
    wr32(out + 16, committed ? 0x1000u : 0x10000u);         /* MEM_COMMIT / MEM_FREE */
    wr32(out + 20, committed ? (text ? 0x20u : 0x04u) : 1u); /* EXECUTE_READ / READWRITE / NOACCESS */
    wr32(out + 24, image ? 0x1000000u : 0x20000u);          /* MEM_IMAGE / MEM_PRIVATE */
    RET(28, 3);
}

/* --- console: there is none ----------------------------------------------------------------------- */

static void sh_GetConsoleCP(Guest* g) { RET(437, 0); }
static void sh_GetConsoleOutputCP(Guest* g) { RET(437, 0); }
static void sh_SetConsoleMode(Guest* g) { gt_set_error(6); RET(0, 2); }
static void sh_GetConsoleMode(Guest* g) { gt_set_error(6); RET(0, 2); }
static void sh_FlushConsoleInputBuffer(Guest* g) { gt_set_error(6); RET(0, 1); }
static void sh_ReadConsoleInputA(Guest* g) { gt_set_error(6); RET(0, 4); }

static void sh_WriteConsoleA(Guest* g)
{
    rt_log("[console] %.*s", (int)ARG(2), ARGS(1));
    if (ARG(3))
        wr32(ARG(3), ARG(2));
    RET(1, 5);
}

static void sh_WriteConsoleW(Guest* g)
{
    if (ARG(3))
        wr32(ARG(3), ARG(2));
    RET(1, 5);
}

static void sh_SetEnvironmentVariableW(Guest* g) { RET(1, 2); }

/* GetTimeFormatA / GetDateFormatA(lcid, flags, SYSTEMTIME* or NULL, format, buf, n): the default
 * en-US forms (the game logs with them). */
static void systime_now(uint16_t* y, uint16_t* mo, uint16_t* d, uint16_t* h, uint16_t* mi, uint16_t* s)
{
    time_t t = time(NULL);
    struct tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    *y = (uint16_t)(tm.tm_year + 1900), *mo = (uint16_t)(tm.tm_mon + 1), *d = (uint16_t)tm.tm_mday;
    *h = (uint16_t)tm.tm_hour, *mi = (uint16_t)tm.tm_min, *s = (uint16_t)tm.tm_sec;
}

static void format_out(Guest* g, const char* text)
{
    uint32_t buf = ARG(4), n = ARG(5), len = (uint32_t)strlen(text) + 1;
    if (!n)
        RET(len, 6);
    if (!buf || len > n)
    {
        gt_set_error(122);
        RET(0, 6);
    }
    memcpy(GUEST_PTR(buf), text, len);
    RET(len, 6);
}

static void sh_GetTimeFormatA(Guest* g)
{
    uint16_t y, mo, d, h, mi, s;
    systime_now(&y, &mo, &d, &h, &mi, &s);
    if (ARG(2))
        h = rd16(ARG(2) + 8), mi = rd16(ARG(2) + 10), s = rd16(ARG(2) + 12);
    char text[32];
    snprintf(text, sizeof text, "%u:%02u:%02u %s", h % 12 ? h % 12 : 12, mi, s, h < 12 ? "AM" : "PM");
    format_out(g, text);
}

static void sh_GetDateFormatA(Guest* g)
{
    uint16_t y, mo, d, h, mi, s;
    systime_now(&y, &mo, &d, &h, &mi, &s);
    if (ARG(2))
        y = rd16(ARG(2)), mo = rd16(ARG(2) + 2), d = rd16(ARG(2) + 6);
    char text[32];
    snprintf(text, sizeof text, "%u/%u/%u", mo, d, y);
    format_out(g, text);
}

/* --- ADVAPI32: the registry (reg.c's private store) and odds ------------------------------------ */

/* RegOpenKeyExW(hkey, subkey, options, sam, phkey) */
static void sh_RegOpenKeyExW(Guest* g)
{
    uint32_t sub = w_to_guest_utf8(ARG(1));
    uint32_t a[5] = { ARG(0), sub, ARG(2), ARG(3), ARG(4) };
    uint32_t r = call_shim(g, "advapi32.dll", "RegOpenKeyExA", 5, a);
    rt_log("[recomp] RegOpenKeyExW(%08x, %s) = %u\n", ARG(0), sub ? (const char*)GUEST_PTR(sub) : "", r);
    if (sub)
        gheap_free(sub);
    RET(r, 5);
}

/* RegQueryValueExW(hkey, name, reserved, type*, data, cb*): strings come back as UTF-16. */
static void sh_RegQueryValueExW(Guest* g)
{
    uint32_t key = ARG(0), name = w_to_guest_utf8(ARG(1)), type_p = ARG(3), data = ARG(4), cb_p = ARG(5);
    uint32_t type_v = gheap_alloc(4, 1), cb_v = gheap_alloc(4, 1), tmp = gheap_alloc(4096, 1);
    wr32(cb_v, 4096);
    uint32_t a[6] = { key, name, 0, type_v, tmp, cb_v };
    uint32_t r = call_shim(g, "advapi32.dll", "RegQueryValueExA", 6, a);
    uint32_t type = rd32(type_v), got = rd32(cb_v);
    rt_log("[recomp] RegQueryValueExW(%s) = %u\n", name ? (const char*)GUEST_PTR(name) : "", r);
    if (r == 0)
    {
        int str = type == 1 || type == 2 || type == 7; /* REG_SZ, REG_EXPAND_SZ, REG_MULTI_SZ */
        uint32_t need = str ? 2u * got : got;
        if (type_p)
            wr32(type_p, type);
        if (data)
        {
            uint32_t cap = cb_p ? rd32(cb_p) : 0;
            if (need > cap)
                r = 234; /* ERROR_MORE_DATA */
            else if (str)
                for (uint32_t i = 0; i < got; ++i)
                    wr16(data + 2u * i, rd8(tmp + i));
            else
                memcpy(GUEST_PTR(data), GUEST_PTR(tmp), got);
        }
        if (cb_p)
            wr32(cb_p, need);
    }
    gheap_free(type_v);
    gheap_free(cb_v);
    gheap_free(tmp);
    if (name)
        gheap_free(name);
    RET(r, 6);
}

static void sh_RegisterEventSourceA(Guest* g) { RET(0x7E57, 2); }
static void sh_DeregisterEventSource(Guest* g) { RET(1, 1); }
static void sh_ReportEventA(Guest* g) { RET(1, 9); }

/* GetUserNameA(buf, size*) */
static void sh_GetUserNameA(Guest* g)
{
    static const char name[] = "Player";
    uint32_t n = ARG(1) ? rd32(ARG(1)) : 0;
    if (ARG(1))
        wr32(ARG(1), sizeof name);
    if (!ARG(0) || n < sizeof name)
    {
        gt_set_error(122);
        RET(0, 2);
    }
    memcpy(GUEST_PTR(ARG(0)), name, sizeof name);
    RET(1, 2);
}

/* EncodePointer / DecodePointer: the CRT looks them up and obfuscates its function pointers with
 * them; the identity is a valid encoding (Windows XP SP1 and earlier have none at all). */
static void sh_EncodePointer(Guest* g) { RET(ARG(0), 1); }
static void sh_DecodePointer(Guest* g) { RET(ARG(0), 1); }

/* GetNativeSystemInfo(SYSTEM_INFO*): what a 32-bit process sees on 32-bit Windows */
static void sh_GetNativeSystemInfo(Guest* g)
{
    uint32_t a[1] = { ARG(0) };
    call_shim(g, "kernel32.dll", "GetSystemInfo", 1, a);
    RET(0, 1);
}

static const ShimDef XIV_K32[] = {
    { "kernel32.dll", "EncodePointer", sh_EncodePointer },
    { "kernel32.dll", "DecodePointer", sh_DecodePointer },
    { "kernel32.dll", "GetNativeSystemInfo", sh_GetNativeSystemInfo },
    { "advapi32.dll", "RegOpenKeyExW", sh_RegOpenKeyExW },
    { "advapi32.dll", "RegQueryValueExW", sh_RegQueryValueExW },
    { "advapi32.dll", "RegisterEventSourceA", sh_RegisterEventSourceA },
    { "advapi32.dll", "DeregisterEventSource", sh_DeregisterEventSource },
    { "advapi32.dll", "ReportEventA", sh_ReportEventA },
    { "advapi32.dll", "GetUserNameA", sh_GetUserNameA },
    { "kernel32.dll", "SetCurrentDirectoryW", sh_SetCurrentDirectoryW },
    { "kernel32.dll", "GetCurrentDirectoryW", sh_GetCurrentDirectoryW },
    { "kernel32.dll", "RemoveDirectoryW", sh_RemoveDirectoryW },
    { "kernel32.dll", "CreateDirectoryW", sh_CreateDirectoryW },
    { "kernel32.dll", "GetFileAttributesW", sh_GetFileAttributesW },
    { "kernel32.dll", "SetFileAttributesW", sh_SetFileAttributesW },
    { "kernel32.dll", "DeleteFileW", sh_DeleteFileW },
    { "kernel32.dll", "MoveFileExW", sh_MoveFileExW },
    { "kernel32.dll", "GetDriveTypeW", sh_GetDriveTypeW },
    { "kernel32.dll", "LoadLibraryW", sh_LoadLibraryW },
    { "kernel32.dll", "CreateMutexW", sh_CreateMutexW },
    { "kernel32.dll", "GetModuleFileNameW", sh_GetModuleFileNameW },
    { "kernel32.dll", "GetFullPathNameW", sh_GetFullPathNameW },
    { "kernel32.dll", "FindFirstFileW", sh_FindFirstFileW },
    { "kernel32.dll", "FindNextFileW", sh_FindNextFileW },
    { "kernel32.dll", "GetDiskFreeSpaceW", sh_GetDiskFreeSpaceW },
    { "kernel32.dll", "GetDiskFreeSpaceExW", sh_GetDiskFreeSpaceExW },
    { "kernel32.dll", "GetVolumeInformationW", sh_GetVolumeInformationW },
    { "kernel32.dll", "SetFilePointerEx", sh_SetFilePointerEx },
    { "kernel32.dll", "GetFileSizeEx", sh_GetFileSizeEx },
    { "kernel32.dll", "SetFileTime", sh_SetFileTime },
    { "kernel32.dll", "CreatePipe", sh_CreatePipe },
    { "kernel32.dll", "lstrcpyW", sh_lstrcpyW },
    { "kernel32.dll", "lstrcatW", sh_lstrcatW },
    { "kernel32.dll", "lstrcmpW", sh_lstrcmpW },
    { "kernel32.dll", "FormatMessageA", sh_FormatMessageA },
    { "kernel32.dll", "GetUserDefaultLangID", sh_GetUserDefaultLangID },
    { "kernel32.dll", "GetSystemDefaultLCID", sh_GetSystemDefaultLCID },
    { "kernel32.dll", "SetProcessAffinityMask", sh_SetProcessAffinityMask },
    { "kernel32.dll", "SetThreadAffinityMask", sh_SetThreadAffinityMask },
    { "kernel32.dll", "SetThreadIdealProcessor", sh_SetThreadIdealProcessor },
    { "kernel32.dll", "GetProcessAffinityMask", sh_GetProcessAffinityMask },
    { "kernel32.dll", "IsDebuggerPresent", sh_IsDebuggerPresent },
    { "kernel32.dll", "GetExitCodeProcess", sh_GetExitCodeProcess },
    { "kernel32.dll", "SleepEx", sh_SleepEx },
    { "kernel32.dll", "SwitchToThread", sh_SwitchToThread },
    { "kernel32.dll", "GlobalFree", sh_GlobalFree },
    { "kernel32.dll", "GetSystemDirectoryA", sh_GetSystemDirectoryA },
    { "kernel32.dll", "VirtualQuery", sh_VirtualQuery },
    { "kernel32.dll", "GetConsoleCP", sh_GetConsoleCP },
    { "kernel32.dll", "GetConsoleOutputCP", sh_GetConsoleOutputCP },
    { "kernel32.dll", "SetConsoleMode", sh_SetConsoleMode },
    { "kernel32.dll", "GetConsoleMode", sh_GetConsoleMode },
    { "kernel32.dll", "FlushConsoleInputBuffer", sh_FlushConsoleInputBuffer },
    { "kernel32.dll", "ReadConsoleInputA", sh_ReadConsoleInputA },
    { "kernel32.dll", "WriteConsoleA", sh_WriteConsoleA },
    { "kernel32.dll", "WriteConsoleW", sh_WriteConsoleW },
    { "kernel32.dll", "SetEnvironmentVariableW", sh_SetEnvironmentVariableW },
    { "kernel32.dll", "GetTimeFormatA", sh_GetTimeFormatA },
    { "kernel32.dll", "GetDateFormatA", sh_GetDateFormatA },
    { NULL, NULL, NULL },
};

void xiv_k32_init(void)
{
    thunk_register(XIV_K32);
}
