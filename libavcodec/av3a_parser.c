/*
 * Audio Vivid Codec
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

#include "libavutil/samplefmt.h"
#include "libavutil/intreadwrite.h"
#include "parser.h"
#include "get_bits.h"
#include "libavcodec/av3a.h"

typedef struct AVS3AudioParseContext
{
    int frame_size;
    int bitdepth;
    int sample_rate;
    uint64_t bit_rate;
    uint16_t channels;
    uint64_t channel_layout;

} AVS3AParseContext;

static int raw_av3a_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                          const uint8_t **poutbuf, int *poutbuf_size,
                          const uint8_t *buf, int buf_size)
{
    int ret = 0;
    uint8_t header[MAX_NBYTES_FRAME_HEADER];
    AVS3AHeaderInfo hdf;
    if (buf_size < MAX_NBYTES_FRAME_HEADER) {
        return buf_size;
    }

    memcpy(header, buf, MAX_NBYTES_FRAME_HEADER);

    // read frame header
    if ((ret = read_av3a_frame_header(&hdf, header, MAX_NBYTES_FRAME_HEADER)) != 0) {
        return ret;
    }

    avctx->sample_rate = hdf.sample_rate;
    avctx->bit_rate = hdf.total_bitrate;
    avctx->channels = hdf.total_channels;
    avctx->channel_layout = hdf.channel_layout;
    avctx->frame_size = AVS3_AUDIO_FRAME_SIZE;
    s->format = hdf.bitdepth;

    /* return the full packet */
    *poutbuf = buf;
    *poutbuf_size = buf_size;

    return buf_size;
}

#if CONFIG_AV3A_PARSER
AVCodecParser ff_av3a_parser = {
    .codec_ids = {AV_CODEC_ID_AVS3_AUDIO},
    .priv_data_size = sizeof(AVS3AParseContext),
    .parser_parse = raw_av3a_parse,
};
#endif
