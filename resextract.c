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

// uses unknown 4-bit ADPCM
// but this rough guess sounds close
static inline int sound_filter(int x) {
	x = x & 8 ? 7 - x : x;
	return 128 + x * 8;
}

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
	int bits = 8, ch = 1, freq = 8000, i;
	int bytes_sample = ch * (bits >> 3);
	int samples = (size - 1) * 2;
	uint8_t *d, *data = malloc(samples);
	if (!data) { printf("malloc failed"); return; }

	d = data;
	for (i = 1; i < size; i++) {
		int a = src[i];
		*d++ = sound_filter(a & 15);
		*d++ = sound_filter(a >> 4);
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
#if 1
		// pow(i * 1.0 / 7, 2.0) * 255 + 0.5
		uint8_t gamma3[] = { 0, 5, 21, 47, 83, 130, 187, 255 };
		// pow(i * 1.0 / 3, 2.0) * 255 + 0.5
		uint8_t gamma2[] = { 0, 28, 113, 255 };
		p[0] = gamma3[i >> 5 & 7];
		p[1] = gamma3[i >> 2 & 7];
		p[2] = gamma2[i & 3];
#else
		p[0] = (i >> 5 & 7) * 0x49 >> 1;
		p[1] = (i >> 2 & 7) * 0x49 >> 1;
		p[2] = (i & 3) * 0x55;
#endif
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
		const char * const ext[] = { "bin", "ppm", "wav" };
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
		} else {
			FILE *f = fopen(name, "wb");
			if (!f) break;
			fwrite(rom + addr, 1, res_size, f);
			fclose(f);
		}
	}
	return 0;
}

