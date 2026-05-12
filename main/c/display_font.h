#pragma once

#include <stdint.h>

#define TINYBLOK_OLED_GLYPH_WIDTH 5

static inline const uint8_t *tinyblok_oled_glyph(char ch)
{
    static const uint8_t blank[TINYBLOK_OLED_GLYPH_WIDTH] = {0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t bang[TINYBLOK_OLED_GLYPH_WIDTH] = {0x00, 0x00, 0x5F, 0x00, 0x00};
    static const uint8_t dash[TINYBLOK_OLED_GLYPH_WIDTH] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t dot[TINYBLOK_OLED_GLYPH_WIDTH] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t slash[TINYBLOK_OLED_GLYPH_WIDTH] = {0x20, 0x10, 0x08, 0x04, 0x02};
    static const uint8_t colon[TINYBLOK_OLED_GLYPH_WIDTH] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t greater[TINYBLOK_OLED_GLYPH_WIDTH] = {0x00, 0x41, 0x22, 0x14, 0x08};
    static const uint8_t digit0[TINYBLOK_OLED_GLYPH_WIDTH] = {0x3E, 0x51, 0x49, 0x45, 0x3E};
    static const uint8_t digit1[TINYBLOK_OLED_GLYPH_WIDTH] = {0x00, 0x42, 0x7F, 0x40, 0x00};
    static const uint8_t digit2[TINYBLOK_OLED_GLYPH_WIDTH] = {0x42, 0x61, 0x51, 0x49, 0x46};
    static const uint8_t digit3[TINYBLOK_OLED_GLYPH_WIDTH] = {0x21, 0x41, 0x45, 0x4B, 0x31};
    static const uint8_t digit4[TINYBLOK_OLED_GLYPH_WIDTH] = {0x18, 0x14, 0x12, 0x7F, 0x10};
    static const uint8_t digit5[TINYBLOK_OLED_GLYPH_WIDTH] = {0x27, 0x45, 0x45, 0x45, 0x39};
    static const uint8_t digit6[TINYBLOK_OLED_GLYPH_WIDTH] = {0x3C, 0x4A, 0x49, 0x49, 0x30};
    static const uint8_t digit7[TINYBLOK_OLED_GLYPH_WIDTH] = {0x01, 0x71, 0x09, 0x05, 0x03};
    static const uint8_t digit8[TINYBLOK_OLED_GLYPH_WIDTH] = {0x36, 0x49, 0x49, 0x49, 0x36};
    static const uint8_t digit9[TINYBLOK_OLED_GLYPH_WIDTH] = {0x06, 0x49, 0x49, 0x29, 0x1E};
    static const uint8_t glyph_a[TINYBLOK_OLED_GLYPH_WIDTH] = {0x20, 0x54, 0x54, 0x54, 0x78};
    static const uint8_t glyph_b[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7F, 0x48, 0x44, 0x44, 0x38};
    static const uint8_t glyph_c[TINYBLOK_OLED_GLYPH_WIDTH] = {0x38, 0x44, 0x44, 0x44, 0x20};
    static const uint8_t glyph_d[TINYBLOK_OLED_GLYPH_WIDTH] = {0x38, 0x44, 0x44, 0x48, 0x7F};
    static const uint8_t glyph_e[TINYBLOK_OLED_GLYPH_WIDTH] = {0x38, 0x54, 0x54, 0x54, 0x18};
    static const uint8_t glyph_f[TINYBLOK_OLED_GLYPH_WIDTH] = {0x08, 0x7E, 0x09, 0x01, 0x02};
    static const uint8_t glyph_g[TINYBLOK_OLED_GLYPH_WIDTH] = {0x08, 0x54, 0x54, 0x54, 0x3C};
    static const uint8_t glyph_h[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7F, 0x08, 0x04, 0x04, 0x78};
    static const uint8_t glyph_i[TINYBLOK_OLED_GLYPH_WIDTH] = {0x00, 0x44, 0x7D, 0x40, 0x00};
    static const uint8_t glyph_k[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7F, 0x10, 0x28, 0x44, 0x00};
    static const uint8_t glyph_l[TINYBLOK_OLED_GLYPH_WIDTH] = {0x00, 0x41, 0x7F, 0x40, 0x00};
    static const uint8_t glyph_n[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7C, 0x08, 0x04, 0x04, 0x78};
    static const uint8_t glyph_o[TINYBLOK_OLED_GLYPH_WIDTH] = {0x38, 0x44, 0x44, 0x44, 0x38};
    static const uint8_t glyph_p[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7C, 0x14, 0x14, 0x14, 0x08};
    static const uint8_t glyph_r[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7C, 0x08, 0x04, 0x04, 0x08};
    static const uint8_t glyph_s[TINYBLOK_OLED_GLYPH_WIDTH] = {0x48, 0x54, 0x54, 0x54, 0x20};
    static const uint8_t glyph_t[TINYBLOK_OLED_GLYPH_WIDTH] = {0x04, 0x3F, 0x44, 0x40, 0x20};
    static const uint8_t glyph_v[TINYBLOK_OLED_GLYPH_WIDTH] = {0x1C, 0x20, 0x40, 0x20, 0x1C};
    static const uint8_t glyph_y[TINYBLOK_OLED_GLYPH_WIDTH] = {0x0C, 0x50, 0x50, 0x50, 0x3C};
    static const uint8_t glyph_A[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7E, 0x11, 0x11, 0x11, 0x7E};
    static const uint8_t glyph_B[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7F, 0x49, 0x49, 0x49, 0x36};
    static const uint8_t glyph_C[TINYBLOK_OLED_GLYPH_WIDTH] = {0x3E, 0x41, 0x41, 0x41, 0x22};
    static const uint8_t glyph_F[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7F, 0x09, 0x09, 0x09, 0x01};
    static const uint8_t glyph_H[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
    static const uint8_t glyph_I[TINYBLOK_OLED_GLYPH_WIDTH] = {0x00, 0x41, 0x7F, 0x41, 0x00};
    static const uint8_t glyph_K[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7F, 0x08, 0x14, 0x22, 0x41};
    static const uint8_t glyph_L[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7F, 0x40, 0x40, 0x40, 0x40};
    static const uint8_t glyph_N[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7F, 0x02, 0x0C, 0x10, 0x7F};
    static const uint8_t glyph_O[TINYBLOK_OLED_GLYPH_WIDTH] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
    static const uint8_t glyph_P[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7F, 0x09, 0x09, 0x09, 0x06};
    static const uint8_t glyph_S[TINYBLOK_OLED_GLYPH_WIDTH] = {0x46, 0x49, 0x49, 0x49, 0x31};
    static const uint8_t glyph_T[TINYBLOK_OLED_GLYPH_WIDTH] = {0x01, 0x01, 0x7F, 0x01, 0x01};
    static const uint8_t glyph_U[TINYBLOK_OLED_GLYPH_WIDTH] = {0x3F, 0x40, 0x40, 0x40, 0x3F};
    static const uint8_t glyph_W[TINYBLOK_OLED_GLYPH_WIDTH] = {0x7F, 0x20, 0x18, 0x20, 0x7F};
    static const uint8_t glyph_Y[TINYBLOK_OLED_GLYPH_WIDTH] = {0x07, 0x08, 0x70, 0x08, 0x07};

    if (ch >= '0' && ch <= '9')
    {
        static const uint8_t *const digits[10] = {
            digit0, digit1, digit2, digit3, digit4, digit5, digit6, digit7, digit8, digit9,
        };
        return digits[ch - '0'];
    }

    switch (ch)
    {
    case ' ': return blank;
    case '!': return bang;
    case '-': return dash;
    case '.': return dot;
    case ':': return colon;
    case '/': return slash;
    case '>': return greater;
    case 'A': return glyph_A;
    case 'B': return glyph_B;
    case 'C': return glyph_C;
    case 'F': return glyph_F;
    case 'H': return glyph_H;
    case 'I': return glyph_I;
    case 'K': return glyph_K;
    case 'L': return glyph_L;
    case 'N': return glyph_N;
    case 'O': return glyph_O;
    case 'P': return glyph_P;
    case 'S': return glyph_S;
    case 'T': return glyph_T;
    case 'U': return glyph_U;
    case 'W': return glyph_W;
    case 'Y': return glyph_Y;
    case 'a': return glyph_a;
    case 'b': return glyph_b;
    case 'c': return glyph_c;
    case 'd': return glyph_d;
    case 'e': return glyph_e;
    case 'f': return glyph_f;
    case 'g': return glyph_g;
    case 'h': return glyph_h;
    case 'i': return glyph_i;
    case 'k': return glyph_k;
    case 'l': return glyph_l;
    case 'n': return glyph_n;
    case 'o': return glyph_o;
    case 'p': return glyph_p;
    case 'r': return glyph_r;
    case 's': return glyph_s;
    case 't': return glyph_t;
    case 'v': return glyph_v;
    case 'y': return glyph_y;
    default: return blank;
    }
}
