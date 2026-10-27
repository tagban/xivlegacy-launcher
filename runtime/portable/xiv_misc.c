/* The smaller DLLs ffxivgame.exe imports: CRYPT32 and WINTRUST (the game checks its own
 * Authenticode signature), and the rest as they come up. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "pe.h"
#include "plat.h"
#include "runtime.h"
#include "thunk.h"

#define CRYPT_E_NO_MATCH 0x80092009u

/* --- CRYPT32: no signature found (the answer for an unsigned file) ------------------------------ */

/* CryptQueryObject(type, object, expected_content, expected_format, flags, encoding*, content*,
 * format*, store*, msg*, context*) */
static void sh_CryptQueryObject(Guest* g)
{
    for (int i = 5; i <= 10; ++i)
        if (ARG(i))
            wr32(ARG(i), 0);
    gt_set_error(CRYPT_E_NO_MATCH);
    RET(0, 11);
}

static void sh_CryptMsgGetParam(Guest* g) { gt_set_error(CRYPT_E_NO_MATCH); RET(0, 5); }
static void sh_CryptMsgClose(Guest* g) { RET(1, 1); }
static void sh_CryptDecodeObject(Guest* g) { gt_set_error(CRYPT_E_NO_MATCH); RET(0, 7); }
static void sh_CertFindCertificateInStore(Guest* g) { gt_set_error(CRYPT_E_NO_MATCH); RET(0, 6); }
static void sh_CertFreeCertificateContext(Guest* g) { RET(1, 1); }
static void sh_CertCloseStore(Guest* g) { RET(1, 2); }

/* --- WINTRUST ----------------------------------------------------------------------------------- */

/* WinVerifyTrust(hwnd, action guid*, data*): trusted. */
static void sh_WinVerifyTrust(Guest* g) { RET(0, 3); }

/* --- VERSION: the exe's own VS_VERSIONINFO ------------------------------------------------------ */

#define RT_VERSION 16u

/* Guest UTF-16 or ANSI path -> ASCII lowercase base name. */
static void base_lower(uint32_t p, int wide, char* out, size_t n)
{
    char full[600];
    size_t k = 0;
    for (uint32_t i = 0; k + 1 < sizeof full; ++i)
    {
        uint32_t c = wide ? rd16(p + 2u * i) : rd8(p + i);
        if (!c)
            break;
        full[k++] = (char)(c < 128 ? tolower((int)c) : '?');
    }
    full[k] = 0;
    const char* b = full;
    for (const char* q = full; *q; ++q)
        if (*q == '\\' || *q == '/')
            b = q + 1;
    snprintf(out, n, "%s", b);
}

/* The version resource for a file the game asks about: its own exe; nothing else for now. */
static uint32_t version_res(uint32_t path, int wide, uint32_t* size)
{
    char b[260];
    base_lower(path, wide, b, sizeof b);
    if (!strcmp(b, "ffxivgame.exe"))
        return pe_resource(rt_image_base, RT_VERSION, 1, size);
    rt_log("[recomp] version info asked for %s: not provided\n", b);
    return 0;
}

static void version_size(Guest* g, int wide)
{
    uint32_t size = 0;
    if (ARG(1))
        wr32(ARG(1), 0);
    if (!version_res(ARG(0), wide, &size))
    {
        gt_set_error(1812); /* ERROR_RESOURCE_DATA_NOT_FOUND */
        RET(0, 2);
    }
    RET(size * 2, 2); /* room for the ANSI copies VerQueryValueA hands out, as Windows does */
}

static void version_get(Guest* g, int wide)
{
    uint32_t size = 0, res = version_res(ARG(0), wide, &size), len = ARG(2), out = ARG(3);
    if (!res)
    {
        gt_set_error(1812);
        RET(0, 4);
    }
    memset(GUEST_PTR(out), 0, len);
    memcpy(GUEST_PTR(out), GUEST_PTR(res), size < len ? size : len);
    RET(1, 4);
}

static void sh_GetFileVersionInfoSizeW(Guest* g) { version_size(g, 1); }
static void sh_GetFileVersionInfoSizeA(Guest* g) { version_size(g, 0); }
static void sh_GetFileVersionInfoW(Guest* g) { version_get(g, 1); }
static void sh_GetFileVersionInfoA(Guest* g) { version_get(g, 0); }

static uint32_t align4(uint32_t a) { return (a + 3u) & ~3u; }

/* Node layout: wLength, wValueLength, wType, szKey (UTF-16), pad, value, pad, children. */
static uint32_t node_value(uint32_t n)
{
    uint32_t k = n + 6;
    while (rd16(k))
        k += 2;
    return align4(k + 2);
}

static uint32_t node_value_bytes(uint32_t n)
{
    return rd16(n + 4) == 1 ? rd16(n + 2) * 2u : rd16(n + 2);
}

static int key_is(uint32_t n, const char* name, size_t len)
{
    uint32_t k = n + 6;
    for (size_t i = 0; i < len; ++i, k += 2)
        if (tolower(rd16(k)) != tolower((unsigned char)name[i]))
            return 0;
    return rd16(k) == 0;
}

/* The node for a "\\StringFileInfo\\040904b0\\FileVersion" style path, or 0. */
static uint32_t version_find(uint32_t block, const char* path)
{
    uint32_t n = block;
    for (const char* p = path; *p;)
    {
        while (*p == '\\')
            p++;
        if (!*p)
            break;
        const char* e = p;
        while (*e && *e != '\\')
            e++;
        uint32_t end = n + rd16(n), c = align4(node_value(n) + node_value_bytes(n)), found = 0;
        while (c < end && rd16(c))
        {
            if (key_is(c, p, (size_t)(e - p)))
            {
                found = c;
                break;
            }
            c = align4(c + rd16(c));
        }
        if (!found)
            return 0;
        n = found;
        p = e;
    }
    return n;
}

static void query(Guest* g, int wide)
{
    uint32_t block = ARG(0), sub = ARG(1), out = ARG(2), outlen = ARG(3);
    char path[260];
    size_t k = 0;
    for (uint32_t i = 0; k + 1 < sizeof path; ++i)
    {
        uint32_t c = wide ? rd16(sub + 2u * i) : rd8(sub + i);
        if (!c)
            break;
        path[k++] = (char)c;
    }
    path[k] = 0;
    uint32_t n = version_find(block, path);
    if (!n || !rd16(n + 2))
        RET(0, 4);
    uint32_t v = node_value(n), chars = rd16(n + 2);
    if (!wide && rd16(n + 4) == 1)
    {
        /* an ANSI copy in the second half of the buffer (GetFileVersionInfoSize asked for twice
         * the resource) */
        uint32_t a = block + rd16(block) + (v - block) / 2;
        for (uint32_t i = 0; i < chars; ++i)
            wr8(a + i, (uint8_t)rd16(v + 2u * i));
        v = a;
    }
    wr32(out, v);
    if (outlen)
        wr32(outlen, rd16(n + 4) == 1 ? chars : node_value_bytes(n));
    RET(1, 4);
}

static void sh_VerQueryValueW(Guest* g) { query(g, 1); }
static void sh_VerQueryValueA(Guest* g) { query(g, 0); }

/* --- SHELL32: special folders inside our own data folder ----------------------------------------- */

/* The guest sees a Windows user profile; the host mounts it on its own folder (never the real
 * Documents, so a retail install's settings are untouched). */
#define USER_PROFILE "C:\\users\\Player"

static const char* csidl_path(uint32_t csidl)
{
    switch (csidl & 0xFF)
    {
    case 0x05: /* CSIDL_PERSONAL (My Documents) */
        return USER_PROFILE "\\Documents";
    case 0x1A: /* CSIDL_APPDATA */
        return USER_PROFILE "\\AppData\\Roaming";
    case 0x1C: /* CSIDL_LOCAL_APPDATA */
        return USER_PROFILE "\\AppData\\Local";
    case 0x23: /* CSIDL_COMMON_APPDATA */
        return "C:\\ProgramData";
    case 0x28: /* CSIDL_PROFILE */
        return USER_PROFILE;
    case 0x10: /* CSIDL_DESKTOPDIRECTORY */
        return USER_PROFILE "\\Desktop";
    default:
        return NULL;
    }
}

#define PIDL_MAGIC 0x44495058u /* "XPID" */

/* SHGetSpecialFolderLocation(hwnd, csidl, ppidl): a private PIDL that names the folder. */
static void sh_SHGetSpecialFolderLocation(Guest* g)
{
    if (!csidl_path(ARG(1)))
    {
        rt_log("[recomp] SHGetSpecialFolderLocation(csidl %#x): not provided\n", ARG(1));
        wr32(ARG(2), 0);
        RET(0x80004005u, 3); /* E_FAIL */
    }
    uint32_t p = gheap_alloc(12, 1);
    wr16(p, 10);
    wr32(p + 2, PIDL_MAGIC);
    wr32(p + 6, ARG(1));
    wr32(ARG(2), p); /* the empty item terminating the list is the zeroed tail */
    RET(0, 3);
}

static const char* pidl_path(uint32_t pidl)
{
    if (!pidl || rd16(pidl) != 10 || rd32(pidl + 2) != PIDL_MAGIC)
        return NULL;
    return csidl_path(rd32(pidl + 6));
}

static void sh_SHGetPathFromIDListW(Guest* g)
{
    const char* path = pidl_path(ARG(0));
    if (!path)
        RET(0, 2);
    uint32_t buf = ARG(1), i = 0;
    for (; path[i]; ++i)
        wr16(buf + 2u * i, (uint8_t)path[i]);
    wr16(buf + 2u * i, 0);
    RET(1, 2);
}

static void sh_SHGetPathFromIDListA(Guest* g)
{
    const char* path = pidl_path(ARG(0));
    if (!path)
        RET(0, 2);
    memcpy(GUEST_PTR(ARG(1)), path, strlen(path) + 1);
    RET(1, 2);
}

/* SHGetMalloc(IMalloc**): an IMalloc on the guest heap. */
static uint32_t g_malloc_obj;

static void sh_SHGetMalloc(Guest* g)
{
    if (!g_malloc_obj)
    {
        static const char* const names[9] = { "QueryInterface", "AddRef", "Release", "Alloc", "Realloc",
                                              "Free", "GetSize", "DidAlloc", "HeapMinimize" };
        uint32_t vt = gheap_alloc(4 * 9, 1);
        char full[64];
        for (int i = 0; i < 9; ++i)
        {
            snprintf(full, sizeof full, "IMalloc::%s", names[i]);
            wr32(vt + 4u * i, thunk_for("shell32.dll", full));
        }
        g_malloc_obj = gheap_alloc(4, 1);
        wr32(g_malloc_obj, vt);
    }
    wr32(ARG(0), g_malloc_obj);
    RET(0, 1);
}

static void sh_IMalloc_QueryInterface(Guest* g) { wr32(ARG(2), 0); RET(0x80004002u, 3); }
static void sh_IMalloc_AddRef(Guest* g) { RET(1, 1); }
static void sh_IMalloc_Release(Guest* g) { RET(1, 1); }
static void sh_IMalloc_Alloc(Guest* g) { RET(gheap_alloc(ARG(1) ? ARG(1) : 1, 0), 2); }
static void sh_IMalloc_Realloc(Guest* g) { RET(ARG(1) ? gheap_realloc(ARG(1), ARG(2), 0, 0) : gheap_alloc(ARG(2), 0), 3); }
static void sh_IMalloc_Free(Guest* g)
{
    /* our PIDLs and anything from Alloc live on the guest heap */
    if (ARG(1))
        gheap_free(ARG(1));
    RET(0, 2);
}
static void sh_IMalloc_GetSize(Guest* g) { RET(ARG(1) ? gheap_size(ARG(1)) : 0, 2); }
static void sh_IMalloc_DidAlloc(Guest* g) { RET(1, 2); }
static void sh_IMalloc_HeapMinimize(Guest* g) { RET(0, 1); }

/* ShellExecute(hwnd, verb, file, params, dir, show): nothing is opened (web pages, the updater). */
static void sh_ShellExecuteA(Guest* g)
{
    rt_log("[recomp] ShellExecuteA(%s %s): not opened\n", ARG(1) ? ARGS(1) : "", ARG(2) ? ARGS(2) : "");
    RET(2, 6); /* SE_ERR_FILENOTFOUND */
}

static void sh_ShellExecuteW(Guest* g)
{
    char s[260];
    size_t k = 0;
    for (uint32_t i = 0; ARG(2) && k + 1 < sizeof s && rd16(ARG(2) + 2u * i); ++i)
        s[k++] = (char)rd16(ARG(2) + 2u * i);
    s[k] = 0;
    rt_log("[recomp] ShellExecuteW(%s): not opened\n", s);
    RET(2, 6);
}

/* --- GDI32: what the game draws with GDI is debug text; nothing reaches the screen this way -------- */

static void sh_GetStockObject(Guest* g) { RET(0x01900000u + (ARG(0) & 0xFF), 1); }
static void sh_CreateFontW(Guest* g) { RET(0x018A0001u, 14); }
static void sh_CreatePen(Guest* g) { RET(0x01300001u, 3); }
static void sh_MoveToEx(Guest* g) { if (ARG(3)) { wr32(ARG(3), 0); wr32(ARG(3) + 4, 0); } RET(1, 4); }
static void sh_LineTo(Guest* g) { RET(1, 3); }
static void sh_Ellipse(Guest* g) { RET(1, 5); }
static void sh_TextOutA(Guest* g) { RET(1, 5); }

/* GetTextMetricsA(hdc, TEXTMETRICA*): an 8x16 cell. */
static void sh_GetTextMetricsA(Guest* g)
{
    uint32_t t = ARG(1);
    memset(GUEST_PTR(t), 0, 56);
    wr32(t + 0, 16);  /* tmHeight */
    wr32(t + 4, 13);  /* tmAscent */
    wr32(t + 8, 3);   /* tmDescent */
    wr32(t + 20, 8);  /* tmAveCharWidth */
    wr32(t + 24, 8);  /* tmMaxCharWidth */
    wr32(t + 28, 400); /* tmWeight */
    wr8(t + 45, 32);  /* tmFirstChar.. (approximately) */
    wr8(t + 46, 126);
    RET(1, 2);
}

/* GetTextExtentPoint32A(hdc, s, n, SIZE*) */
static void sh_GetTextExtentPoint32A(Guest* g)
{
    wr32(ARG(3), 8u * ARG(2));
    wr32(ARG(3) + 4, 16);
    RET(1, 4);
}

static void sh_GetGlyphOutlineW(Guest* g) { RET(0xFFFFFFFFu, 7); } /* GDI_ERROR */

/* --- IMM32: the wide forms; no input method composition yet ----------------------------------------- */

static void sh_ImmAssociateContext(Guest* g) { RET(0, 2); }
static void sh_ImmDisableTextFrameService(Guest* g) { RET(1, 1); }
static void sh_ImmGetCandidateListW(Guest* g) { RET(0, 4); }
static void sh_ImmGetCompositionStringW(Guest* g) { RET(0, 4); }

/* --- COMCTL32: window subclassing (ordinals 410/412/413) ------------------------------------------ */


/* --- OLEAUT32 / ole32: VARIANTs for the WMI queries, and COM classes we do not host ---------------- */

static void sh_VariantInit(Guest* g) { memset(GUEST_PTR(ARG(0)), 0, 16); RET(0, 1); }
static void sh_VariantClear(Guest* g)
{
    if (rd16(ARG(0)) == 8 && rd32(ARG(0) + 8)) /* VT_BSTR: the string is ours to free */
        gheap_free(rd32(ARG(0) + 8) - 4);
    memset(GUEST_PTR(ARG(0)), 0, 16);
    RET(0, 1);
}
static void sh_VariantChangeType(Guest* g)
{
    if (ARG(0) != ARG(1))
        memcpy(GUEST_PTR(ARG(0)), GUEST_PTR(ARG(1)), 16);
    RET(rd16(ARG(0)) == (ARG(3) & 0xFFFF) ? 0u : 0x80020005u, 4); /* DISP_E_TYPEMISMATCH */
}
static void sh_SafeArrayCreate(Guest* g)
{
    rt_log("[recomp] SafeArrayCreate(vt %u): not supported\n", ARG(0));
    RET(0, 3);
}
static void sh_SafeArrayPutElement(Guest* g) { RET(0x80070057u, 3); } /* E_INVALIDARG */

/* The MSSMIXER section's one function (0xf3d000, outside .text): the Miles Sound System's CPU check,
 * CPUID bits as MMX 1 | 3DNow! 2 | SSE2 4 | SSE 8. The translated code has MMX, SSE and SSE2. */
static void sh_mss_cpu_features(Guest* g) { RET(1u | 4u | 8u, 0); }

static const uint8_t MSS_CPU_CHECK[] = { 0x53, 0x56, 0x57, 0x1E, 0x06, 0xBE, 0, 0, 0, 0, 0x9C, 0x58, 0x8B, 0xD8 };

/* The game's log: messages it hands a callback at [0x12651B4] (cdecl: text, level). Retail's
 * (0x406330) writes all but level 1 to the C library's stdout, which a GUI program does not have.
 * Printed here, the game says what it is doing and what went wrong. */
static void sh_game_log(Guest* g)
{
    const char* text = ARG(0) ? ARGS(0) : "";
    size_t n = strlen(text);
    rt_log("[game %u] %.*s%s", ARG(1), (int)(n > 900 ? 900 : n), text, n && text[n - 1] == '\n' ? "" : "\n");
    RETC(0);
}

static const ShimDef GAME_LOG[] = {
    { "xivlegacy", "GameLog", sh_game_log },
    { NULL, NULL, NULL },
};

/* The game's other log: text it streams (std::operator<<(ostream&, const char*), 0x4E7290) into its
 * log stream at 0x1363D30, which goes nowhere in retail. The recompiler's hook point at that
 * function's entry (recomp --hooks streamlog=0x4e7290) hands it here. */
#ifndef XIVLEGACY_SPLIT
extern GuestFn rt_hook_streamlog __attribute__((weak));
#endif

static void hook_streamlog(Guest* g)
{
    static char line[1024];
    static size_t len;
    uint32_t stream = rd32(g->esp + 4), text = rd32(g->esp + 8);
    if (stream != 0x1363D30u || !text)
        return;
    for (const char* p = (const char*)GUEST_PTR(text); *p; ++p)
    {
        if (*p == '\n' || len + 1 >= sizeof line)
        {
            line[len] = 0;
            rt_log("[game] %s\n", line);
            len = 0;
            if (*p == '\n')
                continue;
        }
        line[len++] = *p;
    }
    if (len)
    {
        /* a fragment without its newline: shown at once, as a line of its own */
        line[len] = 0;
        rt_log("[game] %s\n", line);
        len = 0;
    }
}

/* The loading screen. The game's "Now Loading" widget (\system\common\NowLoading.form, built at
 * 0x56A4A0, vtable 0xFA588C) hangs off the app: [0x13232B8] + 0x34 is the object the main loop
 * ticks, its +0x64 the UI manager, whose +0x10 + 0x175A4 holds the widget. The widget's byte at
 * +0x229 is set while the screen is up (character list, character select to the world, zone
 * changes) and cleared as it fades out.
 *
 * Behind it, loading is all that matters, and the game is slow at it: the world loads through the
 * main loop's ticks (its frame limiter at 0xCC6D00 sleeps out a 30 fps frame) and worker threads
 * that poll and yield, while the drawing thread renders the hidden world at full speed and holds
 * the guest lock most of the time. So while the screen is up the drawing thread naps each frame, its
 * turn with the lock is short (gt_set_loading), workers yield without napping (SwitchToThread) and
 * the main loop does not sleep out its frame. Character select to the world: 42 s -> 12 s. */
#define LOADING_NAP_MS 30u
static uint32_t loading_widget(void)
{
    static uint32_t widget;
    if (widget && rd32(widget) == 0xFA588Cu)
        return widget;
    widget = 0;
    uint32_t p = 0x13232B8u;
    static const uint32_t steps[] = { 0x34u + 0x64u, 0x10u + 0x175A4u };
    p = rd32(p);
    for (unsigned i = 0; i < 2 && p; ++i)
        p = gwin_is_committed(p + steps[i]) ? rd32(p + steps[i]) : 0;
    if (p && gwin_is_committed(p) && rd32(p) == 0xFA588Cu)
        widget = p;
    return widget;
}

static uint32_t loading_sleep_filter(uint32_t site, uint32_t ms)
{
    /* the main loop's frame limiter (0xCC6D00) */
    return site == 0x00CC6D4Cu && ms > 1 && gt_loading() ? 1 : ms;
}

void xiv_loading_poll(void)
{
    static int on = -1;
    if (on < 0)
    {
        on = getenv("XIVLEGACY_SLOW_LOADING") == NULL; /* the game's own pace, to compare */
        if (on)
            k32_sleep_filter = loading_sleep_filter;
    }
    if (!on)
        return;
    uint32_t w = loading_widget();
    int loading = w && GUEST_PTR(w)[0x229] != 0;
    if (loading != gt_loading())
    {
        gt_set_loading(loading);
        if (getenv("XIVLEGACY_LOADINGLOG"))
            rt_log("[loading] %s\n", loading ? "screen up" : "screen down");
    }
    if (loading)
    {
        gt_unlock();
        plat_sleep_ms(LOADING_NAP_MS);
        gt_lock();
    }
}

/* XIVLEGACY_GAMELOG: the game's three log callbacks (0x12651B4, 0x12EF7E8, 0x12EE6F8), which its init
 * points at bare rets (0x6CE2E0, or 0x4E9310 with its debug flag), pointed at sh_game_log instead.
 * Called every frame: init may run again. */
void xiv_game_log_poll(void)
{
    static int on = -1;
    static uint32_t thunk;
    if (on < 0)
        on = getenv("XIVLEGACY_GAMELOG") != NULL;
    if (!on)
        return;
    if (!thunk)
    {
        thunk = thunk_for("xivlegacy", "GameLog");
#ifdef XIVLEGACY_SPLIT
        if (xiv_game->hook_streamlog)
            *xiv_game->hook_streamlog = hook_streamlog;
#else
        if (&rt_hook_streamlog)
            rt_hook_streamlog = hook_streamlog;
#endif
    }
    static const uint32_t slots[] = { 0x12651B4u, 0x12EF7E8u, 0x12EE6F8u };
    for (unsigned i = 0; i < 3; ++i)
    {
        uint32_t v = rd32(slots[i]);
        if ((v == 0x006CE2E0u || v == 0x004E9310u) && *GUEST_PTR(v) == 0xC3)
            wr32(slots[i], thunk);
    }
}

/* Native stand-ins for guest code the recompiler does not reach, each only where the bytes are
 * the expected ones (called after the image is mapped). */
void xiv_patches_apply(void)
{
    thunk_register(GAME_LOG);
    if (!memcmp(GUEST_PTR(0xF3D000u), MSS_CPU_CHECK, sizeof MSS_CPU_CHECK))
        thunk_register_at(0xF3D000u, sh_mss_cpu_features);
    else
        rt_log("[recomp] the MSSMIXER CPU check is not at 0xf3d000 in this build\n");
}

static const ShimDef XIV_MISC[] = {
    { "gdi32.dll", "GetStockObject", sh_GetStockObject },
    { "gdi32.dll", "CreateFontW", sh_CreateFontW },
    { "gdi32.dll", "CreatePen", sh_CreatePen },
    { "gdi32.dll", "MoveToEx", sh_MoveToEx },
    { "gdi32.dll", "LineTo", sh_LineTo },
    { "gdi32.dll", "Ellipse", sh_Ellipse },
    { "gdi32.dll", "TextOutA", sh_TextOutA },
    { "gdi32.dll", "GetTextMetricsA", sh_GetTextMetricsA },
    { "gdi32.dll", "GetTextExtentPoint32A", sh_GetTextExtentPoint32A },
    { "gdi32.dll", "GetGlyphOutlineW", sh_GetGlyphOutlineW },
    { "imm32.dll", "ImmAssociateContext", sh_ImmAssociateContext },
    { "imm32.dll", "ImmDisableTextFrameService", sh_ImmDisableTextFrameService },
    { "imm32.dll", "ImmGetCandidateListW", sh_ImmGetCandidateListW },
    { "imm32.dll", "ImmGetCompositionStringW", sh_ImmGetCompositionStringW },
    { "oleaut32.dll", "#8", sh_VariantInit },
    { "oleaut32.dll", "#9", sh_VariantClear },
    { "oleaut32.dll", "#12", sh_VariantChangeType },
    { "oleaut32.dll", "#15", sh_SafeArrayCreate },
    { "oleaut32.dll", "#26", sh_SafeArrayPutElement },
    { "shell32.dll", "SHGetSpecialFolderLocation", sh_SHGetSpecialFolderLocation },
    { "shell32.dll", "SHGetPathFromIDListW", sh_SHGetPathFromIDListW },
    { "shell32.dll", "SHGetPathFromIDListA", sh_SHGetPathFromIDListA },
    { "shell32.dll", "SHGetMalloc", sh_SHGetMalloc },
    { "shell32.dll", "ShellExecuteA", sh_ShellExecuteA },
    { "shell32.dll", "ShellExecuteW", sh_ShellExecuteW },
    { "shell32.dll", "IMalloc::QueryInterface", sh_IMalloc_QueryInterface },
    { "shell32.dll", "IMalloc::AddRef", sh_IMalloc_AddRef },
    { "shell32.dll", "IMalloc::Release", sh_IMalloc_Release },
    { "shell32.dll", "IMalloc::Alloc", sh_IMalloc_Alloc },
    { "shell32.dll", "IMalloc::Realloc", sh_IMalloc_Realloc },
    { "shell32.dll", "IMalloc::Free", sh_IMalloc_Free },
    { "shell32.dll", "IMalloc::GetSize", sh_IMalloc_GetSize },
    { "shell32.dll", "IMalloc::DidAlloc", sh_IMalloc_DidAlloc },
    { "shell32.dll", "IMalloc::HeapMinimize", sh_IMalloc_HeapMinimize },
    { "version.dll", "GetFileVersionInfoSizeW", sh_GetFileVersionInfoSizeW },
    { "version.dll", "GetFileVersionInfoSizeA", sh_GetFileVersionInfoSizeA },
    { "version.dll", "GetFileVersionInfoW", sh_GetFileVersionInfoW },
    { "version.dll", "GetFileVersionInfoA", sh_GetFileVersionInfoA },
    { "version.dll", "VerQueryValueW", sh_VerQueryValueW },
    { "version.dll", "VerQueryValueA", sh_VerQueryValueA },
    { "crypt32.dll", "CryptQueryObject", sh_CryptQueryObject },
    { "crypt32.dll", "CryptMsgGetParam", sh_CryptMsgGetParam },
    { "crypt32.dll", "CryptMsgClose", sh_CryptMsgClose },
    { "crypt32.dll", "CryptDecodeObject", sh_CryptDecodeObject },
    { "crypt32.dll", "CertFindCertificateInStore", sh_CertFindCertificateInStore },
    { "crypt32.dll", "CertFreeCertificateContext", sh_CertFreeCertificateContext },
    { "crypt32.dll", "CertCloseStore", sh_CertCloseStore },
    { "wintrust.dll", "WinVerifyTrust", sh_WinVerifyTrust },
    { NULL, NULL, NULL },
};

void xiv_misc_init(void)
{
    thunk_register(XIV_MISC);
}
