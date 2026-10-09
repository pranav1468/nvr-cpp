#pragma once
#ifndef AVUTIL_FRAME_H
#define AVUTIL_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include "pixfmt.h"

#define AV_NUM_DATA_POINTERS 8

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AVBufferRef AVBufferRef;
typedef struct AVFrameSideData AVFrameSideData;

typedef struct AVFrame {
    uint8_t *data[AV_NUM_DATA_POINTERS];
    int linesize[AV_NUM_DATA_POINTERS];
    uint8_t **extended_data;
    int width, height;
    int nb_samples;
    int format;
    int key_frame;
    int pict_type;
} AVFrame;

AVFrame *av_frame_alloc(void);
void av_frame_free(AVFrame **frame);
void av_frame_unref(AVFrame *frame);

#ifdef __cplusplus
}
#endif

#endif // AVUTIL_FRAME_H
