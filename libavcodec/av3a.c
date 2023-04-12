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
#include <stdio.h>
#include "get_bits.h"
#include "libavutil/channel_layout.h"
#include "libavutil/samplefmt.h"
#include "av3a.h"
const McChanelConfig mcChannelConfigTable[AVS3_SIZE_MC_CONFIG_TABLE] = {
    {"STEREO", CHANNEL_CONFIG_STEREO, 2},
    {"MC_5_1_0", CHANNEL_CONFIG_MC_5_1, 6},
    {"MC_7_1_0", CHANNEL_CONFIG_MC_7_1, 8},
    {"MC_10_2", CHANNEL_CONFIG_MC_10_2, 12},
    {"MC_22_2", CHANNEL_CONFIG_MC_22_2, 24},
    {"MC_4_0", CHANNEL_CONFIG_MC_4_0, 4},
    {"MC_5_1_2", CHANNEL_CONFIG_MC_5_1_2, 8},
    {"MC_5_1_4", CHANNEL_CONFIG_MC_5_1_4, 10},
    {"MC_7_1_2", CHANNEL_CONFIG_MC_7_1_2, 10},
    {"MC_7_1_4", CHANNEL_CONFIG_MC_7_1_4, 12}};

const int avs3_samplingrate_table[AVS3_SIZE_FS_TABLE] = {
    192000, 96000, 48000, 44100, 32000, 24000, 22050, 16000, 8000};

// bitrate table for mono
const int64_t bitrateTableMono[AVS3_SIZE_BITRATE_TABLE] = {
    16000, 32000, 44000, 56000, 64000, 72000, 80000, 96000, 128000, 144000,
    164000, 192000, 0, 0, 0, 0};

// bitrate table for stereo
const int64_t bitrateTableStereo[AVS3_SIZE_BITRATE_TABLE] = {
    24000, 32000, 48000, 64000, 80000, 96000, 128000, 144000, 192000, 256000,
    320000, 0, 0, 0, 0, 0};

// bitrate table for MC 5.1
const int64_t bitrateTableMC5P1[AVS3_SIZE_BITRATE_TABLE] = {
    192000, 256000, 320000, 384000, 448000, 512000, 640000, 720000, 144000, 96000,
    128000, 160000, 0, 0, 0, 0};

// bitrate table for MC 7.1
const int64_t bitrateTableMC7P1[AVS3_SIZE_BITRATE_TABLE] = {
    192000, 480000, 256000, 384000, 576000, 640000, 128000, 160000, 0, 0,
    0, 0, 0, 0, 0, 0};

// bitrate table for MC 4.0
const int64_t bitrateTableMC4P0[AVS3_SIZE_BITRATE_TABLE] = {
    48000, 96000, 128000, 192000, 256000, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0};

// bitrate table for MC 5.1.2
const int64_t bitrateTableMC5P1P2[AVS3_SIZE_BITRATE_TABLE] = {
    152000, 320000, 480000, 576000, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0};

// bitrate table for MC 5.1.4
const int64_t bitrateTableMC5P1P4[AVS3_SIZE_BITRATE_TABLE] = {
    176000, 384000, 576000, 704000, 256000, 448000, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0};

// bitrate table for MC 7.1.2
const int64_t bitrateTableMC7P1P2[AVS3_SIZE_BITRATE_TABLE] = {
    216000, 480000, 576000, 384000, 768000, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0};

// bitrate table for MC 7.1.4
const int64_t bitrateTableMC7P1P4[AVS3_SIZE_BITRATE_TABLE] = {
    240000, 608000, 384000, 512000, 832000, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0};

const int64_t bitrateTableFoa[AVS3_SIZE_BITRATE_TABLE] = {
    48000, 96000, 128000, 192000, 256000, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0};

const int64_t bitrateTableHoa2[AVS3_SIZE_BITRATE_TABLE] = {
    192000, 256000, 320000, 384000, 480000, 512000, 640000, 0, 0, 0,
    0, 0, 0, 0, 0, 0};

// bitrate table for HOA order 3
const int64_t bitrateTableHoa3[AVS3_SIZE_BITRATE_TABLE] = {
    256000, 320000, 384000, 512000, 640000, 896000, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0};

// Codec channel number & bitrate config table
// format: {channelConfigIdx, numChannels, bitrateTable}
const CodecBitrateConfig codecBitrateConfigTable[CHANNEL_CONFIG_UNKNOWN] = {
    {CHANNEL_CONFIG_MONO, bitrateTableMono},
    {CHANNEL_CONFIG_STEREO, bitrateTableStereo},
    {CHANNEL_CONFIG_MC_5_1, bitrateTableMC5P1},
    {CHANNEL_CONFIG_MC_7_1, bitrateTableMC7P1},
    {CHANNEL_CONFIG_MC_10_2, NULL},
    {CHANNEL_CONFIG_MC_22_2, NULL},
    {CHANNEL_CONFIG_MC_4_0, bitrateTableMC4P0},
    {CHANNEL_CONFIG_MC_5_1_2, bitrateTableMC5P1P2},
    {CHANNEL_CONFIG_MC_5_1_4, bitrateTableMC5P1P4},
    {CHANNEL_CONFIG_MC_7_1_2, bitrateTableMC7P1P2},
    {CHANNEL_CONFIG_MC_7_1_4, bitrateTableMC7P1P4},
    {CHANNEL_CONFIG_HOA_ORDER1, bitrateTableFoa},
    {CHANNEL_CONFIG_HOA_ORDER2, bitrateTableHoa2},
    {CHANNEL_CONFIG_HOA_ORDER3, bitrateTableHoa3}};

uint16_t read_av3a_frame_header(AVS3AHeaderInfo *hdf, const uint8_t *buf, const int byte_size){

    GetBitContext gb;

    uint8_t nn_type, codec_id, content_type, stamping_rate_index, hoa_order, resolution_index, bitdepth, resolution;
    AVS3AChannelConfig channel_config;

    int16_t channels, objects;
    uint64_t channel_layout;
    int64_t bitratePerObj, bitrateBedMc, total_bitrate;
    uint8_t coding_profile, num_chan_index, obj_brt_idx, bed_brt_idx, brt_idx;

    // Read max header length into bs buffer
    init_get_bits8(&gb, buf, MAX_NBYTES_FRAME_HEADER);

    // 12 bits for frame sync word
    if (get_bits(&gb, 12) != AVS3_AUDIO_SYNC_WORD) {
        return AVERROR_INVALIDDATA;
    }

    // 4 bits for codec id
    codec_id = get_bits(&gb, 4);
    if (codec_id != 2) {
        return AVERROR_INVALIDDATA;
    }

    // 1 bits for anc data
    if (get_bits(&gb, 1) != 0) {
        return AVERROR_INVALIDDATA;
    }

    // 3 bits nn type
    nn_type = get_bits(&gb, 3);

    // 3 bits for coding profile
    coding_profile = get_bits(&gb, 3);

    // 4 bits for sampling index
    stamping_rate_index = get_bits(&gb, 4);

    // skip 8 bits for CRC first part
    skip_bits(&gb, 8);

    if (coding_profile == 0) {

        content_type = 0;

        // 7 bits for mono/stereo/MC
        num_chan_index = get_bits(&gb, 7);

        channel_config = (AVS3AChannelConfig)num_chan_index;

        switch (channel_config)
        {
        case CHANNEL_CONFIG_MONO:
            channels = 1;
            channel_layout = CHANNEL_CONFIG_MONO;
            break;
        case CHANNEL_CONFIG_STEREO:
            channels = 2;
            channel_layout = CHANNEL_CONFIG_STEREO;
            break;
        case CHANNEL_CONFIG_MC_4_0:
            channels = 4;
            channel_layout = CHANNEL_CONFIG_MC_4_0;
            break;
        case CHANNEL_CONFIG_MC_5_1:
            channels = 6;
            channel_layout = CHANNEL_CONFIG_MC_5_1;
            break;
        case CHANNEL_CONFIG_MC_7_1:
            channels = 8;
            channel_layout = CHANNEL_CONFIG_MC_7_1;
            break;
        case CHANNEL_CONFIG_MC_5_1_2:
            channels = 8;
            channel_layout = CHANNEL_CONFIG_MC_5_1_2;
            break;
        case CHANNEL_CONFIG_MC_5_1_4:
            channels = 10;
            channel_layout = CHANNEL_CONFIG_MC_5_1_4;
            break;
        case CHANNEL_CONFIG_MC_7_1_2:
            channels = 10;
            channel_layout = CHANNEL_CONFIG_MC_7_1_2;
            break;
        case CHANNEL_CONFIG_MC_7_1_4:
            channels = 12;
            channel_layout = CHANNEL_CONFIG_MC_7_1_4;
            break;
        case CHANNEL_CONFIG_MC_22_2:
            channels = 24;
            channel_layout = CHANNEL_CONFIG_MC_22_2;
            break;
        default:
            break;
        }
    }
    else if (coding_profile == 1) {

        // sound bed type, 2bits
        uint8_t soundBedType = get_bits(&gb, 2);

        if (soundBedType == 0) {

            content_type = 1;

            // for only objects
            // object number, 7 bits
            objects = get_bits(&gb, 7);
            objects += 1;

            // bitrate index for each obj, 4 bits
            obj_brt_idx = get_bits(&gb, 4);

            total_bitrate = codecBitrateConfigTable[CHANNEL_CONFIG_MONO].bitrateTable[obj_brt_idx] * objects;
        }
        else if (soundBedType == 1) {

            content_type = 2;

            // for MC + objs
            // channel number index, 7 bits
            num_chan_index = get_bits(&gb, 7);

            // bitrate index for sound bed, 4 bits
            bed_brt_idx = get_bits(&gb, 4);

            // object number, 7 bits
            objects = get_bits(&gb, 7);
            objects += 1;

            // bitrate index for each obj, 4 bits
            obj_brt_idx = get_bits(&gb, 4);

            // channelNumIdx for sound bed
            channel_config = (AVS3AChannelConfig)num_chan_index;

            // sound bed bitrate
            bitrateBedMc = codecBitrateConfigTable[channel_config].bitrateTable[bed_brt_idx];

            // numChannels for sound bed
            for (int16_t i = 0; i < AVS3_SIZE_MC_CONFIG_TABLE; i++)
            {
                if (channel_config == mcChannelConfigTable[i].channelNumConfig)
                {
                    channels = mcChannelConfigTable[i].numChannels;
                }
            }

            // bitrate per obj
            bitratePerObj = codecBitrateConfigTable[CHANNEL_CONFIG_MONO].bitrateTable[obj_brt_idx];

            // add num chans and num objs to get total chans
            /* channels += objects; */

            total_bitrate = bitrateBedMc + bitratePerObj * objects;
        }
    }
    else if (coding_profile == 2) {
        content_type = 3;

        // 4 bits for HOA order
        hoa_order = get_bits(&gb, 4);
        hoa_order += 1;

        switch (hoa_order)
        {
        case 1:
            channels = 4;
            channel_config = CHANNEL_CONFIG_HOA_ORDER1;
            break;
        case 2:
            channels = 9;
            channel_config = CHANNEL_CONFIG_HOA_ORDER2;
            break;
        case 3:
            channels = 16;
            channel_config = CHANNEL_CONFIG_HOA_ORDER3;
            break;
        default:
            break;
        }
    }else{
        return AVERROR_INVALIDDATA;
    }

    // 2 bits for bit depth
    resolution_index = get_bits(&gb, 2);
    switch (resolution_index) {
    case 0:
        bitdepth = AV_SAMPLE_FMT_U8;
        resolution = 8;
        break;
    case 1:
        bitdepth = AV_SAMPLE_FMT_S16;
        resolution = 16;
        break;
    case 2:
        resolution = 24;
        break;
    default:
        return AVERROR_INVALIDDATA;
    }

    if (coding_profile != 1) {
        // 4 bits for bitrate index
        brt_idx = get_bits(&gb, 4);
        total_bitrate = codecBitrateConfigTable[channel_config].bitrateTable[brt_idx];
    }

    // 8 bits for CRC second part
    skip_bits(&gb, 8);

    /* AVS3P6 M6954-v3 */
    hdf->codec_id = codec_id;
    hdf->sample_rate_index =stamping_rate_index;
    hdf->sample_rate = avs3_samplingrate_table[stamping_rate_index];
    hdf->bitdepth = bitdepth;

    hdf->nn_type = nn_type;
    hdf->content_type = content_type;

    if (hdf->content_type == 0) {
        hdf->channel_num_index = num_chan_index;
        hdf->channels = channels;
        hdf->objects = 0;
        hdf->total_channels = channels;
        hdf->channel_layout = channel_layout;
    }else if(hdf->content_type == 1) {
        hdf->objects = objects;
        hdf->channels = 0;
        hdf->total_channels = objects;
        hdf->channel_layout = CHANNEL_CONFIG_UNKNOWN;
    }else if(hdf->content_type == 2) {
        hdf->channel_num_index = num_chan_index;
        hdf->channels = channels;
        hdf->objects = objects;
        hdf->total_channels = channels + objects;
        //hdf->channel_layout = channel_layout;
        hdf->channel_layout = CHANNEL_CONFIG_UNKNOWN;
    }else if(hdf->content_type == 3) {
        hdf->hoa_order = hoa_order;
        hdf->channels = channels;
        hdf->total_channels = channels;
        hdf->channel_layout = CHANNEL_CONFIG_UNKNOWN;
    }else{
        return AVERROR_INVALIDDATA;
    }

    hdf->total_bitrate = total_bitrate;
    hdf->resolution = resolution;
    hdf->resolution_index = resolution_index;

    return 0;
}
