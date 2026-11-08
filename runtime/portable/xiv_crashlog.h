/* What the log needs when the game dies: who freed the object it called into, and the last
 * game packets each way. Cheap enough to run all the time.
 *
 * - Frees: every HeapFree of a block whose first word is a vtable of the game image (a C++ object
 *   being deleted) is kept in a ring with the guest return addresses on the stack at the time.
 *   After a bad indirect call (FATAL) or a crash, each guest register, and the word it points at,
 *   is looked up there: "ecx 22290fe8 is inside a freed RaptureSchEffectController (vtable
 *   ff5418) freed from 0066621c <- 007d28d5 ...".
 * - Packets: the 1.0 framing (16-byte base packet, then subpackets) of every game TCP stream is
 *   followed, and the last subpackets each way (opcode, source and target actor, size, the first
 *   payload words) are kept in a ring.
 * - RaptureActor's and CharaActor's RTTI type names are checked on each critical section and
 *   restored if overwritten (a mistranslation did that, and every RaptureActor then passed for a
 *   CharaActor: the crash at 0x0065DD20 / 0x0065DD40, under 0x007D27A0).
 * - The game's deferred actor delete (1.23b): a queued CharaActor whose effect controller is gone
 *   (destroyed already) is not deleted again, or has the field cleared; entries that aren't objects,
 *   duplicates, actors back in their world's list and entries freed during the drain are taken out
 *   of the delete. See the .c. XIVLEGACY_ACTORLOG=1 logs every drop. */
#pragma once

#include <stdint.h>

#include "runtime.h"

/* The image ranges a vtable (rdata) and a return address (text) must lie in (pe.c sets them). */
void xcl_set_image(uint32_t text_lo, uint32_t text_hi, uint32_t rdata_lo, uint32_t rdata_hi);

/* HeapFree(p) by the game, with the guest stack pointer at the call. Before the block goes. */
void xcl_note_free(uint32_t p, uint32_t size, uint32_t esp);

/* EnterCriticalSection by the game: notes the game allocator's own frees (see the .c). */
void xcl_note_cs(uint32_t esp);

/* HeapAlloc by the game (the deferred-delete check), and the block it got. */
void xcl_note_alloc(uint32_t esp);
void xcl_note_got(uint32_t p);

/* After the notes above, with the guest: XIVLEGACY_CASTCHECK=1 diagnostics as a drain starts. */
void xcl_cast_check(Guest* g);

/* LeaveCriticalSection by the game: notes the game allocator's own allocations. */
void xcl_note_leave_cs(uint32_t esp, uint32_t esi);

/* Bytes on game TCP stream `stream` (a socket slot): dir 0 received, 1 sent. */
void xcl_packets(int stream, int dir, const uint8_t* buf, uint32_t n);
/* The stream at that slot was closed or reused: start its framing afresh. */
void xcl_stream_reset(int stream);

/* Writes the report for these guest registers (NULL: none) to fd. Async-signal tolerant: no
 * allocation, only snprintf and write. */
void xcl_report(int fd, const Guest* g);
