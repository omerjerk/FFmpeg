/*
 * raw ALS muxer
 * Copyright (c) 2010 Thilo Borgmann <thilo.borgmann _at_ googlemail.com>
 * Copyright (c) 2010 Justin Ruggles <justin.ruggles@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavcodec/mpeg4audio.h"
#include "avformat.h"

typedef struct AlsEncContext {
    int header_size;
} AlsEncContext;

static int als_write_header(struct AVFormatContext *s)
{
    MPEG4AudioConfig m4ac;
    AVCodecParameters *par = s->streams[0]->codecpar;
    AlsEncContext *ctx = s->priv_data;
    int config_offset;

    /* get offset of ALSSpecificConfig in extradata */
    config_offset = avpriv_mpeg4audio_get_config(&m4ac, par->extradata,
                                             par->extradata_size, 1);
    if (config_offset < 0)
        return -1;

    config_offset = (config_offset + 7) >> 3;
    ctx->header_size = par->extradata_size - config_offset;

    /* write STREAMINFO or full header */
    avio_write(s->pb, &par->extradata[config_offset], ctx->header_size);

    return 0;
}

static int als_write_trailer(struct AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    AlsEncContext *ctx = s->priv_data;
    int header_size;

    header_size = ctx->header_size;

    if (pb->seekable) {
        /* rewrite the header */
        int64_t file_size   = avio_tell(pb);
        int     header_size = ctx->header_size;

        avio_seek(pb, 0, SEEK_SET);
        if (als_write_header(s))
            return -1;

        if (header_size != ctx->header_size) {
            av_log(s, AV_LOG_WARNING, "ALS header size mismatch. Unable to rewrite header.\n");
        }
        avio_seek(pb, file_size, SEEK_SET);
        avio_flush(pb);
    } else {
        av_log(s, AV_LOG_WARNING, "unable to rewrite ALS header.\n");
    }

    return 0;
}

static int als_write_packet(struct AVFormatContext *s, AVPacket *pkt)
{
    avio_write(s->pb, pkt->data, pkt->size);
    avio_flush(s->pb);
    return 0;
}

AVOutputFormat ff_als_muxer = {
    .name             = "als",
    .long_name        = NULL_IF_CONFIG_SMALL("raw MPEG-4 Audio Lossless Coding (ALS)"),
    .priv_data_size   = sizeof(AlsEncContext),
    .mime_type        = NULL,
    .extensions       = "als",
    .audio_codec      = AV_CODEC_ID_MP4ALS,
    .video_codec      = AV_CODEC_ID_NONE,
    .write_header     = als_write_header,
    .write_packet     = als_write_packet,
    .write_trailer    = als_write_trailer,
    .flags            = AVFMT_NOTIMESTAMPS,
};
