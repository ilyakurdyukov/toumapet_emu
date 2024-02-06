#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define ERR_EXIT(...) do { \
	fprintf(stderr, __VA_ARGS__); exit(1); \
} while (0)

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

static uint8_t pal[256][3];

#define READ16(p) ((p)[0] | (p)[1] << 8)
#define READ24(p) ((p)[0] | (p)[1] << 8 | (p)[2] << 16)

static int decode_image_1bit(uint8_t *src, size_t size, const char *fn) {
	int w, h, x, y; const char *err_str;
	uint8_t *data = NULL, *d; FILE *f;

#define GOTO_ERR(str) do { \
	err_str = str; goto err; \
} while (0)

	if (size < 2) GOTO_ERR("too small");
	w = src[0]; h = src[1]; src += 2;
	data = malloc(w * h);
	if (!data) GOTO_ERR("malloc failed");
	x = ((w + 7) >> 3) * h + 2;
	if (size < x) GOTO_ERR("too small");

	d = data;
	for (y = 0; y < h; y++, d += w) {
		int a = -1;
		for (x = 0; x < w; x++, a <<= 1) {
			if (a & 1 << 16) a = *src++ | 0x100;
			d[x] = (a >> 7 & 1) + '0';
		}
	}
#undef GOTO_ERR

	f = fopen(fn, "wb");
	if (f) {
		fprintf(f, "P1\n%u %u\n", w, h);
		for (y = 0; y < h; y++, fputs("\n", f))
			fwrite(data + y * w, 1, w, f);
		fclose(f);
	}
	free(data);
	return 0;

err:
	printf("unpack_image failed (%s)\n", err_str);
	if (data) free(data);
	return 1;
}

static int decode_image(uint8_t *src, size_t size, const char *fn) {
	int w, h, x, y; const char *err_str;
	uint8_t *data = NULL, *d; FILE *f;

#define GOTO_ERR(str) do { \
	err_str = str; goto err; \
} while (0)

	if (size < 4) GOTO_ERR("too small");
	size -= 4;
	if (src[1] != 0 || src[3] != 0x80)
		GOTO_ERR("unexpected image header");
	w = src[0]; h = src[2]; src += 4;
	data = malloc(w * h);
	if (!data) GOTO_ERR("malloc failed");

	d = data;
	for (y = 0; y < h; y++, d += w) {
		int len = READ16(src), a = 0, n = 1;
		uint8_t *s = src + 2;
		if (size < len) GOTO_ERR("end of file");
		src += len; size -= len; len -= 4;
		for (x = 0; x < w; x++) {
			if (!--n) {
				if ((len -= 1) < 0) GOTO_ERR("RLE error");
				a = *s++; n = 1;
				if (!a) {
					if ((len -= 2) < 0) GOTO_ERR("RLE error");
					a = *s++; n = *s++;
					if (!n) GOTO_ERR("zero RLE count");
				}
			}
			d[x] = a;
		}
	}
#undef GOTO_ERR

	f = fopen(fn, "wb");
	if (f) {
		fprintf(f, "P6\n%u %u\n255\n", w, h);
		for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			fwrite(pal[data[y * w + x]], 1, 3, f);
		fclose(f);
	}
	free(data);
	return 0;

err:
	printf("unpack_image failed (%s)\n", err_str);
	if (data) free(data);
	return 1;
}

#if 1
// uses unknown 4-bit ADPCM
static uint8_t adpcm_value[256] = {
	0xff, 0xff, 0xff, 0x00, 0x00, 0x02, 0x03, 0x05,
	0xfe, 0xfe, 0xff, 0xfe, 0x00, 0x03, 0x08, 0x0a,
	0xfd, 0xfd, 0xfe, 0xfd, 0xfd, 0xfe, 0xfd, 0x04,
	0xfd, 0xfc, 0xfc, 0xfb, 0xfb, 0xfc, 0xff, 0x07,
	0xfd, 0xfb, 0xfb, 0xfb, 0xfb, 0xfc, 0x00, 0x0a,
	0xfc, 0xfb, 0xfa, 0xfa, 0xfb, 0xfc, 0xff, 0x0b,
	0xfb, 0xfb, 0xfb, 0xfb, 0xfb, 0xfc, 0xff, 0x0c,
	0xfa, 0xfa, 0xfa, 0xfa, 0xfa, 0xfc, 0x01, 0x11,
	0xf9, 0xf9, 0xfa, 0xfa, 0xfa, 0xfc, 0x01, 0x13,
	0xf9, 0xf9, 0xf8, 0xf8, 0xf8, 0xfa, 0xff, 0x11,
	0xf9, 0xf9, 0xf7, 0xf6, 0xf6, 0xf7, 0xfd, 0x17,
	0xf8, 0xf8, 0xf8, 0xf6, 0xf6, 0xf8, 0x00, 0x1e,
	0xf7, 0xf7, 0xf7, 0xf6, 0xf7, 0xf9, 0x06, 0x38,
	0xf6, 0xf6, 0xf6, 0xf5, 0xf6, 0xfb, 0x0a, 0x33,
	0xf6, 0xf7, 0xf6, 0xf5, 0xf6, 0xfa, 0x07, 0x2e,
	0xf6, 0xf7, 0xf6, 0xf5, 0xf5, 0xf8, 0x04, 0x2f,
	0xf5, 0xf6, 0xf6, 0xf6, 0xf5, 0xf8, 0x01, 0x28,
	0xf6, 0xf6, 0xf5, 0xf5, 0xf5, 0xf7, 0x00, 0x21,
	0xf6, 0xf7, 0xf7, 0xf7, 0xf8, 0xfb, 0x04, 0x1c,
	0xf6, 0xf6, 0xf7, 0xf7, 0xf8, 0xfb, 0x02, 0x15,
	0xf6, 0xf7, 0xf8, 0xf8, 0xfa, 0xfd, 0x04, 0x18,
	0xf6, 0xf8, 0xfa, 0xfa, 0xfa, 0xff, 0x05, 0x1e,
	0xf6, 0xf7, 0xfc, 0xfd, 0xff, 0x03, 0x08, 0x19,
	0xf7, 0xfa, 0x00, 0x00, 0x04, 0x07, 0x0a, 0x13,
	0xf8, 0xfd, 0x03, 0x08, 0x0c, 0x0d, 0x13, 0x1c,
	0xf8, 0x00, 0x08, 0x0c, 0x0d, 0x13, 0x1a, 0x1c,
	0xf8, 0x04, 0x0a, 0x10, 0x10, 0x0f, 0x16, 0x17,
	0xfc, 0x04, 0x0f, 0x13, 0x18, 0x19, 0x19, 0x10,
	0xfd, 0x08, 0x12, 0x1f, 0x1f, 0x25, 0x21, 0x0d,
	0xfd, 0x0a, 0x10, 0x1e, 0x23, 0x2a, 0x1b, 0x09,
	0xfe, 0x0a, 0x0e, 0x25, 0x1f, 0x29, 0x25, 0x06,
	0xfe, 0x0d, 0x19, 0x33, 0x55, 0x3e, 0x1e, 0xfe };

static uint8_t adpcm_next[256];

typedef struct { uint8_t idx; } adpcm_status_t;

static void adpcm_init(adpcm_status_t *adpcm) {
	int i;
	adpcm->idx = 0;
	if (adpcm_next[7]) return;
	for (i = 0; i < 256; i++) {
		int a = i >> 3;
#define X(thr) ((a + (32 - thr)) >> 5)
		switch (i & 7) {
		case 0: a -= 1 + X(20) + X(30); break;
		case 1: a -= 1 + X(26) + X(30); break;
		case 2: a -= 1 + X(28); break;
		case 3: a -= X(27) + X(29); break;
		case 7: a += 4 + X(11) + X(12); break;
#undef X
		default: a++;
		}
		a = a < 0 ? 0 : a > 31 ? 31 : a;
		adpcm_next[i] = a * 8;
		adpcm_value[i] += ((i & 7) + 1) * ((i >> 3) + 1);
	}
}

static int adpcm_decode(adpcm_status_t *adpcm, unsigned x) {
	unsigned a = (x & 7) | adpcm->idx;
	adpcm->idx = adpcm_next[a];
	a = adpcm_value[a];
	return (x & 8 ? -a : a) << 6;
}
#else
typedef struct { uint8_t dummy; } adpcm_status_t;
static void adpcm_init(adpcm_status_t *adpcm) {}
// this rough guess sounds close
static int adpcm_decode(adpcm_status_t *adpcm, unsigned x) {
	x = x & 8 ? 7 - x : x;
	return x << 11;
}
#endif

static void decode_sound(uint8_t *src, size_t size, const char *fn) {
	struct {
		char riff[4];
		uint32_t file_size;
		char wavefmt[8];
		uint32_t len;
		uint16_t fmt, ch;
		uint32_t freq;
		uint32_t bytes_sec;
		uint16_t bytes_sample, bits;
		char data[4];
		uint32_t data_size;
	} head;
	FILE *f;
	int bits = 16, ch = 1, freq = 8000, i;
	int bytes_sample = ch * (bits >> 3);
	int samples = (size - 1) * 2;
	int16_t *d, *data = malloc(samples * sizeof(*d));
	adpcm_status_t adpcm;
	if (!data) { printf("malloc failed"); return; }

	d = data;
	adpcm_init(&adpcm);
	for (i = 1; i < size; i++) {
		int a = src[i];
		*d++ = adpcm_decode(&adpcm, a & 15);
		*d++ = adpcm_decode(&adpcm, a >> 4);
	}

	memcpy(head.riff, "RIFF", 4);
	memcpy(head.wavefmt, "WAVEfmt ", 8);
	head.len = 16;
	head.fmt = 1;
	head.ch = ch;
	head.freq = freq;
	head.bytes_sec = freq * bytes_sample;
	head.bytes_sample = bytes_sample;
	head.bits = bits;
	memcpy(head.data, "data", 4);
	head.data_size = samples * bytes_sample;

	f = fopen(fn, "wb");
	if (f) {
		fwrite(&head, 1, sizeof(head), f);
		fwrite(data, 1, head.data_size, f);
		fclose(f);
	}
	free(data);
}

typedef struct {
	uint8_t *rom;
	uint32_t rom_size;
	uint8_t rom_key;
} sysctx_t;

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

int main(int argc, char **argv) {
	size_t rom_size = 0; uint8_t *rom, *p;
	unsigned i, res_tab, end; int res_idx = -1;
	const char *rom_fn, *out_fn = "res";
	sysctx_t sys;

	if (argc < 2) {
		printf("Usage: resextract flash.bin [path/name] [index]\n");
		return 0;
	}
	rom_fn = argv[1];
	if (argc > 2) out_fn = argv[2];
	if (argc > 3) {
		res_idx = strtol(argv[3], NULL, 0);
		if (res_idx >> 24) return 1;
	}

	rom = loadfile(rom_fn, &rom_size, 8 << 20);
	if (!rom) ERR_EXIT("loading ROM failed\n");

	sys.rom = rom;
	sys.rom_size = rom_size;
	check_rom(&sys);

	p = pal[0];
	for (i = 0; i < 256; i++, p += 3) {
		static const uint8_t curve_r[] = { 0, 8, 24, 57, 99, 123, 214, 255 };
		static const uint8_t curve_g[] = { 0, 12, 24, 48, 85, 125, 170, 255 };
		static const uint8_t curve_b[] = { 0, 66, 132, 255 };
		p[0] = curve_r[i >> 5 & 7];
		p[1] = curve_g[i >> 2 & 7];
		p[2] = curve_b[i & 3];
	}

	res_tab = READ24(rom);
	if (rom_size < res_tab + 6) return 1;
	end = rom_size - res_tab - 5;
	i = 0;
	if (res_idx >= 0) {
		i = res_idx;
		if (end > i * 3) end = i * 3 + 1;
	}
	for (; i * 3 < end; i++) {
		const char * const ext[] = { "bin", "ppm", "wav", "pbm" };
		char name[256]; int type = 0;
		unsigned addr = READ24(rom + res_tab + i * 3);
		unsigned next = READ24(rom + res_tab + i * 3 + 3);
		unsigned res_size;
		if (next == 0xffffff) next = res_tab;
		if (addr >= next) return 1;
		if (next > rom_size) return 1;
		res_size = next - addr;
		if (res_size >= 4) {
			if (rom[addr + 3] == 0x80 && rom[addr + 1] == 0) type = 1;
#if 1
			else if (rom[addr] == 0x81) type = 2;
#endif
			else {
				int w = rom[addr], h = rom[addr + 1];
				int st = (w + 7) >> 3;
				if (w <= 0x80 && h <= 0x80 && res_size == st * h + 2) type = 3;
			}
		}

		if (res_idx >= 0)
			snprintf(name, sizeof(name), "%s.%s", out_fn, ext[type]);
		else
			snprintf(name, sizeof(name), "%s%u.%s", out_fn, i, ext[type]);

		if (type == 1) {
			int ret = decode_image(rom + addr, res_size, name);
			if (ret) printf("error at res%u (addr = 0x%x)\n", i, addr);
		} else if (type == 2) {
			decode_sound(rom + addr, res_size, name);
		} else if (type == 3) {
			int ret = decode_image_1bit(rom + addr, res_size, name);
			if (ret) printf("error at res%u (addr = 0x%x)\n", i, addr);
		} else {
			FILE *f = fopen(name, "wb");
			if (!f) break;
			fwrite(rom + addr, 1, res_size, f);
			fclose(f);
		}
	}
	return 0;
}

