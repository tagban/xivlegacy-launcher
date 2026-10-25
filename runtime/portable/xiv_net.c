/* WinHTTP and iphlpapi for ffxivgame.exe.
 *
 * WinHTTP: sessions, connections and requests are handles; sending fails the way an unreachable
 * host does (ERROR_WINHTTP_CANNOT_CONNECT), and the host the game wanted is logged, so what it is
 * used for shows up before anything is wired to it.
 *
 * iphlpapi: one Ethernet adapter with a fixed, locally administered MAC address and a private IPv4
 * address - what the game reads to identify the machine, the same on every run. */
#include <stdio.h>
#include <string.h>

#include "gthread.h"
#include "kobj.h"
#include "plat.h"
#include "thunk.h"

#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_WINHTTP_CANNOT_CONNECT 12029u
#define ERROR_WINHTTP_HEADER_NOT_FOUND 12150u

static void narrow(uint32_t w, char* out, size_t n)
{
    size_t i = 0;
    if (w)
        for (; i + 1 < n; ++i)
        {
            uint16_t c = rd16(w + 2u * i);
            if (!c)
                break;
            out[i] = c < 0x80 ? (char)c : '?';
        }
    out[i] = 0;
}

/* --- WinHTTP ----------------------------------------------------------------------------------- */

static void sh_WinHttpOpen(Guest* g) { RET(k_new(K_OTHER, NULL, NULL), 5); }

/* WinHttpConnect(session, server, port, reserved) */
static void sh_WinHttpConnect(Guest* g)
{
    char host[256];
    narrow(ARG(1), host, sizeof host);
    rt_log("[winhttp] connect %s:%u (not reachable from here yet)\n", host, ARG(2));
    RET(k_new(K_OTHER, NULL, NULL), 4);
}

/* WinHttpOpenRequest(connect, verb, object, version, referrer, accept_types, flags) */
static void sh_WinHttpOpenRequest(Guest* g)
{
    char verb[16], path[512];
    narrow(ARG(1), verb, sizeof verb);
    narrow(ARG(2), path, sizeof path);
    rt_log("[winhttp] request %s %s\n", verb[0] ? verb : "GET", path);
    RET(k_new(K_OTHER, NULL, NULL), 7);
}

static void sh_WinHttpSetOption(Guest* g) { RET(1, 4); }
static void sh_WinHttpSetStatusCallback(Guest* g) { RET(0, 4); }

static void sh_WinHttpSendRequest(Guest* g)
{
    gt_set_error(ERROR_WINHTTP_CANNOT_CONNECT);
    RET(0, 7);
}

static void sh_WinHttpWriteData(Guest* g)
{
    if (ARG(3))
        wr32(ARG(3), 0);
    gt_set_error(ERROR_WINHTTP_CANNOT_CONNECT);
    RET(0, 4);
}

static void sh_WinHttpReceiveResponse(Guest* g)
{
    gt_set_error(ERROR_WINHTTP_CANNOT_CONNECT);
    RET(0, 2);
}

static void sh_WinHttpQueryHeaders(Guest* g)
{
    gt_set_error(ERROR_WINHTTP_HEADER_NOT_FOUND);
    RET(0, 6);
}

static void sh_WinHttpQueryDataAvailable(Guest* g)
{
    if (ARG(1))
        wr32(ARG(1), 0);
    gt_set_error(ERROR_WINHTTP_CANNOT_CONNECT);
    RET(0, 2);
}

static void sh_WinHttpReadData(Guest* g)
{
    if (ARG(3))
        wr32(ARG(3), 0);
    gt_set_error(ERROR_WINHTTP_CANNOT_CONNECT);
    RET(0, 4);
}

static void sh_WinHttpCloseHandle(Guest* g) { RET(k_close(ARG(0)), 1); }

/* --- iphlpapi ---------------------------------------------------------------------------------- */

#define IFROW_SIZE 860u
#define IPADDRROW_SIZE 24u
static const uint8_t MAC[6] = { 0x02, 0x58, 0x49, 0x56, 0x4C, 0x01 }; /* 02:"XIVL":01 */
#define ADDR 0x0200A8C0u /* 192.168.0.2, network order as stored */
#define MASK 0x00FFFFFFu /* 255.255.255.0 */

/* GetIfTable(MIB_IFTABLE*, ULONG* size, BOOL order) */
static void sh_GetIfTable(Guest* g)
{
    uint32_t p = ARG(0), sz = ARG(1), need = 4 + IFROW_SIZE;
    if (!sz)
        RET(ERROR_INVALID_PARAMETER, 3);
    if (!p || rd32(sz) < need)
    {
        wr32(sz, need);
        RET(ERROR_INSUFFICIENT_BUFFER, 3);
    }
    memset(GUEST_PTR(p), 0, need);
    wr32(p, 1);
    uint32_t r = p + 4;
    static const char name[] = "\\DEVICE\\TCPIP_{XIVLEGACY}";
    for (unsigned i = 0; name[i]; ++i)
        wr16(r + 2u * i, (uint16_t)name[i]);
    wr32(r + 512, 1);         /* dwIndex */
    wr32(r + 516, 6);         /* dwType: IF_TYPE_ETHERNET_CSMACD */
    wr32(r + 520, 1500);      /* dwMtu */
    wr32(r + 524, 1000000000u); /* dwSpeed */
    wr32(r + 528, 6);         /* dwPhysAddrLen */
    memcpy(GUEST_PTR(r + 532), MAC, 6);
    wr32(r + 540, 1);         /* dwAdminStatus: up */
    wr32(r + 544, 5);         /* dwOperStatus: operational */
    static const char descr[] = "XIVLegacy Network Adapter";
    wr32(r + 600, sizeof descr - 1);
    memcpy(GUEST_PTR(r + 604), descr, sizeof descr - 1);
    RET(0, 3);
}

/* GetIpAddrTable(MIB_IPADDRTABLE*, ULONG* size, BOOL order) */
static void sh_GetIpAddrTable(Guest* g)
{
    uint32_t p = ARG(0), sz = ARG(1), need = 4 + IPADDRROW_SIZE;
    if (!sz)
        RET(ERROR_INVALID_PARAMETER, 3);
    if (!p || rd32(sz) < need)
    {
        wr32(sz, need);
        RET(ERROR_INSUFFICIENT_BUFFER, 3);
    }
    memset(GUEST_PTR(p), 0, need);
    wr32(p, 1);
    wr32(p + 4, ADDR);
    wr32(p + 8, 1);     /* dwIndex: the adapter above */
    wr32(p + 12, MASK);
    wr32(p + 16, 1);    /* dwBCastAddr */
    wr32(p + 20, 0xFFFF); /* dwReasmSize */
    wr16(p + 26, 1);    /* wType: MIB_IPADDR_PRIMARY */
    RET(0, 3);
}

static const ShimDef NET[] = {
    { "winhttp.dll", "WinHttpOpen", sh_WinHttpOpen },
    { "winhttp.dll", "WinHttpConnect", sh_WinHttpConnect },
    { "winhttp.dll", "WinHttpOpenRequest", sh_WinHttpOpenRequest },
    { "winhttp.dll", "WinHttpSetOption", sh_WinHttpSetOption },
    { "winhttp.dll", "WinHttpSetStatusCallback", sh_WinHttpSetStatusCallback },
    { "winhttp.dll", "WinHttpSendRequest", sh_WinHttpSendRequest },
    { "winhttp.dll", "WinHttpWriteData", sh_WinHttpWriteData },
    { "winhttp.dll", "WinHttpReceiveResponse", sh_WinHttpReceiveResponse },
    { "winhttp.dll", "WinHttpQueryHeaders", sh_WinHttpQueryHeaders },
    { "winhttp.dll", "WinHttpQueryDataAvailable", sh_WinHttpQueryDataAvailable },
    { "winhttp.dll", "WinHttpReadData", sh_WinHttpReadData },
    { "winhttp.dll", "WinHttpCloseHandle", sh_WinHttpCloseHandle },
    { "iphlpapi.dll", "GetIfTable", sh_GetIfTable },
    { "iphlpapi.dll", "GetIpAddrTable", sh_GetIpAddrTable },
    { NULL, NULL, NULL },
};

void xiv_net_init(void) { thunk_register(NET); }
