#pragma once
#ifndef AVCODEC_AVCODEC_H
#define AVCODEC_AVCODEC_H

#include <stdint.h>
#include <stddef.h>
#include "libavutil/frame.h"
#include "libavutil/pixfmt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AVPacketSideData AVPacketSideData;

typedef struct AVPacket {
    AVBufferRef *buf;
    int64_t pts;
    int64_t dts;
    uint8_t *data;
    int size;
    int stream_index;
    int flags;
    AVPacketSideData *side_data;
    int side_data_elems;
    int64_t duration;
    int64_t pos;
} AVPacket;

typedef struct AVCodec AVCodec;
typedef struct AVCodecContext AVCodecContext;

unsigned int avcodec_version(void);
const AVCodec *avcodec_find_decoder_by_name(const char *name);
AVCodecContext *avcodec_alloc_context3(const AVCodec *codec);
int avcodec_open2(AVCodecContext *avctx, const AVCodec *codec, void **options);
void avcodec_free_context(AVCodecContext **avctx);
int avcodec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt);
int avcodec_receive_frame(AVCodecContext *avctx, AVFrame *frame);

AVPacket *av_packet_alloc(void);
void av_packet_free(AVPacket **pkt);
void av_packet_unref(AVPacket *pkt);

#ifdef __cplusplus
}
#endif

#endif // AVCODEC_AVCODEC_H
