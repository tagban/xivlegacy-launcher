/* xiv_crashlog.c deferred actor delete guard and packet ring, on a fake guest:
 *   clang -O1 -DRT_GUEST_WINDOW -Iruntime -Iruntime/portable runtime/tests/crashlog_test.c \
 *     runtime/portable/xiv_crashlog.c -o /tmp/clt && /tmp/clt */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "guest.h"
#include "xiv_crashlog.h"

unsigned char* rt_guest_base;
int gwin_is_committed(uint32_t addr) { return addr >= 0x10000u && addr < 0x10000000u; }
uint64_t plat_wall_ms(void) { return 0; }
void rt_call_indirect(Guest* g, uint32_t target) { (void)g, (void)target; }
void rt_log(const char* fmt, ...) { (void)fmt; }

#define TEXT_LO 0x00401000u
#define TEXT_HI 0x00E00000u
#define RDATA_LO 0x00F00000u
#define RDATA_HI 0x01300000u
#define VEC 0x0134DFA0u

static const uint8_t DRAIN_CODE[] = { 0x8D, 0x4C, 0x24, 0x28, 0x51, 0x8B, 0xCE, 0xE8, 0xBF, 0xF9, 0xFF, 0xFF,
    0x8D, 0x54, 0x24, 0x18, 0x52, 0x8B, 0xCE, 0xE8, 0xB3, 0xF9, 0xFF, 0xFF };
static const uint8_t FREE_CODE[] = { 0x55, 0xFF, 0x15, 0x6C, 0xE1, 0xF3, 0x00 };

static void put(uint32_t a, const uint8_t* b, uint32_t n) { memcpy(GUEST_PTR(a), b, n); }

int main(void)
{
    rt_guest_base = mmap(NULL, 0x10000000u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(rt_guest_base != MAP_FAILED);
    xcl_set_image(TEXT_LO, TEXT_HI, RDATA_LO, RDATA_HI);
    put(0x007D2DD5u, DRAIN_CODE, sizeof DRAIN_CODE);
    put(0x0040DF80u, FREE_CODE, sizeof FREE_CODE);
    static const uint8_t RES1[] = { 0x8D, 0x4C, 0x24, 0x2C, 0xE8, 0x50, 0xBB, 0xD0, 0xFF };
    static const uint8_t RES2[] = { 0x8D, 0x4C, 0x24, 0x1C, 0xE8, 0x2E, 0xBB, 0xD0, 0xFF };
    put(0x007D2CB0u - 9, RES1, sizeof RES1);
    put(0x007D2CD2u - 9, RES2, sizeof RES2);

    /* the two actor type names, as the image has them */
    static const char RAPTURE[] = ".?AVRaptureActor@Scene@Application@@", CHARA[] = ".?AVCharaActor@Chara@Actor@Scene@Application@@";
    put(0x012B77E8u, (const uint8_t*)RAPTURE, sizeof RAPTURE);
    put(0x012B7818u, (const uint8_t*)CHARA, sizeof CHARA);

    /* two actors with a vtable in .rdata whose first slot is in .text */
    const uint32_t A = 0x02000000u, B = 0x02001000u, VT = 0x00F00100u;
    wr32(VT, 0x00500000u), wr32(A, VT), wr32(B, VT);

    /* the global vector, seen holding them (arms the guard), then cleared by the drain */
    const uint32_t gb = 0x03000000u;
    wr32(VEC + 4, gb), wr32(VEC + 8, gb + 8), wr32(VEC + 12, gb + 0x100), wr32(gb, A), wr32(gb + 4, B);
    uint32_t idle = 0x04000000u; /* a stack with nothing on it */
    xcl_note_cs(idle);
    wr32(VEC + 8, gb);

    /* the drain's frame: the rest at E+0x28 (A), CommonResourceActors at E+0x18 (B); in 0x7D27A0 on the first */
    const uint32_t E = 0x05000000u, v1 = 0x03100000u, v2 = 0x03200000u;
    wr32(E + 0x2C, v1), wr32(E + 0x30, v1 + 4), wr32(v1, A);
    wr32(E + 0x1C, v2), wr32(E + 0x20, v2 + 4), wr32(v2, B);
    const uint32_t ret = E - 8;
    wr32(ret, 0x007D2DE1u), wr32(ret + 4, E + 0x28);

    /* A's destructor (the delete pass) frees B, which still waits in the other batch */
    uint32_t esp = ret - 0x80;
    wr32(esp + 0x40, 0x007D28D5u);
    wr32(esp, 0x0040DF87u), wr32(esp + 0x18, B);
    xcl_note_cs(esp);
    assert(rd32(v2) == 0 && "B is dropped from its batch");
    assert(rd32(v1) == A && "A is untouched");

    /* then A itself: its own slot goes too (harmless, the pass has read it) */
    wr32(esp + 0x18, A);
    xcl_note_cs(esp);
    assert(rd32(v1) == 0);

    /* a free with no drain on the stack leaves batches alone */
    wr32(v1, A), wr32(idle, 0x0040DF87u), wr32(idle + 0x18, A);
    xcl_note_cs(idle);
    assert(rd32(v1) == A);

    /* an EnterCriticalSection that isn't the allocator's free does nothing */
    wr32(esp, 0x00412345u), wr32(esp + 0x18, A);
    xcl_note_cs(esp);
    assert(rd32(v1) == A);

    /* the queue as the drain starts (its reserve's allocation): live actors stay, even one at an
     * address freed before (the allocators aren't all seen, so it may be live again); one queued
     * twice, one back in its world's list and a non-object go */
    {
        const uint32_t C = 0x02002000u, D = 0x02003000u, Z = 0x02004000u, W = 0x02005000u;
        wr32(C, VT), wr32(D, VT), wr32(Z, 0x3F000000u);
        wr32(A + 0x118, 0), wr32(C + 0x118, 0), wr32(D + 0x118, W);
        /* D's world: actor manager at +0xA4, its list's head node at +0x14; one node holding D */
        const uint32_t head = 0x02006000u, node = 0x02006100u;
        wr32(W + 0xA4 + 0x14, head), wr32(head, node), wr32(head + 4, node), wr32(node, head), wr32(node + 4, head),
            wr32(node + 8, D);
        xcl_note_free(C, 4, idle);
        wr32(gb, A), wr32(gb + 4, C), wr32(gb + 8, A), wr32(gb + 12, D), wr32(gb + 16, Z);
        wr32(VEC + 8, gb + 20);
        const uint32_t rs = 0x04100000u;
        wr32(rs + 0x40, 0x007D2CD2u);
        xcl_note_alloc(rs);
        assert(rd32(VEC + 8) == gb + 8 && rd32(gb) == A && rd32(gb + 4) == C && "A and C are left to delete");
        /* no drain on the stack: untouched */
        wr32(gb + 8, D), wr32(VEC + 8, gb + 12);
        xcl_note_alloc(idle);
        assert(rd32(VEC + 8) == gb + 12);
        wr32(VEC + 8, gb);

        /* during the drain, a batch entry whose memory turned into something else is dropped */
        wr32(v1, A), wr32(v1 + 4, Z), wr32(E + 0x30, v1 + 8), wr32(v2, D);
        wr32(esp, 0x0040DF87u), wr32(esp + 0x18, B);
        xcl_note_cs(esp);
        assert(rd32(v1) == A && rd32(v1 + 4) == 0 && rd32(v2) == D);
        wr32(E + 0x30, v1 + 4);
    }

    /* a CharaActor whose effect controller is gone: the field is cleared before the drain uses it */
    {
        /* RTTI: vtable[-1] -> locator (type descriptor at +0xC, hierarchy at +0x10) */
        const uint32_t VTC = 0x00F00200u, COL = 0x00F00300u, H = 0x00F00340u, ARR = 0x00F00360u, BCD = 0x00F00380u,
                       TD_OTHER = 0x00F00400u;
        wr32(VTC - 4, COL), wr32(VTC, 0x00500000u), wr32(COL + 0xC, TD_OTHER), wr32(COL + 0x10, H);
        wr32(H + 8, 1), wr32(H + 0xC, ARR), wr32(ARR, BCD), wr32(BCD, 0x012B7810u);
        const uint32_t P = 0x02007000u, Q = 0x02009000u, CT = 0x0200B000u;
        wr32(P, VTC), wr32(P + 0x12F0, CT), wr32(CT, 0x0000003Au); /* controller memory reused */
        wr32(Q, VT), wr32(Q + 0x12F0, CT);                           /* not a CharaActor: untouched */
        wr32(P + 0x118, 0), wr32(Q + 0x118, 0);
        wr32(gb, P), wr32(gb + 4, Q), wr32(VEC + 8, gb + 8);
        xcl_note_alloc(0x04100000u);
        assert(rd32(VEC + 8) == gb + 4 && rd32(gb) == Q && "P, destroyed already, is not deleted again");
        assert(rd32(Q + 0x12F0) == CT);
        /* a live CharaActor's controller freed during the drain: the field is cleared, the actor stays */
        wr32(VEC + 8, gb); /* the drain has taken the queue */
        wr32(CT, VT), wr32(P + 0x12F0, CT), wr32(v1, P), wr32(E + 0x30, v1 + 4);
        wr32(esp, 0x00412345u);
        xcl_note_free(CT, 0x58, esp);
        assert(rd32(P + 0x12F0) == 0 && rd32(v1) == P);
        /* one whose controller is garbage during the drain is dropped from the batch */
        wr32(P + 0x12F0, CT), wr32(CT, 0x0E8A798Cu);
        xcl_note_free(0x0200C000u, 0x10, esp);
        assert(rd32(v1) == 0);

        /* the id lookup's hook: a CharaActor whose block runs into unmapped memory is not found;
         * a whole one, another object, and NULL pass */
        static const uint8_t LOOKUP[] = { 0x8D, 0x88, 0xA4, 0x00, 0x00, 0x00, 0xE8 };
        put(0x00610D4Au, LOOKUP, sizeof LOOKUP);
        const uint32_t TAIL = 0x0FFFF000u; /* its +0x1000 is past the fake's committed window */
        wr32(TAIL, VTC);
        Guest lg;
        memset(&lg, 0, sizeof lg);
        lg.esp = idle;
        lg.eax = TAIL, xcl_actor_lookup(&lg);
        assert(lg.eax == 0 && "a CharaActor cut off by unmapped memory is not found");
        lg.eax = P, xcl_actor_lookup(&lg);
        assert(lg.eax == P);
        lg.eax = Q, xcl_actor_lookup(&lg);
        assert(lg.eax == Q);
        lg.eax = 0x0E8A798Cu, xcl_actor_lookup(&lg);
        assert(lg.eax == 0 && "not an object");
        wr32(VEC + 8, gb);
    }

    /* a blanked type name (any RaptureActor would cast to CharaActor) is put back; the spare word is left */
    wr32(0x012B7814u, 0x41E317C3u), wr32(0x012B7818u, 0), wr32(0x012B77E8u, 0);
    xcl_note_cs(idle);
    assert(!memcmp(GUEST_PTR(0x012B7818u), CHARA, sizeof CHARA) && !memcmp(GUEST_PTR(0x012B77E8u), RAPTURE, sizeof RAPTURE));
    assert(rd32(0x012B7814u) == 0x41E317C3u);

    /* packets: one 1.0 base packet with two subpackets, split across reads; then the report */
    uint8_t pk[16 + 0x30 + 0x28];
    memset(pk, 0, sizeof pk);
    pk[4] = (uint8_t)sizeof pk;
    pk[16] = 0x30, pk[16 + 2] = 3, pk[16 + 4] = 0x44, pk[16 + 0x12] = 0xCA;
    pk[16 + 0x30] = 0x28, pk[16 + 0x30 + 2] = 3, pk[16 + 0x30 + 0x12] = 0xCB;
    xcl_stream_reset(1);
    xcl_packets(1, 0, pk, 20);
    xcl_packets(1, 0, pk + 20, sizeof pk - 20);
    FILE* f = tmpfile();
    Guest g;
    memset(&g, 0, sizeof g);
    g.esp = esp, g.ecx = B;
    xcl_report(fileno(f), &g);
    char out[8192] = { 0 };
    rewind(f);
    fread(out, 1, sizeof out - 1, f);
    assert(strstr(out, "op 00ca") && strstr(out, "op 00cb") && "both subpackets are in the ring");
    assert(strstr(out, "ecx 02001000 is 0 bytes into") && "the register is named as freed");

    printf("crashlog_test: ok\n");
    return 0;
}
