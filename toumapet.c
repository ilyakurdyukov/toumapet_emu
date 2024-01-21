/*
 * Copyright (c) 2024, Ilya Kurdyukov
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE
 * FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY
 * DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include "window.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifndef CPU_TRACE
#define CPU_TRACE 0
#endif

//#define TICK_LIMIT 1000000

#define ERR_EXIT(...) do { \
	if (glob_sys) sys_close(glob_sys); \
	fprintf(stderr, __VA_ARGS__); \
	exit(1); \
} while (0)

static void sys_sleep(int ms) {
#if USE_SDL
	SDL_Delay(ms);
#elif defined(_WIN32)
	Sleep(ms);
#elif 1
	struct timeval t;
	t.tv_sec = 0;
	t.tv_usec = ms * 1000;
	select(0, 0, 0, 0, &t);
#else
	usleep(ms * 1000);
#endif
}

#define FRAME_STACK_MAX 16

typedef struct {
	uint32_t addr; uint16_t size, type;
} frame_t;

#define SCREEN_W 128
// OK-550: 128, OK-560: 160
#define SCREEN_H_MAX 160

typedef struct {
	uint8_t state, cmd, narg, flags;
	uint8_t args[3];
	uint32_t addr;
} flash_t;

typedef struct {
	uint8_t *rom;
	uint32_t rom_size, save_offs;
	uint8_t rom_key, init_done, frame_depth;
	uint8_t keymap[5];
	flash_t flash;
	unsigned zoom, keys, model, screen_h;
	window_t window;
#if !USE_SDL && defined(_WIN32)
	double time_mul;
#endif
#if CPU_TRACE
	char *log_buf;
	unsigned log_pos, log_size, log_overflow;
	const char *log_fn;
#endif
	frame_t frame_stack[FRAME_STACK_MAX];
	uint32_t pal[256];
	uint8_t screen[SCREEN_W * SCREEN_H_MAX];
} sysctx_t;

static sysctx_t *glob_sys = NULL;

static uint32_t sys_time_ms(sysctx_t *sys) {
#if USE_SDL
	return SDL_GetTicks();
#elif defined(_WIN32)
	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	return q.QuadPart * sys->time_mul;
#else
	struct timeval time;
	gettimeofday(&time, NULL);
	return time.tv_sec * 1000 + time.tv_usec / 1000;
#endif
}

static void sys_close(sysctx_t *sys) {
	window_close(&sys->window);
#if CPU_TRACE
	if (sys->log_fn) {
		char *buf = sys->log_buf;
		unsigned pos = sys->log_pos;
		unsigned size = sys->log_size;
		unsigned pos2 = pos, n = 0;
		FILE *f;
		if (sys->log_overflow) {
			do {
				if (buf[pos2++] == '\n') break;
				if (pos2 == size) pos2 = 0;
			} while (pos2 != pos);
			if (pos2 < pos) n = pos - pos2, pos = 0;
			else n = size - pos2;
		}
		f = fopen(sys->log_fn, "wb");
		if (f) {
			fwrite(buf + pos2, 1, n, f);
			fwrite(buf, 1, pos, f);
			fclose(f);
		}
	}
#endif
}

static void sys_init(sysctx_t *sys) {
	int w = SCREEN_W * sys->zoom;
	int h = sys->screen_h * sys->zoom;
	int i, as, rs, gs, bs;
	const char *err = window_init(&sys->window, "ToumaPet", w, h);
	if (err) ERR_EXIT("%s\n", err);

#if !USE_SDL && defined(_WIN32)
	{
		LARGE_INTEGER q;
		QueryPerformanceFrequency(&q);
		sys->time_mul = 1000.0 / q.QuadPart;
	}
#endif

	rs = sys->window.red << 3;
	as = rs & 16 ? -8 : 8;
	gs = rs + as; bs = gs + as;
	as = (rs - as) & 24;

	for (i = 0; i < 256; i++) {
		unsigned r, g, b;
#if 1
		// pow(i * 1.0 / 7, 2.0) * 255 + 0.5
		uint8_t gamma3[] = { 0, 5, 21, 47, 83, 130, 187, 255 };
		// pow(i * 1.0 / 3, 2.0) * 255 + 0.5
		uint8_t gamma2[] = { 0, 28, 113, 255 };
		r = gamma3[i >> 5 & 7];
		g = gamma3[i >> 2 & 7];
		b = gamma2[i & 3];
#else
		r = (i >> 5 & 7) * 0x49 >> 1;
		g = (i >> 2 & 7) * 0x49 >> 1;
		b = (i & 3) * 0x55;
#endif
		sys->pal[i] = r << rs | g << gs | b << bs | 0xff << as;
	}
}

static void sys_update(sysctx_t *sys) {
	uint32_t c, *d = sys->window.imagedata;
	unsigned st = sys->window.stride >> 2;
	uint8_t *s = sys->screen;
	int j, x, y, w = SCREEN_W, h = sys->screen_h;

#define X d[j++] = c
#define M(m, X) case m: \
	for (y = 0; y < h; y++, d += st * m) { \
		for (j = x = 0; x < w; x++) { \
			c = sys->pal[*s++]; X; \
		} \
		if (m > 1) { \
	    memcpy(d + st, d, st * 4); \
			if (m > 2) memcpy(d + st * 2, d, st * (m < 4 ? m - 2 : 2) * 4); \
			if (m > 4) memcpy(d + st * 4, d, st * (m - 4) * 4); \
		} \
	} break;

	switch (sys->zoom) {
		M(1, X) M(2, X;X) M(3, X;X;X)
		M(4, X;X;X;X) M(5, X;X;X;X;X)
	}
#undef M
#undef X

	window_update(&sys->window);
}

typedef struct {
	uint16_t pc;
	uint8_t a, x, y, sp;
	uint8_t flags, dummy;
	uint8_t mem[0x10000];
} cpu_state_t;

enum {
	MASK_C = 1,
	MASK_Z = 2,
	MASK_I = 4,
	MASK_D = 8,
	MASK_B = 0x10,
	// 0x20 is unused
	MASK_V = 0x40,
	MASK_N = 0x80,
};

enum {
	MOD_NUL, /* none */
	MOD_IMM, /* # */
	MOD_ACC, /* A */
	MOD_X,   /* X */
	MOD_Y,   /* Y */
	MOD_Z,   /* zp */
	MOD_ZX,  /* zp,x */
	MOD_ZY,  /* zp,y */
	MOD_ZI,  /* (zp) */
	MOD_ZXI, /* (zp,x) */
	MOD_ZIY, /* (zp),y */
	MOD_A,   /* a */
	MOD_AX,  /* a,x */
	MOD_AY,  /* a,y */
	MOD_R,   /* r */
	MOD_LAST,
	MOD_ZR = MOD_Z /* zp + r */
};

#define X(m, cmt) MOD_##m,
#define S(m, cmt) MOD_##m | 0x80,
static const uint8_t op_mod[256] = {
	X(NUL, "0x00 BRK") X(ZXI, "0x01 ORA") X(NUL, "0x02 ---") X(NUL, "0x03 ---")
	X(Z,   "0x04 TSB") X(Z,   "0x05 ORA") X(Z,   "0x06 ASL") X(Z,   "0x07 RMB")
	X(NUL, "0x08 PHP") X(IMM, "0x09 ORA") X(ACC, "0x0A ASL") X(NUL, "0x0B ---")
	X(A,   "0x0C TSB") X(A,   "0x0D ORA") X(A,   "0x0E ASL") X(ZR,  "0x0F BBR")

	X(R,   "0x10 BPL") X(ZIY, "0x11 ORA") X(ZI,  "0x12 ORA") X(NUL, "0x13 ---")
	X(Z,   "0x14 TRB") X(ZX,  "0x15 ORA") X(ZX,  "0x16 ASL") X(Z,   "0x17 RMB")
	X(NUL, "0x18 CLC") X(AY,  "0x19 ORA") X(ACC, "0x1A INC") X(NUL, "0x1B ---")
	X(A,   "0x1C TRB") X(AX,  "0x1D ORA") X(AX,  "0x1E ASL") X(ZR,  "0x1F BBR")

	X(IMM, "0x20 JSR") X(ZXI, "0x21 AND") X(NUL, "0x22 ---") X(NUL, "0x23 ---")
	X(Z,   "0x24 BIT") X(Z,   "0x25 AND") X(Z,   "0x26 ROL") X(Z,   "0x27 RMB")
	X(NUL, "0x28 PLP") X(IMM, "0x29 AND") X(ACC, "0x2A ROL") X(NUL, "0x2B ---")
	X(A,   "0x2C BIT") X(A,   "0x2D AND") X(A,   "0x2E ROL") X(ZR,  "0x2F BBR")

	X(R,   "0x30 BMI") X(ZIY, "0x31 AND") X(ZI,  "0x32 AND") X(NUL, "0x33 ---")
	X(Z,   "0x34 BIT") X(ZX,  "0x35 AND") X(ZX,  "0x36 ROL") X(Z,   "0x37 RMB")
	X(NUL, "0x38 SEC") X(AY,  "0x39 AND") X(ACC, "0x3A DEC") X(NUL, "0x3B ---")
	X(AX,  "0x3C BIT") X(AX,  "0x3D AND") X(AX,  "0x3E ROL") X(ZR,  "0x3F BBR")

	X(NUL, "0x40 RTI") X(ZXI, "0x41 EOR") X(NUL, "0x42 ---") X(NUL, "0x43 ---")
	X(NUL, "0x44 ---") X(Z,   "0x45 EOR") X(Z,   "0x46 LSR") X(Z,   "0x47 RMB")
	X(ACC, "0x48 PHA") X(IMM, "0x49 EOR") X(ACC, "0x4A LSR") X(NUL, "0x4B ---")
	X(IMM, "0x4C JMP") X(A,   "0x4D EOR") X(A,   "0x4E LSR") X(ZR,  "0x4F BBR")

	X(R,   "0x50 BVC") X(ZIY, "0x51 EOR") X(ZI,  "0x52 EOR") X(NUL, "0x53 ---")
	X(NUL, "0x54 ---") X(ZX,  "0x55 EOR") X(ZX,  "0x56 LSR") X(Z,   "0x57 RMB")
	X(NUL, "0x58 CLI") X(AY,  "0x59 EOR") X(Y,   "0x5A PHY") X(NUL, "0x5B ---")
	X(NUL, "0x5C ---") X(AX,  "0x5D EOR") X(AX,  "0x5E LSR") X(ZR,  "0x5F BBR")

	X(NUL, "0x60 RTS") X(ZXI, "0x61 ADC") X(NUL, "0x62 ---") X(NUL, "0x63 ---")
	S(Z,   "0x64 STZ") X(Z,   "0x65 ADC") X(Z,   "0x66 ROR") X(Z,   "0x67 RMB")
	X(ACC, "0x68 PLA") X(IMM, "0x69 ADC") X(ACC, "0x6A ROR") X(NUL, "0x6B ---")
	X(A,   "0x6C JMP") X(A,   "0x6D ADC") X(A,   "0x6E ROR") X(ZR,  "0x6F BBR")

	X(R,   "0x70 BVS") X(ZIY, "0x71 ADC") X(ZI,  "0x72 ADC") X(NUL, "0x73 ---")
	S(ZX,  "0x74 STZ") X(ZX,  "0x75 ADC") X(ZX,  "0x76 ROR") X(Z,   "0x77 RMB")
	X(NUL, "0x78 SEI") X(AY,  "0x79 ADC") X(Y,   "0x7A PLY") X(NUL, "0x7B ---")
	X(AX,  "0x7C JMP") X(AX,  "0x7D ADC") X(AX,  "0x7E ROR") X(ZR,  "0x7F BBR")

	X(R,   "0x80 BRA") S(ZXI, "0x81 STA") X(NUL, "0x82 ---") X(NUL, "0x83 ---")
	S(Z,   "0x84 STY") S(Z,   "0x85 STA") S(Z,   "0x86 STX") X(Z,   "0x87 SMB")
	X(Y,   "0x88 DEY") X(IMM, "0x89 BIT") X(NUL, "0x8A TXA") X(NUL, "0x8B ---")
	S(A,   "0x8C STY") S(A,   "0x8D STA") S(A,   "0x8E STX") X(ZR,  "0x8F BBS")

	X(R,   "0x90 BCC") S(ZIY, "0x91 STA") S(ZI,  "0x92 STA") X(NUL, "0x93 ---")
	S(ZX,  "0x94 STY") S(ZX,  "0x95 STA") S(ZY,  "0x96 STX") X(Z,   "0x97 SMB")
	X(NUL, "0x98 TYA") S(AY,  "0x99 STA") X(NUL, "0x9A TXS") X(NUL, "0x9B ---")
	S(A,   "0x9C STZ") S(AX,  "0x9D STA") S(AX,  "0x9E STZ") X(ZR,  "0x9F BBS")

	X(IMM, "0xA0 LDY") X(ZXI, "0xA1 LDA") X(IMM, "0xA2 LDX") X(NUL, "0xA3 ---")
	X(Z,   "0xA4 LDY") X(Z,   "0xA5 LDA") X(Z,   "0xA6 LDX") X(Z,   "0xA7 SMB")
	X(NUL, "0xA8 TAY") X(IMM, "0xA9 LDA") X(NUL, "0xAA TAX") X(NUL, "0xAB ---")
	X(A,   "0xAC LDY") X(A,   "0xAD LDA") X(A,   "0xAE LDX") X(ZR,  "0xAF BBS")

	X(R,   "0xB0 BCS") X(ZIY, "0xB1 LDA") X(ZI,  "0xB2 LDA") X(NUL, "0xB3 ---")
	X(ZX,  "0xB4 LDY") X(ZX,  "0xB5 LDA") X(ZY,  "0xB6 LDX") X(Z,   "0xB7 SMB")
	X(NUL, "0xB8 CLV") X(AY,  "0xB9 LDA") X(NUL, "0xBA TSX") X(NUL, "0xBB ---")
	X(AX,  "0xBC LDY") X(AX,  "0xBD LDA") X(AY,  "0xBE LDX") X(ZR,  "0xBF BBS")

	X(IMM, "0xC0 CPY") X(ZXI, "0xC1 CMP") X(NUL, "0xC2 ---") X(NUL, "0xC3 ---")
	X(Z,   "0xC4 CPY") X(Z,   "0xC5 CMP") X(Z,   "0xC6 DEC") X(Z,   "0xC7 SMB")
	X(Y,   "0xC8 INY") X(IMM, "0xC9 CMP") X(X,   "0xCA DEX") X(NUL, "0xCB WAI")
	X(A,   "0xCC CPY") X(A,   "0xCD CMP") X(A,   "0xCE DEC") X(ZR,  "0xCF BBS")

	X(R,   "0xD0 BNE") X(ZIY, "0xD1 CMP") X(ZI,  "0xD2 CMP") X(NUL, "0xD3 ---")
	X(NUL, "0xD4 ---") X(ZX,  "0xD5 CMP") X(ZX,  "0xD6 DEC") X(Z,   "0xD7 SMB")
	X(NUL, "0xD8 CLD") X(AY,  "0xD9 CMP") X(X,   "0xDA PHX") X(NUL, "0xDB STP")
	X(NUL, "0xDC ---") X(AX,  "0xDD CMP") X(AX,  "0xDE DEC") X(ZR,  "0xDF BBS")

	X(IMM, "0xE0 CPX") X(ZXI, "0xE1 SBC") X(NUL, "0xE2 ---") X(NUL, "0xE3 ---")
	X(Z,   "0xE4 CPX") X(Z,   "0xE5 SBC") X(Z,   "0xE6 INC") X(Z,   "0xE7 SMB")
	X(X,   "0xE8 INX") X(IMM, "0xE9 SBC") X(NUL, "0xEA NOP") X(NUL, "0xEB ---")
	X(A,   "0xEC CPX") X(A,   "0xED SBC") X(A,   "0xEE INC") X(ZR,  "0xEF BBS")

	X(R,   "0xF0 BEQ") X(ZIY, "0xF1 SBC") X(ZI,  "0xF2 SBC") X(NUL, "0xF3 ---")
	X(NUL, "0xF4 ---") X(ZX,  "0xF5 SBC") X(ZX,  "0xF6 INC") X(Z,   "0xF7 SMB")
	X(NUL, "0xF8 SED") X(AY,  "0xF9 SBC") X(X,   "0xFA PLX") X(NUL, "0xFB ---")
	X(NUL, "0xFC ---") X(AX,  "0xFD SBC") X(AX,  "0xFE INC") X(ZR,  "0xFF BBS")
#undef X
};

#define UNPACK_FLAGS \
	zflag = ~t & 2; \
	nflag = t; vflag = t << 1; \
	cflag = (t & 1) << 8;

#define PACK_FLAGS \
	t = s->flags & ~0xc3; \
	t |= cflag >> 8; \
	t |= !zflag << 1; \
	t |= vflag >> 1 & 0x40; \
	t |= nflag & 0x80;

#if CPU_TRACE
#include <stdarg.h>
static void trace_printf(sysctx_t *sys, const char *fmt, ...) {
	va_list va; int n;
	char *buf = sys->log_buf;
	unsigned pos = sys->log_pos, size;
	if (!sys->log_buf) return;
	va_start(va, fmt);
	n = vsnprintf(buf + pos, 256, fmt, va);
	va_end(va);
	size = sys->log_size;
	pos += n;
	if (pos >= size) {
		pos -= size;
		sys->log_overflow = 1;
		memcpy(buf, buf + size, pos);
	}
	sys->log_pos = pos;
}

#define TRACE(...) trace_printf(sys, __VA_ARGS__)
#else
#define TRACE(...) (void)0
#endif

#define READ16(p) ((p)[0] | (p)[1] << 8)
#define READ24(p) ((p)[0] | (p)[1] << 8 | (p)[2] << 16)
#define WRITE16(p, a) do { unsigned __tmp = (a); \
	(p)[0] = __tmp; (p)[1] = __tmp >> 8; \
} while (0)
#define WRITE24(p, a) do { unsigned __tmp = (a); \
	(p)[0] = __tmp; (p)[1] = __tmp >> 8; (p)[2] = __tmp >> 16; \
} while (0)

static unsigned get_image(sysctx_t *sys, unsigned id) {
	unsigned rom_size = sys->rom_size;
	unsigned res_offs = READ24(sys->rom);
	res_offs += id * 3;
	if (rom_size < res_offs + 3)
		ERR_EXIT("bad resource index (%u)", id);
	res_offs = READ24(sys->rom + res_offs);
	if (rom_size < res_offs + 4)
		ERR_EXIT("bad resource offset (0x%x)", res_offs);
	return res_offs;
}

static void draw_image(sysctx_t *sys, int x, int y,
		unsigned pos, int flip, int blend, int alpha) {
	int w, h, x_skip = 0, y_skip = 0;
	uint8_t *d, *src = sys->rom + pos;
	uint32_t size = sys->rom_size - pos - 4;
	int x_add, y_add, w2, h2;
	int screen_h = sys->screen_h;

	if (src[1] != 0 || src[3] != 0x80)
		ERR_EXIT("unsupported image\n");
	w2 = w = src[0]; h2 = h = src[2]; src += 4;

	if (flip > 3) ERR_EXIT("unsupported flip\n");

	if (x >= SCREEN_W) x = (int8_t)x, x_skip = -x;
	if (y >= screen_h) y = (int8_t)y, y_skip = -y;

	if (x > SCREEN_W || y > screen_h) return;
	if (x + w > SCREEN_W) w = SCREEN_W - x;
	if (y + h > screen_h) h = screen_h - y;
	d = &sys->screen[y * SCREEN_W + x];

	x_add = 1; y_add = SCREEN_W;
	if (flip & 1) {
		d += w2 - 1; x_add = -x_add;
		x = w; w = w2 - x_skip;
		x_skip = w2 - x;
	}
	if (flip & 2) {
		d += (h2 - 1) * y_add; y_add = -y_add;
		y = h; h = h2 - y_skip;
		y_skip = h2 - y;
	}
	if (w <= 0 || h <= 0) return;
	do {
		int len = READ16(src);
		uint8_t *s = src + 2, *d2 = d;
		int a = 0, n = 1, skip = x_skip;

		if ((int)size < len)
			ERR_EXIT("read outside the ROM\n");
		src += len; size -= len; d += y_add; len -= 4;
		if (--y_skip >= 0) continue;
		w2 = w; do {
			if (!--n) {
				if ((len -= 1) < 0) ERR_EXIT("RLE error\n");
				a = *s++; n = 1;
				if (!a) {
					if ((len -= 2) < 0) ERR_EXIT("RLE error\n");
					a = *s++; n = *s++;
					if (!n) ERR_EXIT("zero RLE count\n");
				}
			}
			if (--skip < 0 && a != alpha) {
				int x = a;
#define X(m) (((x & m) + (blend & m)) & m << 1)
				if (blend != 0xff) x = (X(0xe3) | X(0x1c)) >> 1;
#undef X
				*d2 = x;
			}
			d2 += x_add;
		} while (--w2);
	} while (--h);
}

static void draw_char(sysctx_t *sys, int x, int y,
		unsigned id, int color, int bg) {
	int w = 8, h = 16; uint8_t *d, *s;
	int screen_h = sys->screen_h;
	unsigned pos = READ16(sys->rom + 7);

	if (id < 0x20) ERR_EXIT("unsupported char\n");
	id -= 0x20;
	pos += id << 4;
	if (sys->rom_size < pos + 16)
		ERR_EXIT("read outside the ROM\n");

	if (x > SCREEN_W || y > screen_h) return;
	if (x + w > SCREEN_W) w = SCREEN_W - x;
	if (y + h > screen_h) h = screen_h - y;
	d = &sys->screen[y * SCREEN_W + x];
	s = sys->rom + pos;
	for (y = 0; y < h; y++, d += SCREEN_W) {
		int a = *s++;
		for (x = 0; x < w; x++, a <<= 1)
			if (a & 0x80) d[x] = color;
			else if (bg >= 0) d[x] = bg;
	}
}

static void bios_06(sysctx_t *sys, cpu_state_t *s) {
	unsigned rom_size = sys->rom_size;
	unsigned id = READ16(s->mem + 0x100);
	unsigned res_offs;
	WRITE16(s->mem + 0x102, id); // probably copies the id
	TRACE("image_size (id = %u)", id);
	res_offs = get_image(sys, id);
	s->mem[0x85] = sys->rom[res_offs]; // width
	s->mem[0x86] = sys->rom[res_offs + 2]; // height
}

static void bios_08(sysctx_t *sys, cpu_state_t *s) {
	int x = s->mem[0x100];
	int y = s->mem[0x101];
	int id = READ16(s->mem + 0x102);
	TRACE("image_draw_alpha (x = %u, y = %u, id = %u, flip = %u, blend = 0x%02x)",
			x, y, id, s->mem[0x104], s->mem[0x105]);
	draw_image(sys, x, y, get_image(sys, id), s->mem[0x104], s->mem[0x105], 0xff);
}

static void bios_0a(sysctx_t *sys, cpu_state_t *s) {
	int x = s->mem[0x100];
	int y = s->mem[0x101];
	int id = READ16(s->mem + 0x102);
	unsigned res_offs = get_image(sys, id);
	TRACE("image_draw (x = %u, y = %u, id = %u, flip = %u, blend = 0x%02x)",
			x, y, id, s->mem[0x104], s->mem[0x105]);
	draw_image(sys, x, y, get_image(sys, id), s->mem[0x104], s->mem[0x105], -1);
}

static void bios_0c(sysctx_t *sys, cpu_state_t *s) {
	int start = s->mem[0x100];
	int end = s->mem[0x101];
	int color = s->mem[0x102];
	int screen_h = sys->screen_h;
	TRACE("clear_screen (start = %u, end = %u, color = 0x%02x)",
			start, end, color);
	end++;
	if (end > screen_h) end = screen_h;
	if (start >= end) return;
	end -= start;
	memset(sys->screen + start * SCREEN_W, color, SCREEN_W * end);
}

static void bios_0e(sysctx_t *sys, cpu_state_t *s) {
	int start = s->mem[0x100];
	int end = s->mem[0x101];
	int x, y, w, h;
	int id = READ16(s->mem + 0x102);
	int screen_h = sys->screen_h;
	unsigned res_offs;
	TRACE("repeat_line (start = %u, end = %u, id = %u)",
			start, end, id);
	res_offs = get_image(sys, id);
	w = sys->rom[res_offs];
	h = sys->rom[res_offs + 2];
	end++;
	if (w == 1) {
		uint8_t *p;
		draw_image(sys, start, 0, get_image(sys, id), 0, 0xff, -1);
		if (end > SCREEN_W) end = SCREEN_W;
		if (h > screen_h) h = screen_h;
		if (start >= end) return;
		end -= start;
		p = sys->screen + start;
		for (y = 0; y < h; y++, p += SCREEN_W)
			memset(p, *p, end);
	} else if (h == 1) {
		uint8_t *s, *p;
		draw_image(sys, 0, start, get_image(sys, id), 0, 0xff, -1);
		if (end > screen_h) end = screen_h;
		if (w > SCREEN_W) w = SCREEN_W;
		if (start >= end) return;
		end -= start;
		p = s = sys->screen + start * SCREEN_W;
		while (--end) memcpy(p += SCREEN_W, s, w);
	} else ERR_EXIT("unknown repeat mode");
}

static void bios_10(sysctx_t *sys, cpu_state_t *s) {
	int x1 = s->mem[0x100];
	int y1 = s->mem[0x101];
	int id1 = READ16(s->mem + 0x102);
	int x2 = s->mem[0x105];
	int y2 = s->mem[0x106];
	int id2 = READ16(s->mem + 0x107);
	int w1, h1, w2, h2, cmp;
	unsigned res_offs;
	TRACE("check_intersect ("
			"x1 = %u, y1 = %u, id1 = %u, flip = %u, "
			"x2 = %u, y2 = %u, id2 = %u, flip = %u)",
			x1, y1, id1, s->mem[0x104],
			x2, y2, id2, s->mem[0x109]);
	res_offs = get_image(sys, id1);
	w1 = sys->rom[res_offs];
	h1 = sys->rom[res_offs + 2];
	res_offs = get_image(sys, id2);
	w2 = sys->rom[res_offs];
	h2 = sys->rom[res_offs + 2];
	cmp = 0;
	if (((x2 - x1) & 0xff) < w1) cmp |= 1;
	if (((x1 - x2) & 0xff) < w2) cmp |= 1 + 4;
	if (((y2 - y1) & 0xff) < h1) cmp |= 2;
	if (((y1 - y2) & 0xff) < h2) cmp |= 2 + 8;
	if ((cmp & 3) != 3) {
		TRACE(" a = 0");
		s->a = 0; return;
	}
	TRACE(" a = 1");
	s->a = 1;
}

static void bios_14(sysctx_t *sys, cpu_state_t *s) {
	unsigned addr = READ24(s->mem + 0x80);
	TRACE("bios_14 (addr = 0x%x, %u)", addr, s->mem[0x85]);
	if (sys->rom_size < addr + 4)
		ERR_EXIT("read outside the ROM (0x%x)\n", addr);
	TRACE(" 0x%02x, id = %u, 0x%02x", sys->rom[addr],
			READ16(&sys->rom[addr + 1]), sys->rom[addr + 3]);
}

// 0x2c471
static void bios_16(sysctx_t *sys, cpu_state_t *s) {
	unsigned addr = READ24(s->mem + 0x80);
	TRACE("bios_16 (addr = 0x%x, %u)", addr, s->mem[0x85]);
	if (sys->rom_size < addr + 4)
		ERR_EXIT("read outside the ROM (0x%x)\n", addr);
	TRACE(" 0x%02x, id = %u, 0x%02x", sys->rom[addr],
			READ16(&sys->rom[addr + 1]), sys->rom[addr + 3]);
}

// 0x2c537
static void bios_18(sysctx_t *sys, cpu_state_t *s) {
	unsigned addr = READ24(s->mem + 0x80);
	TRACE("bios_18 (addr = 0x%x, %u)", addr, s->mem[0x85]);
	if (sys->rom_size < addr + 4)
		ERR_EXIT("read outside the ROM (0x%x)\n", addr);
	TRACE(" 0x%02x, id = %u, 0x%02x", sys->rom[addr],
			READ16(&sys->rom[addr + 1]), sys->rom[addr + 3]);
}

// 0x2714f
static void bios_1a(sysctx_t *sys, cpu_state_t *s) {
	unsigned addr = READ24(s->mem + 0x80);
	TRACE("bios_1a (addr = 0x%x, %u)", addr, s->mem[0x85]);
	if (sys->rom_size < addr + 4)
		ERR_EXIT("read outside the ROM (0x%x)\n", addr);
	TRACE(" 0x%02x, id = %u, 0x%02x", sys->rom[addr],
			READ16(&sys->rom[addr + 1]), sys->rom[addr + 3]);
}

static void bios_1c(sysctx_t *sys, cpu_state_t *s) {
	int id = READ24(s->mem + 0x80);
	TRACE("bios_1c (res = %u)", id);
}

static void bios_1e(sysctx_t *sys, cpu_state_t *s) {
	TRACE("bios_1e");
}

static void bios_24(sysctx_t *sys, cpu_state_t *s) {
	int x = s->mem[0x100];
	int y = s->mem[0x101];
	int id = s->mem[0x102];
	TRACE("draw_char_alpha (x = %u, y = %u, id = %u, color = 0x%02x)", x, y, id, s->mem[0x103]);
	draw_char(sys, x, y, id, s->mem[0x103], -1);
}

static void bios_26(sysctx_t *sys, cpu_state_t *s) {
	int x = s->mem[0x100];
	int y = s->mem[0x101];
	int id = s->mem[0x102];
	TRACE("draw_char (x = %u, y = %u, id = %u, color = 0x%02x, bg = 0x%02x)",
			x, y, id, s->mem[0x103], s->mem[0x104]);
	draw_char(sys, x, y, id, s->mem[0x103], s->mem[0x104]);
}

static void bios_2c(sysctx_t *sys, cpu_state_t *s) {
	unsigned addr = READ24(s->mem + 0x80);
	TRACE("bios_2c (addr = 0x%x, %u)", addr, s->mem[0x85]);
	if (sys->rom_size < addr + 4)
		ERR_EXIT("read outside the ROM (0x%x)\n", addr);
	TRACE(" 0x%02x, id = %u, 0x%02x", sys->rom[addr],
			READ16(&sys->rom[addr + 1]), sys->rom[addr + 3]);
}

enum {
	FLASH_OFF = 0,
	FLASH_READY,
	FLASH_CMD,
	FLASH_CMD2
};

#if FLASH_TRACE
#undef FLASH_TRACE
#define FLASH_TRACE(...) printf(__VA_ARGS__)
#else
#undef FLASH_TRACE
#define FLASH_TRACE(...) (void)0
#endif

static void flash_emu(sysctx_t *sys, cpu_state_t *s) {
	unsigned data = s->mem[0x02];
	flash_t *f = &sys->flash;
	unsigned i = f->narg;

	if (f->state == FLASH_OFF) return;
	if (data & 8) { f->state = FLASH_OFF; return; }
	if (f->state == FLASH_READY) {
		if (data == 0) f->state = FLASH_CMD, f->narg = 1 * 16;
		return;
	}

	if (i) {
		if (((data & ~4) ^ (i & 1)) != 2)
			ERR_EXIT("unexpected flash data\n");
		f->narg = --i;
		if (i & 1)
			f->args[i >> 4] = f->args[i >> 4] << 1 | data >> 2;
		else if ((data >> 2 ^ f->args[i >> 4]) & 1)
			ERR_EXIT("wrong bit repeated\n");
		if (i) return;
	}

	if (f->state == FLASH_CMD) {
		f->cmd = f->args[0];
		FLASH_TRACE("flash_cmd 0x%02x\n", f->cmd);
		switch (f->cmd) {
		case 0x50: /* Volatile SR Write Enable */
			f->state = FLASH_OFF;
			break;
		case 0x06: /* Write Enable */
		case 0x04: /* Write Disable */
			f->flags = (f->flags & ~2) | (f->cmd & 2);
			f->state = FLASH_OFF;
			break;

		case 0x05: /* Read Status Register */
		case 0x01: /* Write Status Register */
			f->state = FLASH_CMD2; f->narg = 1 * 16;
			break;
		case 0x02: /* Page Program */
		case 0x20: /* Sector Erase */
			f->state = FLASH_CMD2; f->narg = 3 * 16;
			f->addr = ~0;
			break;
		default:
			ERR_EXIT("unknown flash cmd 0x%02x\n", f->cmd);
		}
	} else {
		unsigned addr;
		switch (f->cmd) {
		case 0x20: /* Sector Erase */
			addr = READ24(f->args);
			FLASH_TRACE("Sector Erase 0x%06x\n", addr);
			if (addr & 0xfff)
				ERR_EXIT("unaligned sector address 0x%06x\n", addr);
			if (addr < sys->save_offs || addr >= sys->rom_size)
				ERR_EXIT("unexpected erase address 0x%06x\n", addr);
			if (!(f->flags & 2)) { f->state = FLASH_OFF; break; }
			memset(sys->rom + addr, 0xff ^ sys->rom_key, 0x1000);
			f->state = FLASH_OFF;
			break;
		case 0x02: /* Page Program */
			addr = f->addr;
			if (addr == ~0u) {
				f->addr = addr = READ24(f->args);
				FLASH_TRACE("Page Program 0x%06x\n", addr);
				if (addr & 0xff)
					ERR_EXIT("unaligned page address 0x%06x\n", addr);
				if (addr < sys->save_offs || addr >= sys->rom_size)
					ERR_EXIT("unexpected program address 0x%06x\n", addr);
				if (!(f->flags & 2)) { f->state = FLASH_OFF; break; }
				f->narg = 1 * 16;
			} else {
				sys->rom[addr] = f->args[0] ^ sys->rom_key;
				f->addr = ++addr;
				if (addr & 0xff) f->narg = 1 * 16;
				else f->state = FLASH_OFF;
			}
			break;
		default:
			f->state = FLASH_OFF;
			break;
		}
	}
}

static void game_event(sysctx_t *sys);

void run_emu(sysctx_t *sys, cpu_state_t *s) {
	unsigned pc = s->pc, t = s->flags;
	uint8_t zflag; int8_t nflag, vflag; uint16_t cflag;
	UNPACK_FLAGS
	unsigned depth = sys->frame_depth, frame_size = 0;
	frame_t *frames = sys->frame_stack;
	unsigned input_timer = 0;
#if TICK_LIMIT
	unsigned tickcount = 0;
#endif

	if (depth)
		frame_size = frames[depth - 1].size;

#define NEXT s->mem[pc++ & 0xffff]

	for (;;) {
		unsigned m, op;
		int o = -1; uint8_t *p = NULL;

#if TICK_LIMIT
		if (++tickcount > TICK_LIMIT)
			ERR_EXIT("instruction limit reached\n");
#endif

		pc &= 0xffff;
#if CPU_TRACE
		if (pc >= 0x300 && (pc - 0x300) < frame_size) {
			int addr = pc - 0x300 + 0x10000 + frames[depth - 1].addr;
			TRACE("%05x: ", addr);
		} else {
			TRACE("%04x: ", pc);
		}
#endif
#define SYS_RET 0x7000
#define SYS_RET1 0x7001
		if (pc >= 0x6000) {
			if (pc == 0x6000) {
				switch (s->x) {
				case 0x06: bios_06(sys, s); break;
				case 0x08: bios_08(sys, s); break;
				case 0x0a: bios_0a(sys, s); break;
				case 0x0c: bios_0c(sys, s); break;
				case 0x0e: bios_0e(sys, s); break;
				case 0x10: bios_10(sys, s); break;
				case 0x14: bios_14(sys, s); break;
				case 0x16: bios_16(sys, s); break;
				case 0x18: bios_18(sys, s); break;
				case 0x1a: bios_1a(sys, s); break;
				case 0x1c: bios_1c(sys, s); break;
				case 0x1e: bios_1e(sys, s); break;
				case 0x24: bios_24(sys, s); break;
				case 0x26: bios_26(sys, s); break;
				case 0x2c: bios_2c(sys, s); break;
				default:
					ERR_EXIT("unknown syscall\n"); goto end;
				}
			} else if (pc == 0x6003) {
				unsigned addr = READ24(s->mem + 0x80), i, n;
				TRACE("ROM read (0x%x)\n", addr);
				if (sys->rom_size <= addr)
					ERR_EXIT("read outside the ROM (0x%x)\n", addr);
				n = sys->rom_size - addr;
				for (i = 0; i < 6; i++)
					s->mem[0x8d + i] = i < n ?
							sys->rom[addr + i] : ~sys->rom_key;
			} else if (pc == SYS_RET) {
				unsigned addr;
				if (!depth) ERR_EXIT("call stack underflow\n");
				depth--;
				if (!depth) { TRACE("last call\n"); goto end; }
				addr = frames[depth - 1].addr;
				frame_size = frames[depth - 1].size;
				memcpy(s->mem + 0x300, sys->rom + addr, frame_size);
			} else if (pc == 0x60de || pc == 0x6052) {
				int tail_call = pc == 0x6052;
				unsigned addr = READ24(s->mem + 0x80);
				frame_size = READ16(s->mem + 0x83) << 1;
				TRACE("ROM call (0x%x, 0x%x)\n", addr, frame_size);
				if (frame_size >= 0x500)
					ERR_EXIT("too big rom call (0x%x, 0x%x)\n", addr, frame_size);
				if (sys->rom_size < addr + frame_size)
					ERR_EXIT("bad ROM call (0x%x, 0x%x)\n", addr, frame_size);
				if (depth >= FRAME_STACK_MAX)
					ERR_EXIT("call stack overflow\n");
				if (tail_call) {
					if (!depth) ERR_EXIT("call stack underflow\n");
					depth--;
				}
				frames[depth].addr = addr;
				frames[depth].size = frame_size;
				depth++;

				if (!tail_call) {
					pc = SYS_RET - 1;
					o = s->sp; s->sp = o - 2;
					s->mem[0x100 + o] = pc >> 8;
					s->mem[0x100 + ((o - 1) & 0xff)] = pc;
				}

				memcpy(s->mem + 0x300, sys->rom + addr, frame_size);
				pc = 0x300;
				continue;
			} else {
				ERR_EXIT("unexpected pc 0x%04x\n", pc);
				goto end;
			}
			s->mem[pc = SYS_RET1] = 0x60;
		}
		op = s->mem[pc++];
		m = op_mod[op];
		t = m & 0x7f;
		if (t >= MOD_LAST) __builtin_unreachable();
		switch (t) {
		case MOD_NUL: /* none */ break;
		case MOD_IMM: /* # */ p = &NEXT; break;
		case MOD_ACC: /* A */ p = &s->a; break;
		case MOD_X: /* X */ p = &s->x; break;
		case MOD_Y: /* Y */ p = &s->y; break;
		case MOD_Z: /* zp */
			o = NEXT; p = s->mem + o; break;
		case MOD_ZX: /* zp,x */
			o = NEXT + s->x; p = &s->mem[o &= 0xff]; break;
		case MOD_ZY: /* zp,y */
			o = NEXT + s->y; p = &s->mem[o &= 0xff]; break;
		case MOD_ZI: /* (zp) */
			o = NEXT; o = s->mem[o] | s->mem[(o + 1) & 0xff] << 8;
			p = s->mem + o; break;
		case MOD_ZXI: /* (zp,x) */
			o = NEXT + s->x;
			o = s->mem[o & 0xff] | s->mem[(o + 1) & 0xff] << 8;
			p = s->mem + o; break;
		case MOD_ZIY: /* (zp),y */
			o = NEXT; o = s->mem[o] | s->mem[(o + 1) & 0xff] << 8;
			o = (o + s->y) & 0xffff; p = s->mem + o; break;
		case MOD_A: /* a */
			o = NEXT; o |= NEXT << 8; p = s->mem + o; break;
		case MOD_AX: /* a,x */
			o = NEXT; o |= NEXT << 8;
			o = (o + s->x) & 0xffff; p = s->mem + o; break;
		case MOD_AY: /* a,y */
			o = NEXT; o |= NEXT << 8;
			o = (o + s->y) & 0xffff; p = s->mem + o; break;
		case MOD_R: /* r */
			t = *(int8_t*)&NEXT;
			break;
		}

#if 0 // CPU memory map
		// ports (128) + RAM (2048)
		if (o >= 0x880) {
			// 0x8000: LCD cmd, 0xc000: LCD data
			if (o >= 0x8000) p = &dummy;
			// chip ROM (8192)
			else if (o >= 0x4000) p = s->mem + (o | 0x6000);
			// maps to RAM
			else p = s->mem + 128 + ((o - 128) & 0x7ff);
		}
#endif

		if (o >= 0 && !(m & 0x80)) {
			TRACE("R[0x%02x] ", o);
			// reads memory
			switch (o) {
			case 0x00:
				if (++input_timer >= 16) {
					input_timer = 0;
					game_event(sys);
				}
				*p = ~sys->keys;
				break;
			case 0x02: *p &= ~2; break; // 0x13271
			//case 0x01: *p |= 1 << 1; break; // charging
			case 0x14: *p |= 1 << 6; break; // 0x129ff
			case 0x7b: *p |= 1 << 3; break; // 0x106e5
			case 0x93: *p |= 1 << 7; break; // 0x100a6
			}
		}

		switch (op) {
#define BRANCH(cond) if (cond) pc += t; break;
		case 0x0f: case 0x1f: case 0x2f: case 0x3f: /* BBRn */
		case 0x4f: case 0x5f: case 0x6f: case 0x7f:
		case 0x8f: case 0x9f: case 0xaf: case 0xbf: /* BBSn */
		case 0xcf: case 0xdf: case 0xef: case 0xff: {
			unsigned bit = *p >> (op >> 4 & 7) & 1;
			t = *(int8_t*)&NEXT;
			p = NULL; BRANCH(bit == op >> 7)
		}
		case 0x10: /* BPL */ BRANCH(nflag >= 0)
		case 0x30: /* BMI */ BRANCH(nflag < 0)
		case 0x50: /* BVC */ BRANCH(vflag >= 0)
		case 0x70: /* BVS */ BRANCH(vflag < 0)
		case 0x80: /* BRA */ BRANCH(1)
		case 0x90: /* BCC */ BRANCH(cflag < 0x100)
		case 0xb0: /* BCS */ BRANCH(cflag >= 0x100)
		case 0xd0: /* BNE */ BRANCH(zflag)
		case 0xf0: /* BEQ */ BRANCH(!zflag)
#undef BRANCH

		case 0x07: case 0x17: case 0x27: case 0x37: /* RMBn */
		case 0x47: case 0x57: case 0x67: case 0x77:
			t = *p & ~(1 << (op >> 4 & 7)); break;
		case 0x87: case 0x97: case 0xa7: case 0xb7: /* SMBn */
		case 0xc7: case 0xd7: case 0xe7: case 0xf7:
			t = *p | 1 << (op >> 4 & 7); break;

		case 0x18: /* CLC */ cflag = 0; break;
		case 0x38: /* SEC */ cflag = 0x100; break;
		case 0x58: /* CLI */ s->flags &= ~MASK_I; break;
		case 0x78: /* SEI */ s->flags |= MASK_I; break;
		case 0xb8: /* CLV */ vflag = 0; break;
		case 0xd8: /* CLD */ s->flags &= ~MASK_D; break;
		case 0xf8: /* SED */ s->flags |= MASK_D; break;

#define ARITH4(i) \
	case i + 0x00: /* zp */ \
	case i + 0x08: /* a */ \
	case i + 0x10: /* zp,x */ \
	case i + 0x18: /* a,x */

#define ARITH8(i) \
	ARITH4(i + 5) \
	case i + 0x01: /* (zp,x) */ \
	case i + 0x11: /* (zp),y */ \
	case i + 0x12: /* (zp) */ \
	case i + 0x19: /* a,y */ \

		ARITH4(0x06)
		case 0x0a: /* ASL A */
			t = *p << 1;
			zflag = t; nflag = t; cflag = t;
			break;

		ARITH4(0x24)
		case 0x89: /* BIT # */
			t = *p;
			zflag = t & s->a;
			nflag = t; vflag = t << 1;
			p = NULL; break;

		ARITH4(0x26)
		case 0x2a: /* ROL A */
			t = *p << 1 | cflag >> 8;
			zflag = t; nflag = t; cflag = t;
			break;

		ARITH4(0x46)
		case 0x4a: /* LSR A */
			t = *p; cflag = (t & 1) << 8;
			t >>= 1; zflag = t; nflag = t;
			break;

		ARITH4(0x66)
		case 0x6a: /* ROR A */
			t = *p; t |= cflag & 0x100;
			cflag = (t & 1) << 8;
			t >>= 1; zflag = t; nflag = t;
			break;

		ARITH4(0xa4)
		case 0xa0: /* LDY # */
			s->y = t = *p; zflag = t; nflag = t;
			p = NULL; TRACE("Y = 0x%02x", t); break;

		ARITH4(0xa6)
		case 0xa2: /* LDX # */
			s->x = t = *p; zflag = t; nflag = t;
			p = NULL; TRACE("X = 0x%02x", t); break;

		ARITH4(0xc6)
		case 0x3a: /* DEC A */
		case 0x88: /* DEY */
		case 0xca: /* DEX */
			t = *p - 1; zflag = t; nflag = t; break;

		ARITH4(0xe6)
		case 0x1a: /* INC A */
		case 0xc8: /* INY */
		case 0xe8: /* INX */
			t = *p + 1; zflag = t; nflag = t; break;

		ARITH8(0x00)
		case 0x09: /* ORA # */
			s->a = t = s->a | *p; zflag = t; nflag = t;
			p = NULL; TRACE("A = 0x%02x", t);  break;

		ARITH8(0x20)
		case 0x29: /* AND # */
			s->a = t = s->a & *p; zflag = t; nflag = t;
			p = NULL; TRACE("A = 0x%02x", t); break;

		ARITH8(0x40)
		case 0x49: /* EOR # */
			s->a = t = s->a ^ *p; zflag = t; nflag = t;
			p = NULL; TRACE("A = 0x%02x", t); break;

		ARITH8(0x60)
		case 0x69: /* ADC # */
			t = *p;
		op_add2:
			{
				unsigned a = s->a, d = a ^ t;
				if (s->flags & MASK_D) {
					int b = (a & 15) + (t & 15) + (cflag >> 8);
					if (op <= 0x7f) {
						if (b >= 10) b += 6;
					} else {
						if (b < 16) b -= 6;
					}
					b = (a & 0xf0) + (t & 0xf0) + (b >= 16 ? 16 : 0) + (b & 15);
					vflag = (b ^ a) & ~d;
					if (op <= 0x7f) {
						if (b >= 0xa0) b += 0x60;
						cflag = b;
					} else {
						cflag = b;
						if (b < 0x100) b -= 0x60;
					}
					t = b;
				} else {
					t += a + (cflag >> 8);
					vflag = (t ^ a) & ~d;
					cflag = t;
				}
				s->a = t;
			}
			zflag = t; nflag = t;
			p = NULL; TRACE("A = 0x%02x", t & 0xff); break;

		case 0x64: case 0x74: case 0x9c: case 0x9e: /* STZ */
			t = 0; break;
		case 0x84: case 0x8c: case 0x94: /* STY */
			t = s->y; break;
		case 0x86: case 0x8e: case 0x96: /* STX */
			t = s->x; break;
		ARITH8(0x80) /* STA */ t = s->a; break;

		ARITH8(0xa0)
		case 0xa9: /* LDA # */
			s->a = t = *p; zflag = t; nflag = t;
			p = NULL; TRACE("A = 0x%02x", t); break;

		ARITH8(0xe0)
		case 0xe9: /* SBC # */
			t = *p ^ 0xff;
			goto op_add2;

		case 0xc0: case 0xc4: case 0xcc: /* CPY */
			t = s->y; goto op_cmp;
		case 0xe0: case 0xe4: case 0xec: /* CPX */
			t = s->x; goto op_cmp;
		ARITH8(0xc0)
		case 0xc9: /* CMP # */
			t = s->a;
		op_cmp:
			t -= *p;
			cflag = t + 0x100;
			zflag = t; nflag = t;
			p = NULL; break;

#undef ARITH8
#undef ARITH4

		case 0x4c: /* JMP a */
			t = *p | NEXT << 8; pc = t; p = NULL; break;

		case 0x6c: /* JMP (a) */
		case 0x7c: /* JMP (a,x) */
			pc = *p | s->mem[(o + 1) & 0xffff] << 8;
			p = NULL; break;

		case 0x04: case 0x0c: /* TSB Test and Set Bits */ {
			unsigned a = s->a;
			t = *p; zflag = t & a; t |= a; break;
		}

		case 0x14: case 0x1c: /* TRB Test and Reset Bits */ {
			unsigned a = s->a;
			t = *p; zflag = t & a; t &= ~a; break;
		}

		case 0x8a: /* TXA */
			s->a = t = s->x; TRACE("A = 0x%02x", t);
			zflag = t; nflag = t; break;
		case 0x98: /* TYA */
			s->a = t = s->y; TRACE("A = 0x%02x", t);
			zflag = t; nflag = t; break;
		case 0x9a: /* TXS */ 
			s->sp = s->x; TRACE("S = 0x%02x", s->x); break;
		case 0xa8: /* TAY */
			s->y = t = s->a; TRACE("Y = 0x%02x", t);
			zflag = t; nflag = t; break;
		case 0xaa: /* TAX */
			s->x = t = s->a; TRACE("X = 0x%02x", t);
			zflag = t; nflag = t; break;
		case 0xba: /* TSX */
			s->x = t = s->sp; TRACE("X = 0x%02x", t);
			zflag = t; nflag = t; break;

		case 0x08: /* PHP */
			PACK_FLAGS goto op_push;
		case 0x48: /* PHA */
		case 0x5a: /* PHY */
		case 0xda: /* PHX */
			t = *p; p = NULL;
		op_push:
			o = s->sp; s->sp = o - 1;
			s->mem[0x100 + o] = t;
			TRACE("[0x1%02x] = 0x%02x, S = %02x", o, t, o - 1);
			break;

		case 0x28: /* PLP */
			s->sp = o = s->sp + 1;
			s->flags = t = s->mem[0x100 + (o & 0xff)];
			UNPACK_FLAGS break;

		case 0x68: /* PLA */
		case 0x7a: /* PLY */
		case 0xfa: /* PLX */
			s->sp = o = s->sp + 1;
			t = s->mem[0x100 + (o & 0xff)];
#if CPU_TRACE
			TRACE("S = %02x, ", o); o = -1;
#else
			*p = t; p = NULL;
#endif
			break;

		case 0x20: /* JSR */
			o = s->sp; s->sp = o - 2;
			s->mem[0x100 + o] = pc >> 8;
			s->mem[0x100 + ((o - 1) & 0xff)] = pc;
			t = *p | NEXT << 8; pc = t; p = NULL; break;

		case 0x40: /* RTI */
			o = s->sp; s->sp = o + 3;
			s->flags = t = s->mem[0x100 + ((o + 1) & 0xff)];
			UNPACK_FLAGS
			pc = s->mem[0x100 + ((o + 2) & 0xff)];
			pc |= s->mem[0x100 + ((o + 3) & 0xff)] << 8;
			break;

		case 0x60: /* RTS */
			o = s->sp; s->sp = o + 2;
			pc = s->mem[0x100 + ((o + 1) & 0xff)];
			pc |= s->mem[0x100 + ((o + 2) & 0xff)] << 8;
			pc++;
			break;

		case 0xea: /* NOP */
			break;

		case 0xcb: /* WAI */
			sys->keys |= 1 << 19;
			goto end;

		/* undefined */
		case 0x02: case 0x03: case 0x0b:
		case 0x13: case 0x1b:
		case 0x22: case 0x23: case 0x2b:
		case 0x33: case 0x3b:
		case 0x42: case 0x43: case 0x44: case 0x4b:
		case 0x53: case 0x54: case 0x5b: case 0x5c:
		case 0x62: case 0x63: case 0x6b:
		case 0x73: case 0x7b:
		case 0x82: case 0x83: case 0x8b:
		case 0x93: case 0x9b:
		case 0xa3: case 0xab:
		case 0xb3: case 0xbb:
		case 0xc2: case 0xc3:
		case 0xd3: case 0xd4: case 0xdc:
		case 0xe2: case 0xe3: case 0xeb:
		case 0xf3: case 0xf4: case 0xfb: case 0xfc:
		case 0x00: /* BRK */
		case 0xdb: /* STP */
			fprintf(stderr, "unexpected opcode 0x%02x\n", op);
			goto end;

		default:
			fprintf(stderr, "unknown opcode 0x%02x\n", op);
			goto end;
		}
		if (p) {
			*p = t;
#if CPU_TRACE
			if (o >= 0) TRACE("[0x%02x] = 0x%02x", o, t & 0xff);
			else if (p == &s->a) TRACE("A = 0x%02x", t & 0xff);
			else if (p == &s->x) TRACE("X = 0x%02x", t & 0xff);
			else if (p == &s->y) TRACE("Y = 0x%02x", t & 0xff);
#endif
			if (o == 0x02) flash_emu(sys, s);
			else if (o == 0x12) {
				sys->flash.state = t ? FLASH_OFF : FLASH_READY;
			} else if (o == 0x00) {
				/* power off */
				if (!t) {
					TRACE(", power off");
					sys->keys |= 1 << 18 | 1 << 20; break;
				}
			} else if (o == 0x8000) { /* lcd_cmd */
				if (t == 0x28) { /* Display OFF */
					TRACE(", display off");
					sys->keys |= 1 << 20;
				}
			}
		}
#if CPU_TRACE
		TRACE("\n");
#endif
	}
end:
	PACK_FLAGS
	s->flags = t;
	s->pc = pc;
	sys->frame_depth = depth;
}

static uint8_t* loadfile(const char *fn, size_t *num, size_t nmax) {
	size_t n, j = 0; uint8_t *buf = 0;
	FILE *fi = fopen(fn, "rb");
	if (fi) {
		fseek(fi, 0, SEEK_END);
		n = ftell(fi);
		if (n <= nmax) {
			fseek(fi, 0, SEEK_SET);
			if (n) {
				buf = (uint8_t*)malloc(n);
				if (buf) j = fread(buf, 1, n, fi);
			}
		}
		fclose(fi);
	}
	if (num) *num = j;
	return buf;
}

static void game_event(sysctx_t *sys) {
	for (;;) {
		int ev, key, key2;
		ev = window_event(&sys->window, &key);
		if (ev == EVENT_EMPTY) break;
		switch (ev) {
		case EVENT_KEY_PRESS:
		case EVENT_KEY_RELEASE:
#if 0
			printf("key = %d\n", key); fflush(stdout);
#endif
			if (key == SYSKEY_ESCAPE && ev == EVENT_KEY_PRESS) {
				sys->keys |= 1 << 16; return;
			}
			key2 = -1;
			switch (key) {
			/* left (select) */
			case SYSKEY_LEFT:
			case SYSKEY_A + 'a':
				key2 = sys->keymap[0]; break;
			/* middle (enter) */
			case SYSKEY_DOWN:
			case SYSKEY_A + 's':
				key2 = sys->keymap[1]; break;
			/* right (back/menu) */
			case SYSKEY_RIGHT:
			case SYSKEY_A + 'd':
				key2 = sys->keymap[2]; break;
			/* left side button */
			case SYSKEY_DELETE:
			case SYSKEY_A + 'q':
				key2 = sys->keymap[3]; break;
			/* right side button */
			case SYSKEY_PAGEDOWN:
			case SYSKEY_A + 'e':
				key2 = sys->keymap[4]; break;
			/* reset */
			case SYSKEY_A + 'r':
				key2 = 17; break;
			}
			if (key2 >= 0) {
				key2 = 1 << key2;
				key = ev == EVENT_KEY_PRESS ? key2 : 0;
				sys->keys = (sys->keys & ~key2) | key;
			}
			break;

		case EVENT_EMPTY:
		case EVENT_QUIT:
			sys->keys |= 1 << 16;
			return;
		}
	}
}

#include <time.h>

static void update_time(cpu_state_t *s) {
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	s->mem[0x1df] = tm->tm_year % 100;
	s->mem[0x1e0] = tm->tm_mon;
	s->mem[0x1e1] = tm->tm_mday - 1;
	s->mem[0x1e2] = tm->tm_hour;
	s->mem[0x1e3] = tm->tm_min;
	s->mem[0x1e4] = tm->tm_sec * 2;
}

void run_game(sysctx_t *sys, cpu_state_t *s) {
	unsigned disp_time, frames, fps = 30;
	unsigned last_time, timer_rem;
reset:
	frames = 0;
	if (!sys->init_done) {
		sys->init_done = 1;
		s->mem[0xa3] |= 1; // to play start animation
#if 0 // Opens at boot when the left and right buttons are pressed.
		s->mem[0x93] |= 1; // to open test menu
#endif
		s->mem[0x99] = sys->rom_key;

		sys->frame_depth = 0;
		s->sp = 0x7f; // guess
		s->pc = 0x60de;
		WRITE24(s->mem + 0x80, READ16(sys->rom + 3));
		WRITE16(s->mem + 0x83, READ16(sys->rom + 3 + 2));
		run_emu(sys, s);
	}

	last_time = sys_time_ms(sys);
	timer_rem = 0;

#ifndef START_DELAY
// to be able to open the test menu
#define START_DELAY 500
#endif

	sys_update(sys);
#if START_DELAY
	sys_sleep(START_DELAY);
	game_event(sys);
#endif

	disp_time = sys_time_ms(sys);
	while (!(sys->keys & 3 << 16)) {
		int ev;
		unsigned a, cur_time;

		// decrease idle timer
		a = READ16(s->mem + 0x181);
		//if (a) WRITE16(s->mem + 0x181, a < 30 ? 0 : a - 30);
		if (a) WRITE16(s->mem + 0x181, a - 1);

		a = sys_time_ms(sys) - last_time;
		a = a * 256 / 1000;
		last_time += (a >> 8) * 1000;
		s->mem[0xaf] += a - timer_rem;
		timer_rem = a;

		if (sys->keys & 1 << 19) { /* WAI */
			sys->keys &= ~(1 << 19);
		} else {
			s->mem[0x93] |= 1 << 4; // OK-560 compat: enable timers
			sys->frame_depth = 0;
			s->sp = 0x7f; // guess
			s->pc = 0x60de;
			WRITE24(s->mem + 0x80, READ16(sys->rom + 0x1b));
			WRITE16(s->mem + 0x83, READ16(sys->rom + 0x1b + 2));
		}
		run_emu(sys, s);
		if (sys->keys & 1 << 20) { // clean screen
			sys->keys &= ~(1 << 20);
			memset(sys->screen, 0, sizeof(sys->screen));
		}

		sys_update(sys);
#if 0
		sys_sleep(1000 / fps);
#else
		cur_time = sys_time_ms(sys);
		if (++frames >= fps)
			disp_time += 1000, frames = 0;
		a = frames * 1000 / fps + disp_time - cur_time;
		if ((int)a < 0) disp_time = cur_time, frames = 0;
		else sys_sleep(a);
#endif

		game_event(sys);
	}
	if (!(sys->keys & 1 << 16)) {
		sys->keys &= 0xff;
		sys->init_done = 0;
		memset(s, 0, sizeof(*s));
		goto reset;
	}
}

static void check_rom(sysctx_t *sys) {
	unsigned rom_size = sys->rom_size;
	unsigned res_offs;
	unsigned i, key, moffs = 0x23;
	const char *magic = "tony";
	if (rom_size < 0x10000)
		ERR_EXIT("ROM is too small\n");
	sys->rom_key = key = sys->rom[moffs] ^ magic[0];
	for (i = 1; i < 4; i++) {
		if ((sys->rom[moffs + i] ^ key) != (unsigned)magic[i])
			ERR_EXIT("ROM magic doesn't match\n");
	}
	if (key)
	for (i = 0; i < rom_size; i++) sys->rom[i] ^= key;
	res_offs = READ24(sys->rom);
	if (rom_size < res_offs)
		ERR_EXIT("bad resources offset\n");
}

static void xor_save(sysctx_t *sys) {
	unsigned i, key = sys->rom_key;
	if (key)
	for (i = sys->save_offs; i < sys->rom_size; i++)
		sys->rom[i] ^= key;
}

int main(int argc, char **argv) {
	const char *rom_fn = "toumapet.bin";
	const char *save_fn = NULL;
#if CPU_TRACE
	const char *log_fn = NULL;
	int log_size = 4 << 20;
#endif
	uint8_t *rom; size_t rom_size;
	cpu_state_t cpu;
	sysctx_t sys;
	int zoom = 3, upd_time = 0;

	while (argc > 1) {
		if (!strcmp(argv[1], "--save")) {
			if (argc <= 2) ERR_EXIT("bad option\n");
			save_fn = argv[2];
			if (!*save_fn) save_fn = NULL;
			argc -= 2; argv += 2;
		} else if (!strcmp(argv[1], "--rom")) {
			if (argc <= 2) ERR_EXIT("bad option\n");
			rom_fn = argv[2];
			argc -= 2; argv += 2;
#if CPU_TRACE
		} else if (!strcmp(argv[1], "--log")) {
			if (argc <= 2) ERR_EXIT("bad option\n");
			log_fn = argv[2];
			if (!*log_fn) log_fn = NULL;
			argc -= 2; argv += 2;
		} else if (!strcmp(argv[1], "--log-size")) {
			if (argc <= 2) ERR_EXIT("bad option\n");
			log_size = atoi(argv[2]);
			argc -= 2; argv += 2;
#endif
		} else if (!strcmp(argv[1], "--zoom")) {
			if (argc <= 2) ERR_EXIT("bad option\n");
			zoom = atoi(argv[2]);
			if (zoom < 1) zoom = 1;
			if (zoom > 5) zoom = 5;
			argc -= 2; argv += 2;
		} else if (!strcmp(argv[1], "--update-time")) {
			upd_time = 1;
			argc -= 1; argv += 1;
		} else ERR_EXIT("unknown option\n");
	}

	memset(&cpu, 0, sizeof(cpu));
	memset(&sys, 0, sizeof(sys));

	rom = loadfile(rom_fn, &rom_size, 8 << 20);
	if (!rom) ERR_EXIT("can't load ROM file\n");
	// a rough way to detect a model
	if (rom_size == 4 << 20) {
		sys.model = 550;
		sys.screen_h = 128;
		sys.keymap[0] = 4;
		sys.keymap[1] = 5;
		sys.keymap[2] = 6;
		sys.keymap[3] = 3;
		sys.keymap[4] = 2;
	} else if (rom_size == 8 << 20) {
		sys.model = 560;
		sys.screen_h = 160;
		sys.keymap[0] = 2;
		sys.keymap[1] = 3;
		sys.keymap[2] = 4;
		sys.keymap[3] = 5;
		sys.keymap[4] = 6;
	} else ERR_EXIT("unexpected ROM size\n");

	sys.save_offs = rom_size - 0x10000;
	sys.rom = rom;
	sys.rom_size = rom_size;
	sys.zoom = zoom;
	check_rom(&sys);

#if CPU_TRACE
	if (log_fn) {
		if (log_size < 256) log_size = 256;
		if (log_size > 1 << 30) log_size = 1 << 30;
		sys.log_fn = log_fn;
		sys.log_size = log_size;
		sys.log_overflow = 0;
		sys.log_pos = 0;
		sys.log_buf = malloc(log_size + 256);
		if (!sys.log_buf) ERR_EXIT("malloc failed\n");
		glob_sys = &sys;
	}
#endif

	if (save_fn) {
		unsigned n1, n2, n3;
		FILE *f = fopen(save_fn, "rb");
		if (f) {
			n1 = fread(cpu.mem, 1, sizeof(cpu.mem), f);
			n2 = fread(sys.rom + sys.save_offs, 1, sys.rom_size - sys.save_offs, f);
			n3 = fread(sys.screen, 1, SCREEN_W * sys.screen_h, f);
			(void)n3;
			fclose(f);
			if (n1 != sizeof(cpu.mem)) ERR_EXIT("unexpected save size\n");
			if (n2 != 0x10000) ERR_EXIT("unexpected save size\n");
			sys.init_done = 1;
			xor_save(&sys);
		}
	}

	sys_init(&sys);

	if (0) { // test keys
		for (;;) {
			int ev, key = -1;
			ev = window_event(&sys.window, &key);
			switch (ev) {
			case EVENT_KEY_PRESS:
			case EVENT_KEY_RELEASE:
				printf("%d %d\n", ev, key);
				fflush(stdout);
			}
			if (ev == EVENT_QUIT) break;
			sys_sleep(10);
		}
		sys_close(&sys);
		return 0;
	}

	if (upd_time) update_time(&cpu);

	run_game(&sys, &cpu);

	if (save_fn) {
		FILE *f = fopen(save_fn, "wb");
		if (f) {
			xor_save(&sys);
			fwrite(cpu.mem, 1, sizeof(cpu.mem), f);
			fwrite(sys.rom + sys.save_offs, 1, sys.rom_size - sys.save_offs, f);
			fwrite(sys.screen, 1, SCREEN_W * sys.screen_h, f);
			fclose(f);
		}
	}

	sys_close(&sys);
}

