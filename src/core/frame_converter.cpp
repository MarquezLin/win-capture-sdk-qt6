// frame_converter.cpp
#include "frame_converter.h"
#include <algorithm>

static inline void yuv_to_rgb(int Y, int U, int V, uint8_t &R, uint8_t &G, uint8_t &B)
{
    int C = Y - 16;
    int D = U - 128;
    int E = V - 128;
    int r = (298 * C + 409 * E + 128) >> 8;
    int g = (298 * C - 100 * D - 208 * E + 128) >> 8;
    int b = (298 * C + 516 * D + 128) >> 8;
    R = (uint8_t)std::clamp(r, 0, 255);
    G = (uint8_t)std::clamp(g, 0, 255);
    B = (uint8_t)std::clamp(b, 0, 255);
}

void gcap::nv12_to_argb(const uint8_t *y, const uint8_t *uv,
                        int w, int h, int yStride, int uvStride,
                        uint8_t *out, int outStride)
{
    for (int j = 0; j < h; ++j)
    {
        const uint8_t *yRow = y + j * yStride;
        const uint8_t *uvRow = uv + (j / 2) * uvStride;
        uint8_t *dst = out + j * outStride;
        for (int i = 0; i < w; i += 2)
        {
            uint8_t Y1 = yRow[i], Y2 = yRow[i + 1];
            uint8_t U = uvRow[i], V = uvRow[i + 1];
            uint8_t r, g, b;

            yuv_to_rgb(Y1, U, V, r, g, b);
            dst[0] = b;
            dst[1] = g;
            dst[2] = r;
            dst[3] = 255; // BGRA
            yuv_to_rgb(Y2, U, V, r, g, b);
            dst[4] = b;
            dst[5] = g;
            dst[6] = r;
            dst[7] = 255;
            dst += 8;
        }
    }
}

// ------------------------------------------------------------
// YUY2 → ARGB
// ------------------------------------------------------------
void gcap::yuy2_to_argb(const uint8_t *yuy2,
                        int width, int height,
                        int strideYUY2,
                        uint8_t *outARGB, int outStride)
{
    for (int y = 0; y < height; y++)
    {
        const uint8_t *src = yuy2 + y * strideYUY2;
        uint8_t *dst = outARGB + y * outStride;

        for (int x = 0; x < width; x += 2)
        {
            int Y0 = src[0];
            int U = src[1];
            int Y1 = src[2];
            int V = src[3];

            uint8_t r0, g0, b0;
            uint8_t r1, g1, b1;

            yuv_to_rgb(Y0, U, V, r0, g0, b0);
            yuv_to_rgb(Y1, U, V, r1, g1, b1);

            // pixel 0
            dst[0] = b0;
            dst[1] = g0;
            dst[2] = r0;
            dst[3] = 255; // BGRA

            // pixel 1
            dst[4] = b1;
            dst[5] = g1;
            dst[6] = r1;
            dst[7] = 255;

            src += 4;
            dst += 8;
        }
    }
}
