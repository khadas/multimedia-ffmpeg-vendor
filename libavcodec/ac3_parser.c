/*
 * AC-3 parser
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2003 Michael Niedermayer
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

#include "libavutil/channel_layout.h"
#include "parser.h"
#include "ac3_parser.h"
#include "aac_ac3_parser.h"
#include "get_bits.h"


#define AC3_HEADER_SIZE 7


static const uint8_t eac3_blocks[4] = {
    1, 2, 3, 6
};

/**
 * Table for center mix levels
 * reference: Section 5.4.2.4 cmixlev
 */
static const uint8_t center_levels[4] = { 4, 5, 6, 5 };

/**
 * Table for surround mix levels
 * reference: Section 5.4.2.5 surmixlev
 */
static const uint8_t surround_levels[4] = { 4, 6, 7, 6 };

static int32_t readVariableBits(GetBitContext *gbc, int32_t nbits) {
    int32_t value = 0;
    int32_t more_bits = 1;

    while (more_bits) {
        value += get_bits(gbc,nbits);
        more_bits = get_bits(gbc,1);
        if (!more_bits)
            break;
        value++;
        value <<= nbits;
    }
    return value;
}


int avpriv_ac3_parse_header(GetBitContext *gbc, AC3HeaderInfo **phdr)
{
    int frame_size_code;
    AC3HeaderInfo *hdr;

    if (!*phdr)
        *phdr = av_mallocz(sizeof(AC3HeaderInfo));
    if (!*phdr)
        return AVERROR(ENOMEM);
    hdr = *phdr;

    memset(hdr, 0, sizeof(*hdr));

    hdr->sync_word = get_bits(gbc, 16);
    if(hdr->sync_word != 0x0B77 &&
       hdr->sync_word != 0xAC40 &&
       hdr->sync_word != 0xAC41)
        return AAC_AC3_PARSE_ERROR_SYNC;

    if (hdr->sync_word == 0xAC40 ||
       hdr->sync_word == 0xAC41) {
       hdr->is_ac4 = 1;
    } else {
       hdr->is_ac4 = 0;
    }

    if (hdr->is_ac4) {
        hdr->frame_size = get_bits(gbc, 16);
        if (hdr->frame_size == 0xFFFF) {
            hdr->frame_size = get_bits(gbc, 24);
        }
        if (hdr->frame_size == 0) {
            av_log(NULL,AV_LOG_ERROR,"Invalid frame size in AC4 header.");
            return AAC_AC3_PARSE_ERROR_FRAME_SIZE;
        }
        if (hdr->sync_word == 0xAC41) {
            // If the sync_word is 0xAC41, a crc_word is also transmitted.
            hdr->frame_size += 2;
        }
        // ETSI TS 103 190-2 V1.1.1 6.2.1.1
        uint32_t bitstreamVersion = get_bits(gbc, 2);
        if (bitstreamVersion == 3) {
            bitstreamVersion += readVariableBits(gbc, 2);
            av_log(NULL,AV_LOG_ERROR,"bitstreamVersion : %u.",bitstreamVersion);
        }

        // ETSI TS 103 190 V1.1.1 Table 82
        int fsIndex = get_bits(gbc,1);
        uint32_t samplingRate = fsIndex ? 48000 : 44100;

        // ETSI TS 103 190 V1.1.1 Table 83
        static uint32_t frame_rate_table_48Khz[16] = {
            23976,
            24000,
            25000,
            29970,
            30000,
            47950,
            48000,
            50000,
            59940,
            60000,
            100000,
            119880,
            120000,
            23440,
            0,
            0,
        };
        // ETSI TS 103 190 V1.1.1 Table 84
        static uint32_t frame_rate_table_44Khz[16] = {
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            21533,  /*11025/512*/
            0,
            0,
        };
        uint32_t frame_rate_index = 0;
        frame_rate_index = get_bits(gbc, 4);
        if (fsIndex) {
            hdr->frame_rate = frame_rate_table_48Khz[frame_rate_index];
        } else {
            hdr->frame_rate = frame_rate_table_44Khz[frame_rate_index];
        }

        hdr->channels = 2;
        hdr->sample_rate = samplingRate;
        av_log(NULL,AV_LOG_TRACE,"[%s:%d] channels:%d, sample_rate:%d, frame_rate:%u.",__FUNCTION__,__LINE__,hdr->channels,hdr->sample_rate,hdr->frame_rate);
        return 0;
    }

    /* read ahead to bsid to distinguish between AC-3 and E-AC-3 */
    hdr->bitstream_id = show_bits_long(gbc, 29) & 0x1F;
    if(hdr->bitstream_id > 16)
        return AAC_AC3_PARSE_ERROR_BSID;

    hdr->num_blocks = 6;

    /* set default mix levels */
    hdr->center_mix_level   = 5;  // -4.5dB
    hdr->surround_mix_level = 6;  // -6.0dB

    /* set default dolby surround mode */
    hdr->dolby_surround_mode = AC3_DSURMOD_NOTINDICATED;

    if(hdr->bitstream_id <= 10) {
        /* Normal AC-3 */
        hdr->crc1 = get_bits(gbc, 16);
        hdr->sr_code = get_bits(gbc, 2);
        if(hdr->sr_code == 3)
            return AAC_AC3_PARSE_ERROR_SAMPLE_RATE;

        frame_size_code = get_bits(gbc, 6);
        if(frame_size_code > 37)
            return AAC_AC3_PARSE_ERROR_FRAME_SIZE;

        skip_bits(gbc, 5); // skip bsid, already got it

        hdr->bitstream_mode = get_bits(gbc, 3);
        hdr->channel_mode = get_bits(gbc, 3);

        if(hdr->channel_mode == AC3_CHMODE_STEREO) {
            hdr->dolby_surround_mode = get_bits(gbc, 2);
        } else {
            if((hdr->channel_mode & 1) && hdr->channel_mode != AC3_CHMODE_MONO)
                hdr->  center_mix_level =   center_levels[get_bits(gbc, 2)];
            if(hdr->channel_mode & 4)
                hdr->surround_mix_level = surround_levels[get_bits(gbc, 2)];
        }
        hdr->lfe_on = get_bits1(gbc);

        hdr->sr_shift = FFMAX(hdr->bitstream_id, 8) - 8;
        hdr->sample_rate = ff_ac3_sample_rate_tab[hdr->sr_code] >> hdr->sr_shift;
        hdr->bit_rate = (ff_ac3_bitrate_tab[frame_size_code>>1] * 1000) >> hdr->sr_shift;
        hdr->channels = ff_ac3_channels_tab[hdr->channel_mode] + hdr->lfe_on;
        hdr->frame_size = ff_ac3_frame_size_tab[frame_size_code][hdr->sr_code] * 2;
        hdr->frame_type = EAC3_FRAME_TYPE_AC3_CONVERT; //EAC3_FRAME_TYPE_INDEPENDENT;
        hdr->substreamid = 0;
    } else {
        /* Enhanced AC-3 */
        hdr->crc1 = 0;
        hdr->frame_type = get_bits(gbc, 2);
        if(hdr->frame_type == EAC3_FRAME_TYPE_RESERVED)
            return AAC_AC3_PARSE_ERROR_FRAME_TYPE;

        hdr->substreamid = get_bits(gbc, 3);

        hdr->frame_size = (get_bits(gbc, 11) + 1) << 1;
        if(hdr->frame_size < AC3_HEADER_SIZE)
            return AAC_AC3_PARSE_ERROR_FRAME_SIZE;

        hdr->sr_code = get_bits(gbc, 2);
        if (hdr->sr_code == 3) {
            int sr_code2 = get_bits(gbc, 2);
            if(sr_code2 == 3)
                return AAC_AC3_PARSE_ERROR_SAMPLE_RATE;
            hdr->sample_rate = ff_ac3_sample_rate_tab[sr_code2] / 2;
            hdr->sr_shift = 1;
        } else {
            hdr->num_blocks = eac3_blocks[get_bits(gbc, 2)];
            hdr->sample_rate = ff_ac3_sample_rate_tab[hdr->sr_code];
            hdr->sr_shift = 0;
        }

        hdr->channel_mode = get_bits(gbc, 3);
        hdr->lfe_on = get_bits1(gbc);

        hdr->bit_rate = 8LL * hdr->frame_size * hdr->sample_rate /
                        (hdr->num_blocks * 256);
        hdr->channels = ff_ac3_channels_tab[hdr->channel_mode] + hdr->lfe_on;
    }
    hdr->channel_layout = avpriv_ac3_channel_layout_tab[hdr->channel_mode];
    if (hdr->lfe_on)
        hdr->channel_layout |= AV_CH_LOW_FREQUENCY;

    return 0;
}

static int ac3_sync(uint64_t state, AACAC3ParseContext *hdr_info,
        int *need_next_header, int *new_frame_start)
{
    int err;
    union {
        uint64_t u64;
        uint8_t  u8[8 + AV_INPUT_BUFFER_PADDING_SIZE];
    } tmp = { av_be2ne64(state) };
    AC3HeaderInfo hdr, *phdr = &hdr;
    GetBitContext gbc;

    init_get_bits(&gbc, tmp.u8+8-AC3_HEADER_SIZE, 54);
    err = avpriv_ac3_parse_header(&gbc, &phdr);

    if(err < 0)
        return 0;

    hdr_info->sample_rate = hdr.sample_rate;
    hdr_info->bit_rate = hdr.bit_rate;
    hdr_info->channels = hdr.channels;
    hdr_info->channel_layout = hdr.channel_layout;
    hdr_info->samples = hdr.num_blocks * 256;
    hdr_info->service_type = hdr.bitstream_mode;
    if (hdr.bitstream_mode == 0x7 && hdr.channels > 1)
        hdr_info->service_type = AV_AUDIO_SERVICE_TYPE_KARAOKE;
    if (hdr.is_ac4) {
        hdr_info->codec_id = AV_CODEC_ID_AC4;
    } else
    if(hdr.bitstream_id>10)
        hdr_info->codec_id = AV_CODEC_ID_EAC3;
    else if (hdr_info->codec_id == AV_CODEC_ID_NONE)
        hdr_info->codec_id = AV_CODEC_ID_AC3;

    *need_next_header = (hdr.frame_type != EAC3_FRAME_TYPE_AC3_CONVERT);
    *new_frame_start  = (hdr.frame_type != EAC3_FRAME_TYPE_DEPENDENT);
    return hdr.frame_size;
}

static av_cold int ac3_parse_init(AVCodecParserContext *s1)
{
    AACAC3ParseContext *s = s1->priv_data;
    s->header_size = AC3_HEADER_SIZE;
    s->sync = ac3_sync;
    return 0;
}


AVCodecParser ff_ac3_parser = {
    .codec_ids      = { AV_CODEC_ID_AC3, AV_CODEC_ID_EAC3, AV_CODEC_ID_AC4},
    .priv_data_size = sizeof(AACAC3ParseContext),
    .parser_init    = ac3_parse_init,
    .parser_parse   = ff_aac_ac3_parse,
    .parser_close   = ff_parse_close,
};
