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
