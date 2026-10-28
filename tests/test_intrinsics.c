/* Unit tests for the portable Cortex-M DSP intrinsics in shim/stm32f7xx_hal.h.
   Every expected value is hand-computed from the ARM architecture semantics:
   QADD16/QSUB16 saturate each signed 16-bit lane to [-32768, 32767]; SADD16 wraps;
   SMLALD accumulates lo*lo + hi*hi (signed 16x16) into a 64-bit result; SSAT clamps
   to a signed n-bit range; PKHBT packs a's low halfword under b's shifted high halfword.
   Run with: make test */
#include <stdio.h>
#include <stdint.h>
#include "stm32f7xx_hal.h"

static int run, failed;
static void ck32(const char *what, uint32_t got, uint32_t want)
{
	run++;
	if (got != want) { failed++; printf("FAIL %s: got 0x%08X want 0x%08X\n", what, got, want); }
}
static void ck64(const char *what, int64_t got, int64_t want)
{
	run++;
	if (got != want) { failed++; printf("FAIL %s: got %lld want %lld\n", what, (long long)got, (long long)want); }
}
static void ckS(const char *what, int32_t got, int32_t want)
{
	run++;
	if (got != want) { failed++; printf("FAIL %s: got %d want %d\n", what, got, want); }
}

int main(void)
{
	/* __QADD16: lane-wise saturating add */
	ck32("QADD16 plain",        __QADD16(0x00010002u, 0x00030004u), 0x00040006u);
	ck32("QADD16 sat hi lane",  __QADD16(0x7FFF0001u, 0x00010001u), 0x7FFF0002u);   /* 32767+1 -> 32767 */
	ck32("QADD16 sat lo lane",  __QADD16(0x00018000u, 0x0000FFFFu), 0x00018000u);   /* lo: -32768 + -1 -> -32768; hi: 1 + 0 */
	ck32("QADD16 both extreme", __QADD16(0x4000C000u, 0x40004000u), 0x7FFF0000u);   /* hi 16384+16384 -> sat; lo -16384+16384 = 0 */
	ck32("QADD16 neg plain",    __QADD16(0xFFFEFFFDu, 0xFFFEFFFDu), 0xFFFCFFFAu);   /* -2 + -2, -3 + -3 */

	/* __QSUB16: lane-wise saturating subtract */
	ck32("QSUB16 plain",        __QSUB16(0x00050003u, 0x00020005u), 0x0003FFFEu);   /* 5-2, 3-5 */
	ck32("QSUB16 sat low",      __QSUB16(0x00008000u, 0x00000001u), 0x00008000u);   /* -32768-1 -> -32768 */
	ck32("QSUB16 sat high",     __QSUB16(0x7FFF0000u, 0xFFFF0000u), 0x7FFF0000u);   /* 32767-(-1) -> 32767 */
	ck32("QSUB16 zero",         __QSUB16(0x12345678u, 0x12345678u), 0x00000000u);

	/* __SADD16: lane-wise wrapping add (no saturation) */
	ck32("SADD16 plain",        __SADD16(0x00010002u, 0x00030004u), 0x00040006u);
	ck32("SADD16 wraps",        __SADD16(0x7FFF0000u, 0x00010000u), 0x80000000u);   /* 32767+1 wraps to -32768 */
	ck32("SADD16 wraps neg",    __SADD16(0x80000000u, 0xFFFF0000u), 0x7FFF0000u);   /* -32768 + -1 wraps to 32767 */

	/* __SMLALD: acc + lo(a)*lo(b) + hi(a)*hi(b), signed halfwords */
	ck64("SMLALD plain",        __SMLALD(0x00020003u, 0x00040005u, 100), 123);      /* 100 + 3*5 + 2*4 */
	ck64("SMLALD negatives",    __SMLALD(0x0002FFFFu, 0xFFFC0003u, 0), -11);        /* (-1)*3 + 2*(-4) */
	ck64("SMLALD max lanes",    __SMLALD(0x7FFF7FFFu, 0x7FFF7FFFu, 0), 2147352578LL); /* 2 * 32767^2 */
	ck64("SMLALD min lane",     __SMLALD(0x00008000u, 0x00008000u, -1073741824LL), 0); /* (-32768)^2 cancels acc */
	/* __SMLALDX: b's halfwords swapped */
	ck64("SMLALDX plain",       __SMLALDX(0x00020003u, 0x00040005u, 0), 22);        /* 3*4 + 2*5 */
	ck64("SMLALDX acc",         __SMLALDX(0x00010001u, 0x00020003u, 1000), 1005);   /* 1000 + 1*2 + 1*3 */

	/* __SSAT: clamp to signed n-bit range */
	int32_t x;
	x = 100;        ckS("SSAT in range",   __SSAT(x, 8), 100);
	x = 200;        ckS("SSAT sat high",   __SSAT(x, 8), 127);
	x = -200;       ckS("SSAT sat low",    __SSAT(x, 8), -128);
	x = 2147483647; ckS("SSAT 16 high",    __SSAT(x, 16), 32767);
	x = -2147483647 - 1; ckS("SSAT 16 low", __SSAT(x, 16), -32768);
	x = 8388608;    ckS("SSAT 24 high",    __SSAT(x, 24), 8388607);
	x = -5;         ckS("SSAT neg small",  __SSAT(x, 4), -5);

	/* __PKHBT: low half from a, high half from (b << shift) */
	ck32("PKHBT shift 8",  __PKHBT(0xAABBCCDDu, 0x11223344u, 8),  0x2233CCDDu);
	ck32("PKHBT shift 0",  __PKHBT(0x0000FFFFu, 0x12345678u, 0),  0x1234FFFFu);   /* high half = (b << 0)[31:16] = b's own high half */
	ck32("PKHBT shift 16", __PKHBT(0xDEADBEEFu, 0x0000ABCDu, 16), 0xABCDBEEFu);

	printf("%d/%d intrinsics checks passed\n", run - failed, run);
	return failed ? 1 : 0;
}
