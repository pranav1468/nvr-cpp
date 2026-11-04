#ifndef JO_JPEG_H
#define JO_JPEG_H

// jo_jpeg by Jon Olick - Public Domain / MIT License
// Minimal C/C++ JPEG encoder

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef JO_MALLOC
#define JO_MALLOC(sz) malloc(sz)
#define JO_FREE(p) free(p)
#endif

typedef void (*jo_write_func)(void *context, void *data, int size);

static void jo_write_bits(void *context, jo_write_func write_fn, unsigned int *bitbuf, int *bitcount, unsigned int code, int size) {
    *bitbuf |= (code << (32 - *bitcount - size));
    *bitcount += size;
    while (*bitcount >= 8) {
        unsigned char b = (unsigned char)(*bitbuf >> 24);
        write_fn(context, &b, 1);
        if (b == 0xff) {
            unsigned char zero = 0;
            write_fn(context, &zero, 1);
        }
        *bitbuf <<= 8;
        *bitcount -= 8;
    }
}

static void jo_calc_quant_table(float *q_table, const unsigned char *basic_table, int scale) {
    for (int i = 0; i < 64; ++i) {
        long q = ((long)basic_table[i] * scale + 50) / 100;
        if (q < 1) q = 1;
        if (q > 255) q = 255;
        q_table[i] = (float)q;
    }
}

static const unsigned char jo_zigzag[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

static const unsigned char jo_std_lum_qt[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99
};

static const unsigned char jo_std_chrom_qt[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

static const unsigned char jo_dc_lum_nr_codes[17] = { 0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const unsigned char jo_dc_lum_values[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const unsigned char jo_dc_chrom_nr_codes[17] = { 0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const unsigned char jo_dc_chrom_values[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

static const unsigned char jo_ac_lum_nr_codes[17] = { 0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const unsigned char jo_ac_lum_values[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

static const unsigned char jo_ac_chrom_nr_codes[17] = { 0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const unsigned char jo_ac_chrom_values[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

typedef struct {
    unsigned short code[256];
    unsigned char size[256];
} jo_huff_table;

static void jo_create_huff_table(jo_huff_table *ht, const unsigned char *nr_codes, const unsigned char *values) {
    int p = 0;
    for (int l = 1; l <= 16; ++l) {
        for (int i = 0; i < nr_codes[l]; ++i) {
            ht->size[values[p++]] = (unsigned char)l;
        }
    }
    unsigned short code = 0;
    int si = ht->size[values[0]];
    p = 0;
    while (p < 256 && values[p]) {
        while (((int)ht->size[values[p]]) == si) {
            ht->code[values[p++]] = code++;
        }
        code <<= 1;
        si++;
    }
}

static void jo_fdct8x8(float *data) {
    static const float a1 = 0.707106781f;
    static const float a2 = 0.541196100f;
    static const float a3 = 0.707106781f;
    static const float a4 = 1.306562965f;
    static const float a5 = 0.382683432f;

    for (int i = 0; i < 8; ++i) {
        float *row = data + i * 8;
        float b0 = row[0] + row[7];
        float b1 = row[1] + row[6];
        float b2 = row[2] + row[5];
        float b3 = row[3] + row[4];
        float b4 = row[3] - row[4];
        float b5 = row[2] - row[5];
        float b6 = row[1] - row[6];
        float b7 = row[0] - row[7];

        float c0 = b0 + b3;
        float c1 = b1 + b2;
        float c2 = b1 - b2;
        float c3 = b0 - b3;
        float c5 = (b6 - b5) * a1;

        row[0] = c0 + c1;
        row[4] = c0 - c1;
        row[2] = c3 + c2 * a1;
        row[6] = c3 - c2 * a1;

        float d0 = b7 + b4;
        float d1 = b7 - b4;
        float d2 = b6 + b5;
        float d3 = d0 - d2;
        float d4 = d3 * a5;

        row[1] = d0 + d4 + (d1 + c5) * a2;
        row[5] = d0 - d4 + (d1 - c5) * a4;
        row[3] = d1 + c5 - d4 - (d0 + d2) * a4;
        row[7] = d1 - c5 - d4 - (d0 - d2) * a2;
    }

    for (int i = 0; i < 8; ++i) {
        float b0 = data[i + 0*8] + data[i + 7*8];
        float b1 = data[i + 1*8] + data[i + 6*8];
        float b2 = data[i + 2*8] + data[i + 5*8];
        float b3 = data[i + 3*8] + data[i + 4*8];
        float b4 = data[i + 3*8] - data[i + 4*8];
        float b5 = data[i + 2*8] - data[i + 5*8];
        float b6 = data[i + 1*8] - data[i + 6*8];
        float b7 = data[i + 0*8] - data[i + 7*8];

        float c0 = b0 + b3;
        float c1 = b1 + b2;
        float c2 = b1 - b2;
        float c3 = b0 - b3;
        float c5 = (b6 - b5) * a1;

        data[i + 0*8] = c0 + c1;
        data[i + 4*8] = c0 - c1;
        data[i + 2*8] = c3 + c2 * a1;
        data[i + 6*8] = c3 - c2 * a1;

        float d0 = b7 + b4;
        float d1 = b7 - b4;
        float d2 = b6 + b5;
        float d3 = d0 - d2;
        float d4 = d3 * a5;

        data[i + 1*8] = d0 + d4 + (d1 + c5) * a2;
        data[i + 5*8] = d0 - d4 + (d1 - c5) * a4;
        data[i + 3*8] = d1 + c5 - d4 - (d0 + d2) * a4;
        data[i + 7*8] = d1 - c5 - d4 - (d0 - d2) * a2;
    }
}

static int jo_encode_block(void *context, jo_write_func write_fn, unsigned int *bitbuf, int *bitcount,
                           float *block, float *q_table, int last_dc,
                           const jo_huff_table *dc_ht, const jo_huff_table *ac_ht) {
    jo_fdct8x8(block);

    int qblock[64];
    for (int i = 0; i < 64; ++i) {
        float v = block[i] * q_table[i];
        qblock[jo_zigzag[i]] = (int)(v > 0 ? (v + 0.5f) : (v - 0.5f));
    }

    // DC encoding
    int dc = qblock[0];
    int diff = dc - last_dc;
    if (diff == 0) {
        jo_write_bits(context, write_fn, bitbuf, bitcount, dc_ht->code[0], dc_ht->size[0]);
    } else {
        int abs_diff = diff < 0 ? -diff : diff;
        int bits = 0;
        while (abs_diff) { bits++; abs_diff >>= 1; }
        jo_write_bits(context, write_fn, bitbuf, bitcount, dc_ht->code[bits], dc_ht->size[bits]);
        unsigned int val = diff < 0 ? (diff - 1 + (1 << bits)) : diff;
        jo_write_bits(context, write_fn, bitbuf, bitcount, val, bits);
    }

    // AC encoding
    int r = 0;
    for (int k = 1; k < 64; ++k) {
        int ac = qblock[k];
        if (ac == 0) {
            r++;
        } else {
            while (r > 15) {
                jo_write_bits(context, write_fn, bitbuf, bitcount, ac_ht->code[0xf0], ac_ht->size[0xf0]);
                r -= 16;
            }
            int abs_ac = ac < 0 ? -ac : ac;
            int bits = 0;
            while (abs_ac) { bits++; abs_ac >>= 1; }
            int symbol = (r << 4) | bits;
            jo_write_bits(context, write_fn, bitbuf, bitcount, ac_ht->code[symbol], ac_ht->size[symbol]);
            unsigned int val = ac < 0 ? (ac - 1 + (1 << bits)) : ac;
            jo_write_bits(context, write_fn, bitbuf, bitcount, val, bits);
            r = 0;
        }
    }
    if (r > 0) {
        jo_write_bits(context, write_fn, bitbuf, bitcount, ac_ht->code[0], ac_ht->size[0]);
    }

    return dc;
}

static inline void jo_write_byte(void *context, jo_write_func write_fn, unsigned char b) {
    write_fn(context, &b, 1);
}

static inline void jo_write_word(void *context, jo_write_func write_fn, unsigned short w) {
    unsigned char b[2] = { (unsigned char)(w >> 8), (unsigned char)(w & 0xff) };
    write_fn(context, b, 2);
}

static int jo_write_jpg_custom(void *context, jo_write_func write_fn, const void *rgb_data, int width, int height, int comp, int quality) {
    if (!rgb_data || width <= 0 || height <= 0 || comp < 3) return 0;

    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int scale = quality < 50 ? (5000 / quality) : (200 - quality * 2);

    float q_lum[64], q_chrom[64];
    jo_calc_quant_table(q_lum, jo_std_lum_qt, scale);
    jo_calc_quant_table(q_chrom, jo_std_chrom_qt, scale);

    // AAN scaling factors
    static const float aan_scale[8] = {
        1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
        1.0f, 0.785694958f, 0.541196100f, 0.275899379f
    };
    float q_lum_scaled[64], q_chrom_scaled[64];
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            int i = y * 8 + x;
            float factor = 1.0f / (aan_scale[x] * aan_scale[y] * 8.0f);
            q_lum_scaled[i] = factor / q_lum[i];
            q_chrom_scaled[i] = factor / q_chrom[i];
        }
    }

    // SOI
    jo_write_word(context, write_fn, 0xffd8);

    // APP0 (JFIF)
    jo_write_word(context, write_fn, 0xffe0);
    jo_write_word(context, write_fn, 16);
    write_fn(context, (void*)"JFIF\0\1\1\0\0\1\0\1\0\0", 14);

    // DQT (Lum)
    jo_write_word(context, write_fn, 0xffdb);
    jo_write_word(context, write_fn, 67);
    jo_write_byte(context, write_fn, 0);
    for (int i = 0; i < 64; ++i) {
        jo_write_byte(context, write_fn, (unsigned char)q_lum[jo_zigzag[i]]);
    }

    // DQT (Chrom)
    jo_write_word(context, write_fn, 0xffdb);
    jo_write_word(context, write_fn, 67);
    jo_write_byte(context, write_fn, 1);
    for (int i = 0; i < 64; ++i) {
        jo_write_byte(context, write_fn, (unsigned char)q_chrom[jo_zigzag[i]]);
    }

    // SOF0
    jo_write_word(context, write_fn, 0xffc0);
    jo_write_word(context, write_fn, 17);
    jo_write_byte(context, write_fn, 8); // 8-bit precision
    jo_write_word(context, write_fn, (unsigned short)height);
    jo_write_word(context, write_fn, (unsigned short)width);
    jo_write_byte(context, write_fn, 3); // 3 components (Y, Cb, Cr)
    jo_write_byte(context, write_fn, 1); // Component ID 1 (Y)
    jo_write_byte(context, write_fn, 0x11); // 1x1 subsampling
    jo_write_byte(context, write_fn, 0); // Quant table 0
    jo_write_byte(context, write_fn, 2); // Component ID 2 (Cb)
    jo_write_byte(context, write_fn, 0x11);
    jo_write_byte(context, write_fn, 1); // Quant table 1
    jo_write_byte(context, write_fn, 3); // Component ID 3 (Cr)
    jo_write_byte(context, write_fn, 0x11);
    jo_write_byte(context, write_fn, 1);

    // DHT
    jo_huff_table dc_lum_ht, dc_chrom_ht, ac_lum_ht, ac_chrom_ht;
    memset(&dc_lum_ht, 0, sizeof(dc_lum_ht));
    memset(&dc_chrom_ht, 0, sizeof(dc_chrom_ht));
    memset(&ac_lum_ht, 0, sizeof(ac_lum_ht));
    memset(&ac_chrom_ht, 0, sizeof(ac_chrom_ht));
    jo_create_huff_table(&dc_lum_ht, jo_dc_lum_nr_codes, jo_dc_lum_values);
    jo_create_huff_table(&dc_chrom_ht, jo_dc_chrom_nr_codes, jo_dc_chrom_values);
    jo_create_huff_table(&ac_lum_ht, jo_ac_lum_nr_codes, jo_ac_lum_values);
    jo_create_huff_table(&ac_chrom_ht, jo_ac_chrom_nr_codes, jo_ac_chrom_values);

    // Write DHT Lum DC
    jo_write_word(context, write_fn, 0xffc4);
    jo_write_word(context, write_fn, 19 + 12);
    jo_write_byte(context, write_fn, 0x00);
    write_fn(context, (void*)(jo_dc_lum_nr_codes + 1), 16);
    write_fn(context, (void*)jo_dc_lum_values, 12);

    // Write DHT Lum AC
    jo_write_word(context, write_fn, 0xffc4);
    jo_write_word(context, write_fn, 19 + 162);
    jo_write_byte(context, write_fn, 0x10);
    write_fn(context, (void*)(jo_ac_lum_nr_codes + 1), 16);
    write_fn(context, (void*)jo_ac_lum_values, 162);

    // Write DHT Chrom DC
    jo_write_word(context, write_fn, 0xffc4);
    jo_write_word(context, write_fn, 19 + 12);
    jo_write_byte(context, write_fn, 0x01);
    write_fn(context, (void*)(jo_dc_chrom_nr_codes + 1), 16);
    write_fn(context, (void*)jo_dc_chrom_values, 12);

    // Write DHT Chrom AC
    jo_write_word(context, write_fn, 0xffc4);
    jo_write_word(context, write_fn, 19 + 162);
    jo_write_byte(context, write_fn, 0x11);
    write_fn(context, (void*)(jo_ac_chrom_nr_codes + 1), 16);
    write_fn(context, (void*)jo_ac_chrom_values, 162);

    // SOS
    jo_write_word(context, write_fn, 0xffda);
    jo_write_word(context, write_fn, 12);
    jo_write_byte(context, write_fn, 3);
    jo_write_byte(context, write_fn, 1); jo_write_byte(context, write_fn, 0x00);
    jo_write_byte(context, write_fn, 2); jo_write_byte(context, write_fn, 0x11);
    jo_write_byte(context, write_fn, 3); jo_write_byte(context, write_fn, 0x11);
    jo_write_byte(context, write_fn, 0); jo_write_byte(context, write_fn, 63); jo_write_byte(context, write_fn, 0);

    // Scan loop
    unsigned int bitbuf = 0;
    int bitcount = 0;
    int last_dc_y = 0, last_dc_cb = 0, last_dc_cr = 0;
    const unsigned char *src = (const unsigned char*)rgb_data;

    for (int y = 0; y < height; y += 8) {
        for (int x = 0; x < width; x += 8) {
            float block_y[64], block_cb[64], block_cr[64];
            for (int by = 0; by < 8; ++by) {
                int py = y + by;
                if (py >= height) py = height - 1;
                for (int bx = 0; bx < 8; ++bx) {
                    int px = x + bx;
                    if (px >= width) px = width - 1;
                    int idx = (py * width + px) * comp;
                    float r = (float)src[idx + 0];
                    float g = (float)src[idx + 1];
                    float b = (float)src[idx + 2];
                    block_y[by * 8 + bx]  = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
                    block_cb[by * 8 + bx] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                    block_cr[by * 8 + bx] = 0.5f * r - 0.418688f * g - 0.081312f * b;
                }
            }

            last_dc_y  = jo_encode_block(context, write_fn, &bitbuf, &bitcount, block_y, q_lum_scaled, last_dc_y, &dc_lum_ht, &ac_lum_ht);
            last_dc_cb = jo_encode_block(context, write_fn, &bitbuf, &bitcount, block_cb, q_chrom_scaled, last_dc_cb, &dc_chrom_ht, &ac_chrom_ht);
            last_dc_cr = jo_encode_block(context, write_fn, &bitbuf, &bitcount, block_cr, q_chrom_scaled, last_dc_cr, &dc_chrom_ht, &ac_chrom_ht);
        }
    }

    // Flush remaining bits
    if (bitcount > 0) {
        unsigned int code = (1 << (8 - bitcount)) - 1;
        jo_write_bits(context, write_fn, &bitbuf, &bitcount, code, 8 - bitcount);
    }

    // EOI
    jo_write_word(context, write_fn, 0xffd9);
    return 1;
}

#endif // JO_JPEG_H
