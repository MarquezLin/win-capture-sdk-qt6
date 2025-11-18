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

void gcap::yuy2_to_argb(const uint8_t *yuy2,
                        int width, int height, int yuy2Stride,
                        uint8_t *outARGB, int outStride)
{
    for (int j = 0; j < height; ++j)
    {
        const uint8_t *src = yuy2 + j * yuy2Stride;
        uint8_t *dst = outARGB + j * outStride;
        for (int i = 0; i < width; i += 2)
        {
            uint8_t Y0 = src[0];
            uint8_t U = src[1];
            uint8_t Y1 = src[2];
            uint8_t V = src[3];
            src += 4;

            uint8_t r, g, b;
            yuv_to_rgb(Y0, U, V, r, g, b);
            dst[0] = b;
            dst[1] = g;
            dst[2] = r;
            dst[3] = 255;

            yuv_to_rgb(Y1, U, V, r, g, b);
            dst[4] = b;
            dst[5] = g;
            dst[6] = r;
            dst[7] = 255;

            dst += 8;
        }
    }
}
