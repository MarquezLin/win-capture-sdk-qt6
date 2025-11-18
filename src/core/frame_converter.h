// frame_converter.h
#pragma once
#include <cstdint>

namespace gcap
{
    // NV12 → ARGB
    void nv12_to_argb(const uint8_t *y, const uint8_t *uv,
                      int width, int height, int yStride, int uvStride,
                      uint8_t *outARGB, int outStride);

    // YUY2 → ARGB
    void yuy2_to_argb(const uint8_t *yuy2,
                      int width, int height, int yuy2Stride,
                      uint8_t *outARGB, int outStride);
}
