#pragma once
#ifndef AVUTIL_PIXFMT_H
#define AVUTIL_PIXFMT_H

#ifdef __cplusplus
extern "C" {
#endif

enum AVPixelFormat {
    AV_PIX_FMT_NONE = -1,
    AV_PIX_FMT_YUV420P = 0,
    AV_PIX_FMT_NV12 = 23,
    AV_PIX_FMT_NB
};

#ifdef __cplusplus
}
#endif

#endif // AVUTIL_PIXFMT_H
