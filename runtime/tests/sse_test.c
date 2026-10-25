/* Semantics checks for runtime/sse.h against values worked out from the Intel SDM.
 *   cc -std=c11 -O1 -Wall -Wextra -I runtime runtime/tests/sse_test.c -lm && ./a.out */
#include <stdio.h>
#include <stdlib.h>

#include "sse.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static X128 f4(float a, float b, float c, float d) { X128 r; r.f[0] = a; r.f[1] = b; r.f[2] = c; r.f[3] = d; return r; }

int main(void) {
    unsigned zf, pf, cf, of, sf, af;

    /* arithmetic, scalar keeps the upper lanes */
    X128 a = f4(1, 2, 3, 4), b = f4(10, 20, 30, 40);
    X128 r = sse_addps(a, b);
    CHECK(r.f[0] == 11 && r.f[3] == 44);
    r = sse_mulss(a, b);
    CHECK(r.f[0] == 10 && r.f[1] == 2 && r.f[3] == 4);

    /* min/max return the second operand on NaN */
    X128 n = f4(NAN, 0, 0, 0);
    CHECK(sse_minss(n, f4(5, 0, 0, 0)).f[0] == 5);
    CHECK(isnan(sse_minss(f4(5, 0, 0, 0), n).f[0]));

    /* shufps: lanes 0,1 from d, 2,3 from s */
    r = sse_shufps(a, b, 0x1B); /* 3,2 from a ; 1,0 from b */
    CHECK(r.f[0] == 4 && r.f[1] == 3 && r.f[2] == 20 && r.f[3] == 10);
    r = sse_unpcklps(a, b);
    CHECK(r.f[0] == 1 && r.f[1] == 10 && r.f[2] == 2 && r.f[3] == 20);
    r = sse_unpckhps(a, b);
    CHECK(r.f[0] == 3 && r.f[1] == 30 && r.f[2] == 4 && r.f[3] == 40);

    /* compares */
    r = sse_cmpps(a, f4(1, 1, 5, NAN), 1); /* lt */
    CHECK(r.u[0] == 0 && r.u[1] == 0 && r.u[2] == 0xFFFFFFFFu && r.u[3] == 0);
    r = sse_cmpps(a, f4(1, 1, 5, NAN), 4); /* neq: unordered is true */
    CHECK(r.u[0] == 0 && r.u[1] == 0xFFFFFFFFu && r.u[3] == 0xFFFFFFFFu);
    SSE_COMI(1.0f, 2.0f);
    CHECK(cf == 1 && zf == 0 && pf == 0);
    SSE_COMI(NAN, 2.0);
    CHECK(cf == 1 && zf == 1 && pf == 1);
    SSE_COMI(3.0, 3.0);
    CHECK(cf == 0 && zf == 1);
    (void)of; (void)sf; (void)af;

    /* conversions */
    CHECK(sse_cvtt_i32(-2.7) == -2);
    CHECK(sse_cvt_i32(MXCSR_DEFAULT, 2.5) == 2);       /* nearest-even */
    CHECK(sse_cvt_i32(MXCSR_DEFAULT, 3.5) == 4);
    CHECK(sse_cvt_i32(MXCSR_DEFAULT | 0x6000u, -2.7) == -2); /* RC = toward zero */
    CHECK(sse_cvt_i32(MXCSR_DEFAULT | 0x2000u, -2.2) == -3); /* RC = down */
    CHECK(sse_cvtt_i32(3e10) == INT32_MIN);
    CHECK(sse_cvtt_i32(NAN) == INT32_MIN);
    r = sse_cvtps2pd(x_zero(), a);
    CHECK(r.d[0] == 1.0 && r.d[1] == 2.0);
    r = sse_cvtpd2ps(a, r);
    CHECK(r.f[0] == 1 && r.f[1] == 2 && r.u[2] == 0 && r.u[3] == 0);
    r = sse_cvtss2sd(f4(0, 0, 9, 9), f4(1.5f, 0, 0, 0));
    CHECK(r.d[0] == 1.5 && r.f[2] == 9);

    /* logic */
    X128 m = x_zero(); m.u[0] = 0x7FFFFFFF;
    r = sse_and(f4(-3, 0, 0, 0), m); /* fabs via andps */
    CHECK(r.f[0] == 3);
    r = sse_xor(f4(3, 0, 0, 0), sse_andn(m, f4(-1, 0, 0, 0))); /* flip sign */
    CHECK(r.f[0] == -3);

    /* integer, saturation, shifts */
    X128 x = x_zero(), y = x_zero();
    x.sw[0] = 30000; y.sw[0] = 10000;
    CHECK(sse_paddsw(x, y).sw[0] == 32767);
    x.b[0] = 250; y.b[0] = 10;
    CHECK(sse_paddusb(x, y).b[0] == 255);
    x.u[0] = 0x80000000u;
    CHECK(sse_psrad(x, 31).i[0] == -1 && sse_psrld(x, 31).u[0] == 1 && sse_pslld(x, 32).u[0] == 0);
    x = x_zero(); x.u[0] = 3; x.u[2] = 5; y = x_zero(); y.u[0] = 0xFFFFFFFFu; y.u[2] = 7;
    r = sse_pmuludq(x, y);
    CHECK(r.q[0] == 3ull * 0xFFFFFFFFull && r.q[1] == 35);
    x = x_zero(); x.b[0] = 1; x.b[15] = 2;
    CHECK(sse_pslldq(x, 1).b[1] == 1 && sse_psrldq(x, 15).b[0] == 2);

    /* pack / unpack, MMX width keeps only the low 8 bytes meaningful */
    x = x_zero(); y = x_zero(); x.i[0] = 70000; x.i[1] = -5; y.i[0] = 7; y.i[1] = -70000;
    r = sse_packssdw(x, y, 8);
    CHECK(r.sw[0] == 32767 && r.sw[1] == -5 && r.sw[2] == 7 && r.sw[3] == -32768);
    x = x_zero(); y = x_zero(); x.w[0] = 1; x.w[1] = 2; x.w[2] = 3; x.w[3] = 4; y.w[2] = 30; y.w[3] = 40;
    r = sse_punpckhwd(x, y, 4); /* MMX: high words 2,3 */
    CHECK(r.w[0] == 3 && r.w[1] == 30 && r.w[2] == 4 && r.w[3] == 40);
    r = sse_punpcklwd(x, y, 8); /* XMM */
    CHECK(r.w[0] == 1 && r.w[1] == 0 && r.w[2] == 2);

    /* shuffles */
    x = x_zero(); for (int k = 0; k < 4; k++) x.u[k] = 10 + k;
    r = sse_pshufd(x_zero(), x, 0x1B);
    CHECK(r.u[0] == 13 && r.u[3] == 10);

    /* masks */
    CHECK(sse_movmskps(f4(-1, 1, -1, 1)) == 5);
    x = x_zero(); x.b[0] = 0x80; x.b[9] = 0x80;
    CHECK(sse_pmovmskb(x, 16) == 0x201 && sse_pmovmskb(x, 8) == 1);

    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("sse.h: all checks passed\n");
    return 0;
}
