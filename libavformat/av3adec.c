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
#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"
#include "apetag.h"
#include "rawdec.h"
#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/channel_layout.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/av3a.h"
#include <string.h>

typedef struct
{
    AVClass *class;
    uint64_t file_size;
    uint64_t frame_cnt;
} AV3ADecContext;

/* Bytes assignment */
typedef struct
{
    uint8_t audio_codec_id;
    uint8_t sample_rate_index;
    uint8_t nn_type;
    uint8_t content_type;
    uint8_t channel_num_index;
    uint8_t num_objects;
    uint8_t hoa_order;
    uint8_t resolution_index;
    uint16_t total_bitrate_kbps;
} AV3ASpecificContext;

static int get_av3a_payload(AVFormatContext *s)
{

    int read_bytes = 0;
    uint16_t sync_word = 0;
    uint8_t header[MAX_NBYTES_FRAME_HEADER];
    GetBitContext gb;
    uint8_t stamping_index, num_channels_idx, bitrate_index, soundbed_type, hoa_order;
    uint8_t coding_profile;
    int sample_rate;
    uint64_t bit_rate;
    AVS3AChannelConfig channel_config;
    int payload_bytes = 0;
    uint16_t bitrateIdxPerObj, bitrateIdxBedMc;
    int numObjs, numChans;

    if ((read_bytes = avio_read(s->pb, header, MAX_NBYTES_FRAME_HEADER)) != MAX_NBYTES_FRAME_HEADER)
    {

        if (avio_feof(s->pb))
        {
            return AVERROR_EOF;
        }
        return AVERROR_INVALIDDATA;
    }

    init_get_bits8(&gb, header, MAX_NBYTES_FRAME_HEADER);

    // 12 bits for frame sync word
    sync_word = get_bits(&gb, 12);

    if (sync_word != AVS3_AUDIO_SYNC_WORD)
    {
        return AVERROR_INVALIDDATA;
    }

    // skip audio_codec_id 4 bits & anc data 1 bit & 3 bits nn type
    skip_bits(&gb, 5 + 3);

    coding_profile = get_bits(&gb, 3);

    stamping_index = get_bits(&gb, 4);

    // skip crc first
    skip_bits(&gb, 8);

    if (coding_profile == 0)
    {

        num_channels_idx = get_bits(&gb, 7);
        channel_config = (AVS3AChannelConfig)num_channels_idx;
    }
    else if (coding_profile == 1)
    {

        soundbed_type = get_bits(&gb, 2);

        if (soundbed_type == 0)
        {
            numObjs = get_bits(&gb, 7);
            numObjs += 1;
            bitrateIdxPerObj = get_bits(&gb, 4);
            bit_rate = codecBitrateConfigTable[CHANNEL_CONFIG_MONO].bitrateTable[bitrateIdxPerObj] * numObjs;
        }
        else if (soundbed_type == 1)
        {

            uint64_t bitratePerObj, bitrateBedMc;

            num_channels_idx = get_bits(&gb, 7);
            bitrateIdxBedMc = get_bits(&gb, 4);
            numObjs = get_bits(&gb, 7);
            numObjs += 1;
            bitrateIdxPerObj = get_bits(&gb, 4);

            channel_config = (AVS3AChannelConfig)num_channels_idx;

            // numChannels for sound bed
            for (int16_t i = 0; i < AVS3_SIZE_MC_CONFIG_TABLE; i++)
            {
                if (channel_config == mcChannelConfigTable[i].channelNumConfig)
                {
                    numChans = mcChannelConfigTable[i].numChannels;
                }
            }

            bitrateBedMc = codecBitrateConfigTable[channel_config].bitrateTable[bitrateIdxBedMc];

            // bitrate per obj
            bitratePerObj = codecBitrateConfigTable[CHANNEL_CONFIG_MONO].bitrateTable[bitrateIdxPerObj];

            // add num chans and num objs to get total chans
            numChans += numObjs;

            bit_rate = bitrateBedMc + bitratePerObj * numObjs;
        }
    }
    else if (coding_profile == 2)
    {

        hoa_order = get_bits(&gb, 4);
        hoa_order += 1;
        switch (hoa_order)
        {
        case 1:
            numChans = 4;
            channel_config = CHANNEL_CONFIG_HOA_ORDER1;
            break;
        case 2:
            numChans = 9;
            channel_config = CHANNEL_CONFIG_HOA_ORDER2;
            break;
        case 3:
            numChans = 16;
            channel_config = CHANNEL_CONFIG_HOA_ORDER3;
            break;
        default:
            break;
        }
    }

    // skip bit depth 2 bits
    skip_bits(&gb, 2);

    if (coding_profile != 1)
    {
        bitrate_index = get_bits(&gb, 4);
        bit_rate = codecBitrateConfigTable[channel_config].bitrateTable[bitrate_index];
    }

    // skip crc second part 8 bits
    skip_bits(&gb, 8);

    sample_rate = avs3_samplingrate_table[stamping_index];

    payload_bytes = (int)ceil((((float)(bit_rate) / sample_rate) * AVS3_AUDIO_FRAME_SIZE) / 8);

    avio_seek(s->pb, -read_bytes, SEEK_CUR);

    return payload_bytes;
}

static int av3a_probe(const AVProbeData *p)
{
    av_log(NULL, AV_LOG_ERROR, "wang:ren av3a_probe \n");

    uint16_t frame_sync_word;
    const char *ptr_ext = NULL;
    uint16_t lval = ((uint16_t)(p->buf[0]));
    uint16_t rval = ((uint16_t)(p->buf[1]));
    char ext_str[5]; // .av3a
    int len = 0;
    int len_ext = 0;
    frame_sync_word = ((lval << 8) | rval) >> 4;

    /* check sync word */
    if (frame_sync_word == AVS3_AUDIO_SYNC_WORD)
    {
        len_ext = strlen(".av3a");
        av_assert0(len_ext == 5);
        if (p->filename != NULL)
        {
            len = strlen(p->filename);
            if (len >= len_ext)
            {
                memcpy(ext_str, p->filename + len - len_ext, len_ext);
                ptr_ext = strrchr(ext_str, '.');
            }
        }

        if (ptr_ext != NULL && strcmp(ptr_ext, ".av3a") == 0)
        {
            return AVPROBE_SCORE_MAX;
        }
    }

    return 0;
}

static int av3a_read_header(AVFormatContext *s)
{
    uint8_t header[MAX_NBYTES_FRAME_HEADER];
    AVIOContext *pb = s->pb; // set by avformat_open_input.
    AVStream *stream = NULL;
    AV3ADecContext *av3adecCtx = s->priv_data;
    AVDictionary *extra_language_metada = NULL;
    AVS3AHeaderInfo hdf;
    int ret = 0;
    AV3ASpecificContext av3aSpecificCtx;

    // create input stream(AVStream) in AVFormatContext->streams = stream.
    if (!(stream = avformat_new_stream(s, NULL)))
    {
        return AVERROR(ENOMEM);
    }

    // set stream constant field
    stream->start_time = 0;
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = s->iformat->raw_codec_id;   // id = AV_CODEC_ID_AVS3_AUDIO
    stream->codecpar->codec_tag = MKTAG('a', 'v', '3', 'a'); // Tag = 'av3a'
    stream->need_parsing = AVSTREAM_PARSE_FULL_RAW;

    // set language metadata
    av_dict_set(&extra_language_metada, "language", "eng", 0);
    stream->metadata = extra_language_metada;

    // read header with max header size
    avio_read(pb, header, MAX_NBYTES_FRAME_HEADER);

    // sparse frame header bits
    if ((ret = read_av3a_frame_header(&hdf, header, MAX_NBYTES_FRAME_HEADER)) != 0)
    {
        return ret;
    }

    // codec parameters
    stream->codecpar->format = hdf.bitdepth;
    stream->codecpar->bit_rate = hdf.total_bitrate;
    stream->codecpar->sample_rate = (int)(hdf.sample_rate);
    stream->codecpar->frame_size = AVS3_AUDIO_FRAME_SIZE; // fixed frame length
    stream->codecpar->channels = hdf.total_channels;
    stream->codecpar->channel_layout = hdf.channel_layout;
    stream->codecpar->bits_per_raw_sample = hdf.resolution;

    // configuration parameters
    av3aSpecificCtx.audio_codec_id = hdf.codec_id;
    av3aSpecificCtx.sample_rate_index = hdf.sample_rate_index;
    av3aSpecificCtx.nn_type = hdf.nn_type;
    av3aSpecificCtx.content_type = hdf.content_type;
    av3aSpecificCtx.channel_num_index = hdf.channel_num_index;
    av3aSpecificCtx.num_objects = hdf.objects;
    av3aSpecificCtx.hoa_order = hdf.hoa_order;
    av3aSpecificCtx.resolution_index = hdf.resolution_index;
    av3aSpecificCtx.total_bitrate_kbps = (int)(hdf.total_bitrate / 1000);

    if (!stream->codecpar->extradata)
    {
        stream->codecpar->extradata_size = sizeof(AV3ASpecificContext);
        stream->codecpar->extradata = av_malloc(stream->codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!stream->codecpar->extradata)
        {
            return AVERROR(ENOMEM);
        }
        memcpy(stream->codecpar->extradata, &av3aSpecificCtx, sizeof(AV3ASpecificContext));
    }

    av3adecCtx->file_size = 0L;
    av3adecCtx->frame_cnt = 0L;

    // seek back to bitstream start offset.
    avio_seek(s->pb, -MAX_NBYTES_FRAME_HEADER, SEEK_CUR);

    return 0;
}

static int av3a_read_packet(AVFormatContext *s, AVPacket *pkt)
{

    int64_t pos;
    int packet_size = 0;
    int read_bytes = 0;
    AV3ADecContext *av3adecCtx = s->priv_data;

    if (!s->streams[0]->codecpar)
    {
        return AVERROR(ENOMEM);
    }

    if (avio_feof(s->pb))
    {
        return AVERROR_EOF;
    }

    pos = avio_tell(s->pb);

    // get packet size
    if ((packet_size = get_av3a_payload(s)) < 0)
    {
        return packet_size;
    }

    // create packet buffer to encoded frame(AVPacket is public ABI).
    if (packet_size == 0 || av_new_packet(pkt, packet_size))
    {
        return AVERROR(EIO);
    }

    // just audio stream
    pkt->stream_index = 0;
    pkt->pos = pos;
    pkt->duration = s->streams[0]->codecpar->frame_size;
    // pkt->dts;

    if ((read_bytes = avio_read(s->pb, pkt->data, packet_size)) < 0)
    {
        if (read_bytes >= 0 && read_bytes < packet_size)
        {
            pkt->size = read_bytes;
        }
        else
        {

            if (avio_feof(s->pb))
            {
                pkt->size = 0;
                return AVERROR_EOF;
            }
            else
            {
                return AVERROR(EIO);
            }
        }
    }

    if (av3adecCtx->frame_cnt < UINT64_MAX - 1)
    {
        av3adecCtx->frame_cnt++;
        if (av3adecCtx->file_size < UINT64_MAX - 1)
        {
            av3adecCtx->file_size += read_bytes;
        }
    }

    return 0;
}

#if CONFIG_AV3A_DEMUXER
AVInputFormat ff_av3a_demuxer = {
    .name = "av3a",
    .long_name = NULL_IF_CONFIG_SMALL("AVS3-P3 3D Audio/Audio Vivid AATF bitstream demuxer"),
    .raw_codec_id = AV_CODEC_ID_AVS3_AUDIO,
    .priv_data_size = sizeof(AV3ADecContext),
    .read_probe = av3a_probe,
    .read_header = av3a_read_header,
    .read_packet = av3a_read_packet,
    .flags = AVFMT_GENERIC_INDEX,
    .extensions = "av3a",
    .mime_type = "audio/av3a",
};
#endif
