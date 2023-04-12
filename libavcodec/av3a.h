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
#include <stdint.h>

/* AVS3 header */
#define AVS3_AUDIO_HEADER_SIZE 7
#define AVS3_SYNC_WORD_SIZE 2
#define MAX_NBYTES_FRAME_HEADER 9
#define AVS3_AUDIO_SYNC_WORD 0xFFF

#define AVS3_AUDIO_FRAME_SIZE 1024
#define AVS3_SIZE_BITRATE_TABLE 16
#define AVS3_SIZE_FS_TABLE 9

/* AVS3 Audio Format */
#define AVS3_MONO_FORMAT 0
#define AVS3_STEREO_FORMAT 1
#define AVS3_MC_FORMAT 2
#define AVS3_HOA_FORMAT 3
#define AVS3_MIX_FORMAT 4

#define AVS3_SIZE_MC_CONFIG_TABLE 10

#define AVS3P3_CH_LAYOUT_5POINT1 (AV_CH_LAYOUT_SURROUND | AV_CH_LOW_FREQUENCY | AV_CH_BACK_LEFT | AV_CH_BACK_RIGHT)

/* Sampling rate index table */
extern const int avs3_samplingrate_table[AVS3_SIZE_FS_TABLE];

// AVS3P3 header information
typedef struct
{
    // header info
    uint8_t codec_id;
    //uint8_t sampling_rate_index;
    //int sampling_rate;
    uint8_t sample_rate_index;
    int sample_rate;
    uint16_t bitdepth;
    uint16_t channels;
    uint16_t objects;
    uint16_t hoa_order;
    uint64_t channel_layout;
    int64_t total_bitrate;

    // configuration
    uint8_t content_type;
    uint16_t channel_num_index;
    uint16_t total_channels;
    uint8_t resolution;
    uint8_t nn_type;
    uint8_t resolution_index;

} AVS3AHeaderInfo;

typedef enum
{
    CHANNEL_CONFIG_MONO = 0,
    CHANNEL_CONFIG_STEREO = 1,
    CHANNEL_CONFIG_MC_5_1,
    CHANNEL_CONFIG_MC_7_1,
    CHANNEL_CONFIG_MC_10_2,
    CHANNEL_CONFIG_MC_22_2,
    CHANNEL_CONFIG_MC_4_0,
    CHANNEL_CONFIG_MC_5_1_2,
    CHANNEL_CONFIG_MC_5_1_4,
    CHANNEL_CONFIG_MC_7_1_2,
    CHANNEL_CONFIG_MC_7_1_4,
    CHANNEL_CONFIG_HOA_ORDER1,
    CHANNEL_CONFIG_HOA_ORDER2,
    CHANNEL_CONFIG_HOA_ORDER3,
    CHANNEL_CONFIG_UNKNOWN
} AVS3AChannelConfig;

/* Codec bitrate config struct */
typedef struct CodecBitrateConfigStructure
{
    AVS3AChannelConfig channelNumConfig;
    const int64_t *bitrateTable;
} CodecBitrateConfig;

typedef struct McChannelConfigStructure
{
    const char mcCmdString[10];
    AVS3AChannelConfig channelNumConfig;
    const int16_t numChannels;
} McChanelConfig;

extern const McChanelConfig mcChannelConfigTable[AVS3_SIZE_MC_CONFIG_TABLE];

/* Codec bitrate config struct */
extern const CodecBitrateConfig codecBitrateConfigTable[CHANNEL_CONFIG_UNKNOWN];

/* bitrate table for each mode */
extern const int64_t bitrateTableMono[AVS3_SIZE_BITRATE_TABLE];
extern const int64_t bitrateTableStereo[AVS3_SIZE_BITRATE_TABLE];
extern const int64_t bitrateTableMC5P1[AVS3_SIZE_BITRATE_TABLE];
extern const int64_t bitrateTableMC7P1[AVS3_SIZE_BITRATE_TABLE];
extern const int64_t bitrateTableMC4P0[AVS3_SIZE_BITRATE_TABLE];
extern const int64_t bitrateTableMC5P1P2[AVS3_SIZE_BITRATE_TABLE];
extern const int64_t bitrateTableMC5P1P4[AVS3_SIZE_BITRATE_TABLE];
extern const int64_t bitrateTableMC7P1P2[AVS3_SIZE_BITRATE_TABLE];
extern const int64_t bitrateTableMC7P1P4[AVS3_SIZE_BITRATE_TABLE];
extern const int64_t bitrateTableFoa[AVS3_SIZE_BITRATE_TABLE];
extern const int64_t bitrateTableHoa2[AVS3_SIZE_BITRATE_TABLE];
extern const int64_t bitrateTableHoa3[AVS3_SIZE_BITRATE_TABLE];

extern uint16_t read_av3a_frame_header(AVS3AHeaderInfo *hdf, const uint8_t *buf, const int byte_size);
