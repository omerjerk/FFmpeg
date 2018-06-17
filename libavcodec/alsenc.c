/*
 * MPEG-4 ALS encoder
 * Copyright (c) 2010 Thilo Borgmann <thilo.borgmann _at_ mail.de>
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

/**
 * @file libavcodec/alsenc.c
 * MPEG-4 ALS encoder
 * @author Thilo Borgmann <thilo.borgmann _at_ mail.de>
 * @author Justin Ruggles <justin.ruggles@gmail.com>
 */


#define DEBUG


#include "als.h"
#include "als_data.h"

#include "avcodec.h"
#include "put_bits.h"
#define LPC_USE_DOUBLE
#include "lpc.h"
#include "mpeg4audio.h"
#include "bgmc.h"
#include "window.h"
#include "internal.h"
#include "libavutil/crc.h"
#include "libavutil/lls.h"
#include "libavutil/samplefmt.h"
#include "libswresample/audioconvert.h"


/** Total size of fixed-size fields in ALSSpecificConfig */
#define ALS_SPECIFIC_CFG_SIZE  30


/** Maximum number of blocks in a frame */
#define ALS_MAX_BLOCKS  32

/** Maximum lag value for LTP */
#define ALS_MAX_LTP_LAG 2048


/** Total number of stages used for allocation */
#define NUM_STAGES 3

/** Give the different stages used for encoding a readable name */
#define STAGE_JOINT_STEREO     0
#define STAGE_BLOCK_SWITCHING  1
#define STAGE_FINAL            2


/** Sets the current stage pointer in the context to the desired stage
 *  and writes all overriding options into specific config */
#define SET_OPTIONS(stage)                        \
{                                                 \
    ctx->cur_stage = ctx->stages + (stage);       \
}


/** Determines entropy coding partitioning level using estimated Rice coding bit counts */
#define EC_SUB_ALGORITHM_RICE_ESTIMATE      0
/** Determines entropy coding partitioning level using exact Rice coding bit counts */
#define EC_SUB_ALGORITHM_RICE_EXACT         1
/** Determines entropy coding partitioning using exact BGMC coding bit counts */
#define EC_SUB_ALGORITHM_BGMC_EXACT         2

/** Estimates Rice parameters using sum of unsigned residual samples */
#define EC_PARAM_ALGORITHM_RICE_ESTIMATE    0
/** Calculates Rice parameters using a search algorithm based on exact bit count */
#define EC_PARAM_ALGORITHM_RICE_EXACT       1
/** Estimates BGMC parameters using mean of unsigned residual samples */
#define EC_PARAM_ALGORITHM_BGMC_ESTIMATE    2
/** Calculates BGMC parameters using a search algorithm based on exact bit count */
#define EC_PARAM_ALGORITHM_BGMC_EXACT       3

/** Uses estimate for returned entropy coding bit count */
#define EC_BIT_COUNT_ALGORITHM_ESTIMATE     0
/** Uses exact calculation for returned entropy coding bit count */
#define EC_BIT_COUNT_ALGORITHM_EXACT        1

/** Find adaptive LPC order by doing a bit count for each order until a probable low point is detected */
#define ADAPT_SEARCH_ALGORITHM_VALLEY_DETECT    0
/** Find adaptive LPC order by doing a bit count for each possible order */
#define ADAPT_SEARCH_ALGORITHM_FULL             1

/**  Estimate bit count during adaptive LPC order search using error from the PARCOR coeff calculations */
#define ADAPT_COUNT_ALGORITHM_ESTIMATE      0
/**  Do an exact bit count during adaptive LPC order search */
#define ADAPT_COUNT_ALGORITHM_EXACT         1

/** Use fixed values for LTP coefficients */
#define LTP_COEFF_ALGORITHM_FIXED           0
/** Calculate LTP coefficients using Cholesky factorization */
#define LTP_COEFF_ALGORITHM_CHOLESKY        1

/** Find optimal block partitioning using Full-Search */
#define BS_ALGORITHM_FULL_SEARCH    1
/** Find optimal block partitioning using Bottom-Up */
#define BS_ALGORITHM_BOTTOM_UP      0


/** Get the bit at position pos+1 in uint32_t *ptr_bs_info */
#define GET_BS_BIT(ptr_bs_info, pos) ((*ptr_bs_info & (1 << (30 - pos))) > 0)


#define OVERFLOW_PROTECT(pb, bits, ERROR)                   \
{                                                           \
    if (put_bits_count(pb) + (bits) > (pb)->size_in_bits) { \
        ERROR                                               \
    }                                                       \
}

#define PUT_BITS_SAFE(pb, bits, val, ERROR)     \
{                                               \
    OVERFLOW_PROTECT(pb, bits, ERROR)           \
    put_bits(pb, bits, val);                    \
}


/** grouped encoding algorithms and options */
typedef struct {
    // encoding options used during the processing of the stage
    int check_constant;             ///< check for constant sample value during this stage
    int check_lsbs;                 ///< check for zero LSB values during this stage
    int adapt_order;                ///< use adaptive order search during this stage
    int max_order;                  ///< max_oder to use during this stage
    int sb_part;                    ///< sb_part to use during this stage

    // encoding algorithms used during the processing of the stage
    int ecsub_algorithm;            ///< algorithm to use to determine entropy coding sub-block partitioning
    int param_algorithm;            ///< algorithm to use to determine Rice parameters
    int count_algorithm;            ///< algorithm to use for residual + rice param bit count
    int adapt_search_algorithm;     ///< algorithm to use for adaptive LPC order search
    int adapt_count_algorithm;      ///< algorithm to use for adaptive LPC order bit count
    int ltp_coeff_algorithm;        ///< algorithm to use to determine LTP coefficients
    int merge_algorithm;            ///< algorithm to use to determine final block partitioning
} ALSEncStage;


typedef struct {
    int use_ltp;                    ///< ltp flag
    int lag;                        ///< lag value for long-term prediction
    int gain[5];                    ///< gain values for ltp 5-tap filter
    int bits_ltp;                   ///< bit count for LTP lag, gain and use_ltp flag
} ALSLTPInfo;


typedef struct {
    unsigned int sub_blocks;        ///< number of entropy coding sub-blocks in this block
    unsigned int rice_param[8];     ///< rice parameters to encode the residuals
                                    ///< of this block
    unsigned int bgmc_param[8];     ///< LSB's of estimated Rice parameters in case of BGMC mode
    int bits_ec_param_and_res;      ///< bit count for Rice/BGMC params and residuals
} ALSEntropyInfo;


// probably mergeable or the very same as ALSBlockData from the decoder
typedef struct {
    int ra_block;                   ///< indicates if this is an RA block
    int constant;                   ///< indicates constant block values
    int32_t constant_value;         ///< if constant, this is the value
    unsigned int length;            ///< length of the block in # of samples
    int div_block;                  ///< if > 0, this block length is 1/(1<<div_block) of a full frame
    unsigned int opt_order;         ///< prediction order for this block
    int32_t *q_parcor_coeff;        ///< 7-bit quantized PARCOR coefficients
    unsigned int js_block;          ///< indicates actual usage of joint-stereo coding
    unsigned int shift_lsbs;        ///< number of bits the samples have been right shifted
    ALSLTPInfo ltp_info[2];         ///< one set of LTPInfo for non-js- and js-residuals
    ALSEntropyInfo ent_info[2];     ///< one set of EntropyInfo for non-LTP- and LTP-residuals
    int32_t *ltp_ptr;               ///< points to the first ltp residual for this block
    int32_t *res_ptr;               ///< points to the first residual for this block
    int32_t *smp_ptr;               ///< points to the first raw sample for this block
    int32_t *dif_ptr;               ///< points to the first difference sample for this block
    int32_t *lsb_ptr;               ///< points to the first LSB shifted sample for this block
    int32_t *cur_ptr;               ///< points to the current sample buffer for this block
    int bits_const_block;           ///< bit count for const block params
    int bits_misc;                  ///< bit count for js_block and shift_lsbs
    int bits_adapt_order;           ///< bit count for LPC order when adaptive order is used
    int bits_parcor_coeff[1024];    ///< cumulative bit counts for PARCOR coeffs
} ALSBlock;


typedef struct {
    AVCodecContext *avctx;
    ALSSpecificConfig sconf;
    PutBitContext pb;
    const AVCRC *crc_table;
    uint32_t crc;                   ///< CRC value calculated from decoded data
    ALSEncStage *stages;            ///< array containing all grouped encoding and algorithm options for each possible stage
    ALSEncStage *cur_stage;         ///< points to the currently used encoding stage
    int ra_counter;                 ///< counts from zero to ra_distance, equals zero for ra-frames
    int js_switch;                  ///< force joint-stereo in case of MCC
    int *independent_bs;            ///< array containing independent_bs flag for each channel
    int32_t *raw_buffer;            ///< buffer containing all raw samples of the frame plus max_order samples from the previous frame (or zeroes) for all channels
    int32_t **raw_samples;          ///< pointer to the beginning of the current frame's samples in the buffer for each channel
    int32_t *raw_dif_buffer;        ///< buffer containing all raw difference samples of the frame plus max_order samples from the previous frame (or zeroes) for all channels
    int32_t **raw_dif_samples;      ///< pointer to the beginning of the current frame's difference samples in the buffer for each channel
    int32_t *raw_lsb_buffer;        ///< buffer containing all shifted raw samples of the frame plus max_order samples from the previous frame for all channels
    int32_t **raw_lsb_samples;      ///< pointer to the beginning of the current frame's shifted samples in the buffer for each channel
    int32_t *res_buffer;            ///< buffer containing all residual samples of the frame plus max_order samples from the previous frame (or zeroes) for all channels
    int32_t **res_samples;          ///< pointer to the beginning of the current frame's samples in the buffer for each channel
    uint32_t *bs_info;              ///< block partitioning used for the current frame
    int *num_blocks;                ///< number of blocks used for the block partitioning
    unsigned int *bs_sizes_buffer;  ///< buffer containing all block sizes for all channels
    unsigned int **bs_sizes;        ///< pointer to the beginning of the channel's block sizes for each channel
    unsigned int *js_sizes_buffer;  ///< buffer containing all block sizes for all channel-pairs of the difference signal
    unsigned int **js_sizes;        ///< pointer to the beginning of the channel's block sizes for each channel-pair difference signal
    uint8_t *js_infos_buffer;       ///< buffer containing all joint-stereo flags for all channel-pairs
    uint8_t **js_infos;             ///< pointer to the beginning of the channel's joint-stereo flags for each channel-pair
    ALSBlock *block_buffer;         ///< buffer containing all ALSBlocks for each channel
    ALSBlock **blocks;              ///< array of 32 ALSBlock pointers per channel pointing into the block_buffer
    int32_t *q_parcor_coeff_buffer; ///< buffer containing 7-bit PARCOR coefficients for all blocks in all channels
    double *acf_coeff;              ///< autocorrelation function coefficients for the current block
    double *parcor_coeff;           ///< double-precision PARCOR coefficients for the current block
    int32_t *r_parcor_coeff;        ///< scaled 21-bit quantized PARCOR coefficients for the current block
    int32_t *lpc_coeff;             ///< LPC coefficients for the current block
    double *parcor_error;           ///< error for each order during PARCOR coeff calculation
    unsigned int max_rice_param;    ///< maximum Rice param, depends on sample depth
    WindowContext acf_window[6];    ///< contexts for pre-autocorrelation windows for each block switching depth
    int32_t *ltp_buffer;            ///< temporary buffer to store long-term predicted samples
    int32_t **ltp_samples;          ///< pointer to the beginning of the current frame's ltp residuals in the buffer for each channel
    double *corr_buffer;            ///< temporary buffer to store the signal during LTP autocorrelation
    double *corr_samples;           ///< pointer to the beginning of the block in corr_buffer
    int frame_buffer_size;          ///< size of the frame_buffer in bytes
    LPCContext lpc;

    int flushed;
    int64_t next_pts;
} ALSEncContext;


/** compression level 0 global options **/
static const ALSSpecificConfig spc_config_c0 = {
    .adapt_order            = 0,
    .long_term_prediction   = 0,
    .max_order              = 4,
    .block_switching        = 0,
    .bgmc                   = 0,
    .sb_part                = 0,
    .joint_stereo           = 0,
    .mc_coding              = 0,
    .rlslms                 = 0,
    .crc_enabled            = 0,
};


/** compression level 0 joint-stereo options
    note: compression level 0 does not use joint-stereo */
static const ALSEncStage stage_js_c0 = {
    .check_constant         = 0,
    .check_lsbs             = 0,
    .max_order              = 0,
    .ecsub_algorithm        = EC_SUB_ALGORITHM_RICE_ESTIMATE,
    .param_algorithm        = EC_PARAM_ALGORITHM_RICE_ESTIMATE,
    .count_algorithm        = EC_BIT_COUNT_ALGORITHM_ESTIMATE,
    .adapt_search_algorithm = ADAPT_SEARCH_ALGORITHM_VALLEY_DETECT,
    .adapt_count_algorithm  = ADAPT_COUNT_ALGORITHM_ESTIMATE,
    .ltp_coeff_algorithm    = LTP_COEFF_ALGORITHM_FIXED,
    .merge_algorithm        = BS_ALGORITHM_BOTTOM_UP,
};


/** compression level 0 block switching stage options */
static const ALSEncStage stage_bs_c0 = {
    .check_constant         = 0,
    .check_lsbs             = 0,
    .max_order              = 4,
    .ecsub_algorithm        = EC_SUB_ALGORITHM_RICE_ESTIMATE,
    .param_algorithm        = EC_PARAM_ALGORITHM_RICE_ESTIMATE,
    .count_algorithm        = EC_BIT_COUNT_ALGORITHM_ESTIMATE,
    .adapt_search_algorithm = ADAPT_SEARCH_ALGORITHM_VALLEY_DETECT,
    .adapt_count_algorithm  = ADAPT_COUNT_ALGORITHM_ESTIMATE,
    .ltp_coeff_algorithm    = LTP_COEFF_ALGORITHM_FIXED,
    .merge_algorithm        = BS_ALGORITHM_BOTTOM_UP,
};


/** compression level 0 final stage options */
static const ALSEncStage stage_final_c0 = {
    .check_constant         = 0,
    .check_lsbs             = 0,
    .ecsub_algorithm        = EC_SUB_ALGORITHM_RICE_ESTIMATE,
    .param_algorithm        = EC_PARAM_ALGORITHM_RICE_ESTIMATE,
    .count_algorithm        = EC_BIT_COUNT_ALGORITHM_ESTIMATE,
    .adapt_search_algorithm = ADAPT_SEARCH_ALGORITHM_VALLEY_DETECT,
    .adapt_count_algorithm  = ADAPT_COUNT_ALGORITHM_ESTIMATE,
    .ltp_coeff_algorithm    = LTP_COEFF_ALGORITHM_FIXED,
    .merge_algorithm        = BS_ALGORITHM_BOTTOM_UP,
};


/** compression level 1 global options **/
static const ALSSpecificConfig spc_config_c1 = {
    .adapt_order            = 0,
    .long_term_prediction   = 0,
    .max_order              = 10,
    .block_switching        = 0,
    .bgmc                   = 0,
    .sb_part                = 1,
    .joint_stereo           = 1,
    .mc_coding              = 0,
    .rlslms                 = 0,
    .crc_enabled            = 1,
};


/** compression level 1 joint-stereo stage options */
static const ALSEncStage stage_js_c1 = {
    .check_constant         = 1,
    .check_lsbs             = 1,
    .max_order              = 5,
    .ecsub_algorithm        = EC_SUB_ALGORITHM_RICE_ESTIMATE,
    .param_algorithm        = EC_PARAM_ALGORITHM_RICE_ESTIMATE,
    .count_algorithm        = EC_BIT_COUNT_ALGORITHM_EXACT,
    .adapt_search_algorithm = ADAPT_SEARCH_ALGORITHM_VALLEY_DETECT,
    .adapt_count_algorithm  = ADAPT_COUNT_ALGORITHM_ESTIMATE,
    .ltp_coeff_algorithm    = LTP_COEFF_ALGORITHM_FIXED,
    .merge_algorithm        = BS_ALGORITHM_FULL_SEARCH,
};


/** compression level 1 block switching stage options */
static const ALSEncStage stage_bs_c1 = {
    .check_constant         = 1,
    .check_lsbs             = 1,
    .ecsub_algorithm        = EC_SUB_ALGORITHM_RICE_EXACT,
    .param_algorithm        = EC_PARAM_ALGORITHM_RICE_EXACT,
    .count_algorithm        = EC_BIT_COUNT_ALGORITHM_EXACT,
    .adapt_search_algorithm = ADAPT_SEARCH_ALGORITHM_VALLEY_DETECT,
    .adapt_count_algorithm  = ADAPT_COUNT_ALGORITHM_ESTIMATE,
    .ltp_coeff_algorithm    = LTP_COEFF_ALGORITHM_FIXED,
    .merge_algorithm        = BS_ALGORITHM_FULL_SEARCH,
};


/** compression level 1 final stage options */
static const ALSEncStage stage_final_c1 = {
    .check_constant         = 1,
    .check_lsbs             = 1,
    .ecsub_algorithm        = EC_SUB_ALGORITHM_RICE_EXACT,
    .param_algorithm        = EC_PARAM_ALGORITHM_RICE_EXACT,
    .count_algorithm        = EC_BIT_COUNT_ALGORITHM_EXACT,
    .adapt_search_algorithm = ADAPT_SEARCH_ALGORITHM_VALLEY_DETECT,
    .adapt_count_algorithm  = ADAPT_COUNT_ALGORITHM_ESTIMATE,
    .ltp_coeff_algorithm    = LTP_COEFF_ALGORITHM_FIXED,
    .merge_algorithm        = BS_ALGORITHM_FULL_SEARCH,
};


/** compression level 2 global options **/
static const ALSSpecificConfig spc_config_c2 = {
    .adapt_order            = 1,
    .long_term_prediction   = 1,
    .max_order              = 32,
    .block_switching        = 1,
    .bgmc                   = 1,
    .sb_part                = 1,
    .joint_stereo           = 1,
    .mc_coding              = 0,
    .rlslms                 = 0,
    .crc_enabled            = 1,
};


/** compression level 2 joint-stereo stage options */
static const ALSEncStage stage_js_c2 = {
    .check_constant         = 1,
    .check_lsbs             = 1,
    .ecsub_algorithm        = EC_SUB_ALGORITHM_BGMC_EXACT,
    .param_algorithm        = EC_PARAM_ALGORITHM_BGMC_ESTIMATE,
    .count_algorithm        = EC_BIT_COUNT_ALGORITHM_EXACT,
    .adapt_search_algorithm = ADAPT_SEARCH_ALGORITHM_VALLEY_DETECT,
    .adapt_count_algorithm  = ADAPT_COUNT_ALGORITHM_ESTIMATE,
    .ltp_coeff_algorithm    = LTP_COEFF_ALGORITHM_CHOLESKY,
    .merge_algorithm        = BS_ALGORITHM_FULL_SEARCH,
};


/** compression level 2 block switching stage options */
static const ALSEncStage stage_bs_c2 = {
    .check_constant         = 1,
    .check_lsbs             = 1,
    .ecsub_algorithm        = EC_SUB_ALGORITHM_BGMC_EXACT,
    .param_algorithm        = EC_PARAM_ALGORITHM_BGMC_ESTIMATE,
    .count_algorithm        = EC_BIT_COUNT_ALGORITHM_EXACT,
    .adapt_search_algorithm = ADAPT_SEARCH_ALGORITHM_VALLEY_DETECT,
    .adapt_count_algorithm  = ADAPT_COUNT_ALGORITHM_ESTIMATE,
    .ltp_coeff_algorithm    = LTP_COEFF_ALGORITHM_CHOLESKY,
    .merge_algorithm        = BS_ALGORITHM_FULL_SEARCH,
};


/** compression level 2 final stage options */
static const ALSEncStage stage_final_c2 = {
    .check_constant         = 1,
    .check_lsbs             = 1,
    .ecsub_algorithm        = EC_SUB_ALGORITHM_BGMC_EXACT,
    .param_algorithm        = EC_PARAM_ALGORITHM_BGMC_ESTIMATE,
    .count_algorithm        = EC_BIT_COUNT_ALGORITHM_EXACT,
    .adapt_search_algorithm = ADAPT_SEARCH_ALGORITHM_VALLEY_DETECT,
    .adapt_count_algorithm  = ADAPT_COUNT_ALGORITHM_ESTIMATE,
    .ltp_coeff_algorithm    = LTP_COEFF_ALGORITHM_CHOLESKY,
    .merge_algorithm        = BS_ALGORITHM_FULL_SEARCH,
};


/** global options for each compression level */
static const ALSSpecificConfig * const spc_config_settings[3] = {
    &spc_config_c0,
    &spc_config_c1,
    &spc_config_c2,
};


/** joint-stereo stage options for each compression level */
static const ALSEncStage * const stage_js_settings[3] = {
    &stage_js_c0,
    &stage_js_c1,
    &stage_js_c2,
};


/** block switching stage options for each compression level */
static const ALSEncStage * const stage_bs_settings[3] = {
    &stage_bs_c0,
    &stage_bs_c1,
    &stage_bs_c2,
};


/** final stage options for each compression level */
static const ALSEncStage * const stage_final_settings[3] = {
    &stage_final_c0,
    &stage_final_c1,
    &stage_final_c2,
};


static void dprint_stage_options(AVCodecContext *avctx, ALSEncStage *stage)
{
    av_log(avctx, AV_LOG_DEBUG, "check_constant = %d\n", stage->check_constant);
    av_log(avctx, AV_LOG_DEBUG, "check_lsbs = %d\n", stage->check_lsbs);
    av_log(avctx, AV_LOG_DEBUG, "adapt_order = %d\n", stage->adapt_order);
    av_log(avctx, AV_LOG_DEBUG, "max_order = %d\n", stage->max_order);
    av_log(avctx, AV_LOG_DEBUG, "sb_part = %d\n", stage->sb_part);

    switch (stage->ecsub_algorithm) {
    case EC_SUB_ALGORITHM_RICE_ESTIMATE: av_log(avctx, AV_LOG_DEBUG, "ecsub_algorithm = rice estimate\n"); break;
    case EC_SUB_ALGORITHM_RICE_EXACT:    av_log(avctx, AV_LOG_DEBUG, "ecsub_algorithm = rice exact\n");    break;
    case EC_SUB_ALGORITHM_BGMC_EXACT:    av_log(avctx, AV_LOG_DEBUG, "ecsub_algorithm = bgmc exact\n");    break;
    }

    switch (stage->param_algorithm) {
    case EC_PARAM_ALGORITHM_RICE_ESTIMATE: av_log(avctx, AV_LOG_DEBUG, "param_algorithm = rice estimate\n"); break;
    case EC_PARAM_ALGORITHM_RICE_EXACT:    av_log(avctx, AV_LOG_DEBUG, "param_algorithm = rice exact\n");    break;
    case EC_PARAM_ALGORITHM_BGMC_ESTIMATE: av_log(avctx, AV_LOG_DEBUG, "param_algorithm = bgmc estimate\n"); break;
    case EC_PARAM_ALGORITHM_BGMC_EXACT:    av_log(avctx, AV_LOG_DEBUG, "param_algorithm = bgmc exact\n");    break;
    }

    switch (stage->count_algorithm) {
    case EC_BIT_COUNT_ALGORITHM_ESTIMATE: av_log(avctx, AV_LOG_DEBUG, "count_algorithm = estimate\n"); break;
    case EC_BIT_COUNT_ALGORITHM_EXACT:    av_log(avctx, AV_LOG_DEBUG, "count_algorithm = exact\n");    break;
    }

    switch (stage->adapt_search_algorithm) {
    case ADAPT_SEARCH_ALGORITHM_VALLEY_DETECT: av_log(avctx, AV_LOG_DEBUG, "adapt_search_algorithm = valley detect\n"); break;
    case ADAPT_SEARCH_ALGORITHM_FULL:          av_log(avctx, AV_LOG_DEBUG, "adapt_search_algorithm = full\n");          break;
    }

    switch (stage->adapt_count_algorithm) {
    case ADAPT_COUNT_ALGORITHM_ESTIMATE: av_log(avctx, AV_LOG_DEBUG, "adapt_count_algorithm = estimate\n"); break;
    case ADAPT_COUNT_ALGORITHM_EXACT:    av_log(avctx, AV_LOG_DEBUG, "adapt_count_algorithm = exact\n");    break;
    }

    switch (stage->ltp_coeff_algorithm) {
    case LTP_COEFF_ALGORITHM_FIXED:    av_log(avctx, AV_LOG_DEBUG, "ltp_coeff_algorithm = fixed\n");    break;
    case LTP_COEFF_ALGORITHM_CHOLESKY: av_log(avctx, AV_LOG_DEBUG, "ltp_coeff_algorithm = cholesky\n"); break;
    }

    switch (stage->merge_algorithm) {
    case BS_ALGORITHM_FULL_SEARCH: av_log(avctx, AV_LOG_DEBUG, "merge_algorithm = full search\n"); break;
    case BS_ALGORITHM_BOTTOM_UP:   av_log(avctx, AV_LOG_DEBUG, "merge_algorithm = bottom-up\n");   break;
    }
}


/**
 * Convert an array of channel-interleaved samples into multiple arrays of
 * samples per channel.
 */
static void deinterleave_raw_samples(ALSEncContext *ctx, void *data)
{
    unsigned int sample, c, shift;

    // transform input into internal format
    #define DEINTERLEAVE_INPUT(bps)                                 \
    {                                                               \
        int##bps##_t *src = (int##bps##_t*) data;                   \
        shift = bps - ctx->avctx->bits_per_raw_sample;              \
        for (sample = 0; sample < ctx->avctx->frame_size; sample++) \
            for (c = 0; c < ctx->avctx->channels; c++)              \
                ctx->raw_samples[c][sample] = (*src++) >> shift;    \
    }

    if (ctx->avctx->bits_per_raw_sample <= 8) {
        uint8_t *src = (uint8_t *)data;
        shift = 8 - ctx->avctx->bits_per_raw_sample;
        for (sample = 0; sample < ctx->avctx->frame_size; sample++)
            for (c = 0; c < ctx->avctx->channels; c++)
                ctx->raw_samples[c][sample] = ((int)(*src++) - 128) >> shift;
    } else if (ctx->avctx->bits_per_raw_sample <= 16) {
        DEINTERLEAVE_INPUT(16)
    } else {
        DEINTERLEAVE_INPUT(32)
    }
}


/**
 * Recursively parse a given block partitioning and sum up all block sizes
 * according to *bs_sizes to get the overall bit count.
 */
static void bs_get_size(const uint32_t bs_info, unsigned int n,
                        unsigned int *bs_sizes, unsigned int *bit_count)
{
    if (n < 31 && ((bs_info << n) & 0x40000000)) {
        // if the level is valid and the investigated bit n is set
        // then recursively check both children at bits (2n+1) and (2n+2)
        n   *= 2;
        bs_get_size(bs_info, n + 1, bs_sizes, bit_count);
        bs_get_size(bs_info, n + 2, bs_sizes, bit_count);
    } else {
        // else the bit is not set or the last level has been reached
        // (bit implicitly not set)
        (*bit_count) += bs_sizes[n];
    }
}


/**
 * Recursively parse a given block partitioning and set all node bits to zero.
 */
static void bs_set_zero(uint32_t *bs_info, unsigned int n)
{
    if (n < 31) {
        // if the level is valid set this bit and
        // all children to zero
        *bs_info &= ~(1 << (30 - n));
        n        *= 2;
        bs_set_zero(bs_info, n + 1);
        bs_set_zero(bs_info, n + 2);
    }
}


/**
 * Recursively parse a given block partitioning and set all joint-stereo
 * block flags according to *js_info.
 */
static void bs_set_js(const uint32_t bs_info, unsigned int n,
                      uint8_t *js_info,
                      ALSBlock **block_c1, ALSBlock **block_c2)
{
    if (n < 31 && ((bs_info << n) & 0x40000000)) {
        // if the level is valid and the investigated bit n is set
        // then recursively check both children at bits (2n+1) and (2n+2)
        n *= 2;
        bs_set_js(bs_info, n + 1, js_info, block_c1, block_c2);
        bs_set_js(bs_info, n + 2, js_info, block_c1, block_c2);
    } else {
        // else the bit is not set or the last level has been reached
        // (bit implicitly not set)
        (*block_c1)->js_block = (js_info[n] == 1);
        (*block_c2)->js_block = (js_info[n] == 2);
        (*block_c1)++;
        (*block_c2)++;
    }
}


/**
 * Recursively set all block sizes to joint-stereo sizes where difference
 * coding pays off for a block.
 */
static void set_js_sizes(ALSEncContext *ctx, unsigned int channel, int stage)
{
    ALSSpecificConfig *sconf = &ctx->sconf;
    unsigned int num_blocks  = sconf->block_switching ? (1 << stage) : 1;
    unsigned int b;

    for (b = 0; b < num_blocks; b++) {
        unsigned int *block_size = ctx->bs_sizes[channel     ] + num_blocks - 1;
        unsigned int *buddy_size = ctx->bs_sizes[channel +  1] + num_blocks - 1;
        unsigned int *js_size    = ctx->js_sizes[channel >> 1] + num_blocks - 1;
        uint8_t      *js_info    = ctx->js_infos[channel >> 1] + num_blocks - 1;

        // replace independent signal size with
        // joint-stereo signal size according to js_info
        // store independent value for resetting to independent coding
        if        (js_info[b] == 1) {
            FFSWAP(unsigned int, block_size[b], js_size[b]);
        } else if (js_info[b] == 2)
            FFSWAP(unsigned int, buddy_size[b], js_size[b]);
    }

    if (sconf->block_switching && stage < sconf->block_switching)
        set_js_sizes(ctx, channel, stage + 1);
}


/**
 * Recursively reset all block sizes to independent sizes.
 */
static void reset_js_sizes(ALSEncContext *ctx, unsigned int channel, int stage)
{
    ALSSpecificConfig *sconf = &ctx->sconf;
    unsigned int num_blocks  = sconf->block_switching ? (1 << stage) : 1;
    unsigned int b;

    for (b = 0; b < num_blocks; b++) {
        ALSBlock     *blocks     = ctx->blocks  [channel     ] + num_blocks - 1;
        ALSBlock     *buddys     = ctx->blocks  [channel +  1] + num_blocks - 1;
        unsigned int *block_size = ctx->bs_sizes[channel     ] + num_blocks - 1;
        unsigned int *buddy_size = ctx->bs_sizes[channel +  1] + num_blocks - 1;
        unsigned int *js_size    = ctx->js_sizes[channel >> 1] + num_blocks - 1;
        uint8_t      *js_info    = ctx->js_infos[channel >> 1] + num_blocks - 1;

        // replace joint-stereo signal size with
        // independent signal size accoring to js_info
        if        (js_info[b] == 1) {
            FFSWAP(unsigned int, block_size[b], js_size[b]);
        } else if (js_info[b] == 2)
            FFSWAP(unsigned int, buddy_size[b], js_size[b]);

        js_info[b]          = 0;
        blocks [b].js_block = 0;
        buddys [b].js_block = 0;
    }

    if (sconf->block_switching && stage < sconf->block_switching)
        reset_js_sizes(ctx, channel, stage + 1);
}


/**
 * Recursively merge all subblocks of the frame until the minimal bit count is
 * found.
 * Use Full-Search strategy.
 */
static void bs_merge_fullsearch(ALSEncContext *ctx, unsigned int n,
                                unsigned int c1, unsigned int c2)
{
    uint32_t *bs_info = &ctx->bs_info[c1];

    if (n < 31 && ((*bs_info << n) & 0x40000000)) {
        // if the level is valid and the investigated bit n is set
        // then recursively check both children at bits (2n+1) and (2n+2)
        unsigned int *sizes_c1 = ctx->bs_sizes[c1];
        unsigned int *sizes_c2 = ctx->bs_sizes[c2];
        unsigned int a         = 2 * n + 1;
        unsigned int b         =     a + 1;
        unsigned int sum_a     = 0;
        unsigned int sum_b     = 0;
        unsigned int sum_n     = sizes_c1[n];

        if (GET_BS_BIT(bs_info, a)) {
            bs_merge_fullsearch(ctx, a, c1, c2);
        }

        if (GET_BS_BIT(bs_info, b)) {
            bs_merge_fullsearch(ctx, b, c1, c2);
        }

        // calculate sizes of both children
        bs_get_size(*bs_info, a, sizes_c1, &sum_a);
        bs_get_size(*bs_info, b, sizes_c1, &sum_b);

        if (c1 != c2) {
            // for joint-stereo, also calculate size of
            // the children of the buddy channel
            sum_n += sizes_c2[n];
            bs_get_size(*bs_info, a, sizes_c2, &sum_a);
            bs_get_size(*bs_info, b, sizes_c2, &sum_b);
        }

        // test for merging
        if (sum_a + sum_b > sum_n) {
            bs_set_zero(bs_info, n);
            if (c1 != c2)
                ctx->bs_info[c2] = *bs_info;
        }
    }
}


/**
 * Recursively merge all subblocks of the frame until the minimal bit count is
 * found.
 * Use Bottom-Up strategy.
 */
static void bs_merge_bottomup(ALSEncContext *ctx, unsigned int n,
                              unsigned int c1, unsigned int c2)
{
    uint32_t *bs_info = &ctx->bs_info[c1];

    if (n < 31 && ((*bs_info << n) & 0x40000000)) {
        // if the level is valid and the investigated bit n is set
        // then recursively check both children at bits (2n+1) and (2n+2)
        unsigned int *sizes_c1 = ctx->bs_sizes[c1];
        unsigned int *sizes_c2 = ctx->bs_sizes[c2];
        unsigned int a         = 2 * n + 1;
        unsigned int b         =     a + 1;
        unsigned int sum_a     = 0;
        unsigned int sum_b     = 0;
        unsigned int sum_n     = sizes_c1[n];

        if (GET_BS_BIT(bs_info, a) && GET_BS_BIT(bs_info, b)) {
            bs_merge_bottomup(ctx, a, c1, c2);
            bs_merge_bottomup(ctx, b, c1, c2);
        }

        // test if both children are leaves of the tree only
        if (!GET_BS_BIT(bs_info, a) && !GET_BS_BIT(bs_info, b)) {
            // get sizes of both children
            sum_a += sizes_c1[a];
            sum_b += sizes_c1[b];

            if (c1 != c2) {
                // for joint-stereo, also get size of
                // the children of the buddy channel
                sum_n += sizes_c2[n];
                sum_a += sizes_c2[a];
                sum_b += sizes_c2[b];
            }

            // test for merging
            if (sum_a + sum_b > sum_n) {
                bs_set_zero(bs_info, n);
                if (c1 != c2)
                    ctx->bs_info[c2] = *bs_info;
            }
        }
    }
}


/**
 * Read block partitioning and set actual block sizes and all sample pointers.
 * Also assure that the block sizes of the last frame correspond to the
 * actual number of samples.
 */
static void set_blocks(ALSEncContext *ctx, uint32_t *bs_info,
                       unsigned int c1, unsigned int c2)
{
    AVCodecContext *avctx    = ctx->avctx;
    ALSSpecificConfig *sconf = &ctx->sconf;
    unsigned int div_blocks[32];
    unsigned int *ptr_div_blocks = div_blocks;
    unsigned int b;
    int ltp          = sconf->long_term_prediction;
    int32_t *ltp_ptr = ltp ? ctx->ltp_samples[c1] : NULL;
    int32_t *res_ptr = ctx->res_samples[c1];
    int32_t *smp_ptr = ctx->raw_samples[c1];
    int32_t *dif_ptr = ctx->raw_dif_samples[c1 >> 1];
    int32_t *lsb_ptr = ctx->raw_lsb_samples[c1];
    ALSBlock *block  = ctx->blocks[c1];

    ctx->num_blocks[c1] = 0;

    ff_als_parse_bs_info(*bs_info, 0, 0, &ptr_div_blocks, &ctx->num_blocks[c1]);

    // The last frame may have an overdetermined block structure given in
    // the bitstream. In that case the defined block structure would need
    // more samples than available to be consistent.
    // The block structure is actually used but the block sizes are adapted
    // to fit the actual number of available samples.
    // Example: 5 samples, 2nd level block sizes: 2 2 2 2.
    // This results in the actual block sizes:    2 2 1 0.
    // This is not specified in 14496-3 but actually done by the reference
    // codec RM22 revision 2.
    // This appears to happen in case of an odd number of samples in the last
    // frame which is actually not allowed by the block length switching part
    // of 14496-3.
    // The ALS conformance files feature an odd number of samples in the last
    // frame.

    for (b = 0; b < ctx->num_blocks[c1]; b++) {
        block->div_block = div_blocks[b];
        div_blocks[b]    = ctx->sconf.frame_length >> div_blocks[b];
        block->length    = div_blocks[b];
        block->res_ptr   = res_ptr;
        block->ltp_ptr   = ltp_ptr;
        block->smp_ptr   = smp_ptr;
        block->dif_ptr   = dif_ptr;
        block->lsb_ptr   = lsb_ptr;
        res_ptr         += block->length;
        ltp_ptr         += block->length;
        smp_ptr         += block->length;
        dif_ptr         += block->length;
        lsb_ptr         += block->length;
        block++;
    }

    if (avctx->frame_size != sconf->frame_length) {
        unsigned int remaining = avctx->frame_size;

        for (b = 0; b < ctx->num_blocks[c1]; b++) {
            if (remaining <= div_blocks[b]) {
                ctx->blocks[c1][b].div_block = -1;
                ctx->blocks[c1][b].length    = remaining;
                ctx->num_blocks[c1]          = b + 1;
                break;
            }
            remaining -= ctx->blocks[c1][b].length;
        }
    }

    if (c1 != c2) {
        res_ptr             = ctx->res_samples[c2];
        ltp_ptr             = ltp ? ctx->ltp_samples[c2] : NULL;
        smp_ptr             = ctx->raw_samples[c2];
        dif_ptr             = ctx->raw_dif_samples[c1 >> 1];
        lsb_ptr             = ctx->raw_lsb_samples[c2];
        block               = ctx->blocks[c2];
        ctx->num_blocks[c2] = ctx->num_blocks[c1];

        for (b = 0; b < ctx->num_blocks[c1]; b++) {
            block->div_block = ctx->blocks[c1][b].div_block;
            block->length    = ctx->blocks[c1][b].length;
            block->res_ptr   = res_ptr;
            block->ltp_ptr   = ltp_ptr;
            block->smp_ptr   = smp_ptr;
            block->dif_ptr   = dif_ptr;
            block->lsb_ptr   = lsb_ptr;
            res_ptr         += block->length;
            ltp_ptr         += block->length;
            smp_ptr         += block->length;
            dif_ptr         += block->length;
            lsb_ptr         += block->length;
            block++;
        }
    }
}


/**
 * Get the best block partitioning for the current frame depending on the
 * chosen algorithm and set the block sizes accordingly.
 * @return Overall bit count for the partition
 */
static unsigned int get_partition(ALSEncContext *ctx, unsigned int c1, unsigned int c2)
{
    ALSEncStage *stage     = ctx->cur_stage;
    unsigned int *sizes_c1 = ctx->bs_sizes[c1];
    unsigned int *sizes_c2 = ctx->bs_sizes[c2];
    unsigned int bit_count = 0;

    // find best partitioning
    if(stage->merge_algorithm == BS_ALGORITHM_BOTTOM_UP) {
        bs_merge_bottomup(ctx, 0, c1, c2);
    } else {
        bs_merge_fullsearch(ctx, 0, c1, c2);
    }

    set_blocks(ctx, &ctx->bs_info[c1], c1, c2);

    if (c1 != c2) {
        // set joint-stereo sizes
        ALSBlock *ptr_blocks_c1 = ctx->blocks[c1];
        ALSBlock *ptr_blocks_c2 = ctx->blocks[c2];
        bs_set_js(ctx->bs_info[c1], 0, ctx->js_infos[c1 >> 1], &ptr_blocks_c1,
                  &ptr_blocks_c2);
    }

    // get bit count for the chosen partitioning
    bs_get_size(ctx->bs_info[c1], 0, sizes_c1, &bit_count);
    if (c1 != c2)
        bs_get_size(ctx->bs_info[c1], 0, sizes_c2, &bit_count);

    return bit_count;
}



/**
 * Subdivide the frame into smaller blocks.
 */
static void block_partitioning(ALSEncContext *ctx)
{
    AVCodecContext *avctx    = ctx->avctx;
    ALSSpecificConfig *sconf = &ctx->sconf;
    unsigned int c;

    // find the best partitioning for each channel
    if (!sconf->mc_coding || ctx->js_switch) {
        // for each channel pair
        for (c = 0; c < avctx->channels - 1; c += 2) {
            if (sconf->joint_stereo) {
                unsigned int bits_ind, bits_dep;
                unsigned int bs_info_len = 1 << FFMAX(3, sconf->block_switching);
                int32_t bs_info_c1, bs_info_c2;
                int32_t bs_info = ctx->bs_info[c];

                // get bit count and bs_info fields
                // for independent channels
                bits_ind    = get_partition(ctx, c    , c    );
                bits_ind   += get_partition(ctx, c + 1, c + 1);
                bs_info_c1  = ctx->bs_info[c    ];
                bs_info_c2  = ctx->bs_info[c + 1];

                ctx->bs_info[c] = bs_info;

                // get bit count and bs_info fields
                // for joint-stereo
                set_js_sizes(ctx, c, 0);
                bits_dep = get_partition(ctx, c, c + 1);

                // set to independent coding if necessary
                if (bits_ind + bs_info_len < bits_dep) {
                    reset_js_sizes(ctx, c, 0);
                    ctx->independent_bs[c    ] = 1;
                    ctx->independent_bs[c + 1] = 1;
                    ctx->bs_info       [c    ] = bs_info_c1;
                    ctx->bs_info       [c + 1] = bs_info_c2;
                    set_blocks(ctx, &ctx->bs_info[c    ], c    , c    );
                    set_blocks(ctx, &ctx->bs_info[c + 1], c + 1, c + 1);
                }
            } else {
                get_partition(ctx, c    , c    );
                get_partition(ctx, c + 1, c + 1);
            }
        }
        // for the last channel if number of channels is odd
        if (c < avctx->channels) {
            get_partition(ctx, c, c);
        }
    } else {
        // MCC: to be implemented
    }
}


/**
 * Count bits needed to write value 'v' using signed Rice coding with
 * parameter 'k'.
 */
static inline int rice_count(int v, int k)
{
    unsigned int v0 = (unsigned int)((2LL*v) ^ (int64_t)(v>>31));
    return (v0 >> k) + 1 + k;
}


/**
 * Count bits needed to write value 'v' using unsigned Rice coding with
 * parameter 'k'.
 */
static inline int urice_count(unsigned int v, int k)
{
    return (v >> k) + 1 + k;
}


/**
 * Write the quotient part of a Rice code.
 * This is the same for signed and unsigned Rice coding.
 * @param[out] q0 quotient
 * @return        0 on success, -1 on error
 */
static inline int golomb_write_quotient(PutBitContext *pb, unsigned int v,
                                        int k, int *q0)
{
    int q;

    *q0 = v >> k;
    q   = *q0 + 1;

    /* protect from buffer overwrite */
    OVERFLOW_PROTECT(pb, q+k, return -1;)

    while (q > 31) {
        put_bits(pb, 31, 0x7FFFFFFF);
        q -= 31;
    }
    put_bits(pb, q, ((1<<q)-1)^1);

    return 0;
}


/**
 * Write a signed Rice code.
 * @return 0 on success, -1 on error
 */
static inline int set_ur_golomb_als(PutBitContext *pb, unsigned int v, int k)
{
    int q0;

    /* write quotient in zero-terminated unary */
    if (golomb_write_quotient(pb, v, k, &q0))
        return -1;

    /* write remainder using k bits */
    if (k)
        put_bits(pb, k, v - (q0 << k));

    return 0;
}


/**
 * Write an unsigned Rice code to the bitstream.
 * @return 0 on success, -1 on error
 */
static inline int set_sr_golomb_als(PutBitContext *pb, int v, int k)
{
    unsigned int v0;
    int q0;

    /* remap to unsigned */
    v0 = (unsigned int)((2LL*v) ^ (int64_t)(v>>31));

    /* write quotient in zero-terminated unary */
    if (golomb_write_quotient(pb, v0, k, &q0))
        return -1;

    /* write remainder using k bits */
    if (k)
        put_bits(pb, k, (v0 >> 1) - ((q0-(!(v0&1))) << (k-1)));

    return 0;
}


/**
 * Encode the LSB part of the given symbols.
 * @return Overall bit count for all encoded symbols
 */
static int bgmc_encode_lsb(PutBitContext *pb, const int32_t *symbols, unsigned int n,
                           unsigned int k, unsigned int max, unsigned int s)
{
    int count       = 0;
    int lsb_mask    = (1 << k) - 1;
    int abs_max     = (max + 1) >> 1;
    int high_offset = -(abs_max      << k);
    int low_offset  =  (abs_max - 1) << k;

    for (; n > 0; n--) {
        int32_t res = *symbols++;

        if ((res >> k) >=  abs_max || (res >> k) <= -abs_max) {
            res += (res >> k) >= abs_max ? high_offset : low_offset;
            if (pb && set_sr_golomb_als(pb, res, s) < 0)
                return -1;
            count += rice_count(res, s);
        } else if (k) {
            if (pb) {
                OVERFLOW_PROTECT(pb, k, return -1;)
                put_sbits(pb, k, res & lsb_mask);
            }
            count += k;
        }
    }

    return count;
}


/**
 * Map LTP gain value to nearest flattened array index.
 * @return Nearest array index
 */
static int map_to_index(int gain)
{
    int i, diff, min_diff, best_index;
    const uint8_t *g_ptr = &ff_als_ltp_gain_values[0][0];

    min_diff   = abs((int)(*g_ptr++) - gain);
    best_index = 0;
    for (i = 1; i < 16; i++) {
        diff = abs((int)(*g_ptr++) - gain);

        if (!diff) {
            return i;
        } else if (diff < min_diff) {
            min_diff   = diff;
            best_index = i;
        } else {
            return best_index;
        }
    }

    return best_index;
}


/**
 * Generate the long-term predicted residuals for a given block using the
 * current set of LTP parameters.
 */
static void gen_ltp_residuals(ALSEncContext *ctx, ALSBlock *block)
{
    ALSLTPInfo *ltp  = &block->ltp_info[block->js_block];
    int32_t *ltp_ptr = block->ltp_ptr;
    int offset       = FFMAX(ltp->lag - 2, 0);
    unsigned int ltp_smp;
    int64_t y;
    int center, end, base;

    memcpy(ltp_ptr, block->cur_ptr, sizeof(*ltp_ptr) * offset);

    center = offset - ltp->lag;
    end    = center + 3;
    for (ltp_smp = offset; ltp_smp < block->length; ltp_smp++,center++,end++) {
        int begin = FFMAX(0, center - 2);
        int tab   = 5 - (end - begin);

        y = 1 << 6;

        for (base = begin; base < end; base++, tab++)
            y += MUL64(ltp->gain[tab], block->cur_ptr[base]);

        ltp_ptr[ltp_smp] = block->cur_ptr[ltp_smp] - (y >> 7);
    }
}


/**
 * Write a given block.
 * @return 0 on success, -1 otherwise
 */
static int write_block(ALSEncContext *ctx, ALSBlock *block)
{
    AVCodecContext *avctx    = ctx->avctx;
    ALSSpecificConfig *sconf = &ctx->sconf;
    PutBitContext *pb        = &ctx->pb;
    unsigned int i;
    int start = 0;

    // block_type
    PUT_BITS_SAFE(pb, 1, !block->constant, return -1;)

    if (block->constant) {
        OVERFLOW_PROTECT(pb, 7, return -1;)

        // const_block
        put_bits(pb, 1, block->constant_value != 0);

        // js_block
        put_bits(pb, 1, block->js_block);
        // reserved
        put_bits(pb, 5, 0);

        if (block->constant_value) {
            int const_val_bits = sconf->floating ? 24 : avctx->bits_per_raw_sample;
            OVERFLOW_PROTECT(pb, const_val_bits, return -1;)
            if (const_val_bits == 32)
                put_bits32(pb, block->constant_value);
            else
                put_sbits(pb, const_val_bits, block->constant_value);
        }
    } else {
        ALSLTPInfo *ltp     = &block->ltp_info[block->js_block];
        ALSEntropyInfo *ent = &block->ent_info[ltp->use_ltp];
        int32_t *res_ptr;
        int sb, sb_length;
        unsigned int high;
        unsigned int low;
        unsigned int follow;
        unsigned int delta[8];
        unsigned int k    [8];
        unsigned int max  [8];
        unsigned int *s  = ent->rice_param;
        unsigned int *sx = ent->bgmc_param;

        // js_block
        PUT_BITS_SAFE(pb, 1, block->js_block, return -1;)

        // ec_sub
        if (sconf->sb_part || sconf->bgmc) {
            if (sconf->sb_part && sconf->bgmc)
                PUT_BITS_SAFE(pb, 2, av_log2(ent->sub_blocks), return -1;)
            else
                // In the last frame, ent->sub_blocks should always be 1 regardless
                // of anything else.
                // This is the case in reference encoder and we need to handle this
                // in our encoder properly.
                PUT_BITS_SAFE(pb, 1, ent->sub_blocks > 1, return -1;)
        }

        // s[k], sx[k]
        if (sconf->bgmc) {
            unsigned int S[8];

            for (sb = 0; sb < ent->sub_blocks; sb++)
                S[sb] = (ent->rice_param[sb] << 4) | ent->bgmc_param[sb];

            PUT_BITS_SAFE(pb, 8 + (avctx->bits_per_raw_sample > 16), S[0], return -1;)
            for (sb = 1; sb < ent->sub_blocks; sb++)
                if (set_sr_golomb_als(pb, S[sb] - S[sb-1], 2))
                    return -1;
        } else {
            PUT_BITS_SAFE(pb, 4 + (avctx->bits_per_raw_sample > 16), ent->rice_param[0], return -1;)

            for (sb = 1; sb < ent->sub_blocks; sb++) {
                if (set_sr_golomb_als(pb, ent->rice_param[sb] - ent->rice_param[sb-1], 0))
                    return -1;
            }
        }

        // shift_lsbs && shift_pos
        PUT_BITS_SAFE(pb, 1, block->shift_lsbs > 0, return -1;)

        if (block->shift_lsbs)
            PUT_BITS_SAFE(pb, 4, block->shift_lsbs - 1, return -1;)

        // opt_order && quant_cof
        if (!sconf->rlslms) {
            // opt_order
            if (sconf->adapt_order) {
                PUT_BITS_SAFE(pb, block->bits_adapt_order, block->opt_order, return -1;)
            }

            // for each quant_cof, put(quant_cof) in rice code
            if (sconf->coef_table == 3) {
                OVERFLOW_PROTECT(pb, block->opt_order * 7, return -1;)
                for (i = 0; i < block->opt_order; i++)
                    put_bits(pb, 7, 64 + block->q_parcor_coeff[i]);
            } else {
                // write coefficient 0 to 19
                int next_max_order = FFMIN(block->opt_order, 20);
                for (i = 0; i < next_max_order; i++) {
                    int rice_param = ff_als_parcor_rice_table[sconf->coef_table][i][1];
                    int offset     = ff_als_parcor_rice_table[sconf->coef_table][i][0];
                    if (set_sr_golomb_als(pb, block->q_parcor_coeff[i] - offset, rice_param))
                        return -1;
                }

                // write coefficients 20 to 126
                next_max_order = FFMIN(block->opt_order, 127);
                for (; i < next_max_order; i++)
                    if (set_sr_golomb_als(pb, block->q_parcor_coeff[i] - (i & 1), 2))
                        return -1;

                // write coefficients 127 to opt_order
                for (; i < block->opt_order; i++)
                    if (set_sr_golomb_als(pb, block->q_parcor_coeff[i], 1))
                        return -1;
            }
        }

        // LPTenable && LTPgain && LTPlag
        if (sconf->long_term_prediction) {
            PUT_BITS_SAFE(pb, 1, ltp->use_ltp, return -1;)

            if (ltp->use_ltp) {
                int ltp_lag_length = 8 + (avctx->sample_rate >=  96000) +
                                         (avctx->sample_rate >= 192000);

                if (set_sr_golomb_als(pb, ltp->gain[0] >> 3,          1) ||
                    set_sr_golomb_als(pb, ltp->gain[1] >> 3,          2) ||
                    set_ur_golomb_als(pb, map_to_index(ltp->gain[2]), 2) ||
                    set_sr_golomb_als(pb, ltp->gain[3] >> 3,          2) ||
                    set_sr_golomb_als(pb, ltp->gain[4] >> 3,          1)) {
                    return -1;
                }

                PUT_BITS_SAFE(pb, ltp_lag_length,
                              ltp->lag - FFMAX(4, block->opt_order + 1),
                              return -1;)
            }
        }

        // write residuals
        // for now, all frames are RA frames, so use progressive prediction for
        // the first 3 residual samples, up to opt_order

        res_ptr   = block->cur_ptr;
        sb_length = block->length / ent->sub_blocks;

        if (sconf->bgmc)
            ff_bgmc_encode_init(&high, &low, &follow);

        for (sb = 0; sb < ent->sub_blocks; sb++) {
            i = 0;
            if (!sb && block->ra_block) {
                int len = block->opt_order;
                int32_t write;
                if (len > 0) {
                    if (set_sr_golomb_als(pb, *res_ptr++, avctx->bits_per_raw_sample-4))
                        return -1;
                    i++;
                    if (len > 1) {
                        write = sb_length <= 1 ? 0 : *res_ptr++;
                        if (set_sr_golomb_als(pb, write, FFMIN(ent->rice_param[sb]+3, ctx->max_rice_param)))
                            return -1;
                        i++;
                        if (len > 2) {
                            write = sb_length <= 2 ? 0 : *res_ptr++;
                            if (set_sr_golomb_als(pb, write, FFMIN(ent->rice_param[sb]+1, ctx->max_rice_param)))
                                return -1;
                            i++;
                        }
                    }
                }
                start = i;
            }
            if (sconf->bgmc) {
                unsigned int b = av_clip((av_ceil_log2(block->length) - 3) >> 1, 0, 5);
                k    [sb]      = s[sb] > b ? s[sb] - b : 0;
                delta[sb]      = 5 - s[sb] + k[sb];
                max  [sb]      = ff_bgmc_max[sx[sb]] >> delta[sb];

                if (ff_bgmc_encode_msb(pb, res_ptr, sb_length - i,
                                       k[sb], delta[sb], max[sb],
                                       s[sb], sx[sb],
                                       &high, &low, &follow) < 0) {
                    return -1;
                }

                res_ptr += sb_length - i;
            } else {
                for (; i < sb_length; i++) {
                    if (set_sr_golomb_als(pb, *res_ptr++, ent->rice_param[sb]))
                        return -1;
                }
            }
        }

        if (sconf->bgmc) {
            if (ff_bgmc_encode_end(pb, &low, &follow) < 0)
                return -1;

            res_ptr = block->cur_ptr + start;

            for (sb = 0; sb < ent->sub_blocks; sb++, start = 0) {
                if (bgmc_encode_lsb(pb, res_ptr, sb_length - start, k[sb], max[sb], s[sb]) < 0)
                    return -1;
                res_ptr += sb_length - start;
            }
        }
    }

    if (!sconf->mc_coding || ctx->js_switch)
        avpriv_align_put_bits(pb);

    return 0;
}


/**
 * Write the frame.
 * @return Overall bit count for the frame on success, -1 otherwise
 */
static int write_frame(ALSEncContext *ctx, const AVPacket *avpkt, int buf_size)
{
    AVCodecContext *avctx    = ctx->avctx;
    ALSSpecificConfig *sconf = &ctx->sconf;
    unsigned int c, b;
    int ret;

    if ((ret = ff_alloc_packet2(avctx, avpkt, buf_size, 0)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error getting output packet\n");
        return ret;
    }
    init_put_bits(&ctx->pb, avpkt->data, avpkt->size);

    // make space for ra_unit_size
    if (sconf->ra_flag == RA_FLAG_FRAMES && sconf->ra_distance == 1) {
        // TODO: maybe keep frame count and allow other RA distances if API will allow
        OVERFLOW_PROTECT(&ctx->pb, 32, return -1;)
        put_bits32(&ctx->pb, 0);
    }

    // js_switch
    if (ctx->js_switch) {
        // to be implemented
        // not yet supported anyway
    }

    // write blocks
    if (!sconf->mc_coding || ctx->js_switch) {
        for (c = 0; c < avctx->channels; c++) {
            if (sconf->block_switching) {
                unsigned int bs_info_len = 1 << FFMAX(3, sconf->block_switching);
                uint32_t bs_info         = ctx->bs_info[c];

                if (sconf->joint_stereo && ctx->independent_bs[c])
                    bs_info |= (1 << 31);

                OVERFLOW_PROTECT(&ctx->pb, bs_info_len, return -1;)
                if (bs_info_len == 32)
                    put_bits32(&ctx->pb, bs_info);
                else
                    put_bits(&ctx->pb, bs_info_len, bs_info >> (32 - bs_info_len));
            }

            for (b= 0; b < ctx->num_blocks[c]; b++) {
                if (ctx->independent_bs[c]) {
                    if (write_block(ctx, &ctx->blocks[c][b]) < 0)
                        return -1;
                } else {
                    if (write_block(ctx, &ctx->blocks[c  ][b]) < 0 ||
                        write_block(ctx, &ctx->blocks[c+1][b]) < 0) {
                        return -1;
                    }
                }
            }

            if(!ctx->independent_bs[c]) c++;
        }
    } else {

        // MCC: to be implemented

    }

    flush_put_bits(&ctx->pb);
    ret = put_bits_count(&ctx->pb) >> 3;

    // write ra_unit_size
    if (sconf->ra_flag == RA_FLAG_FRAMES && sconf->ra_distance == 1) {
        // AV_WB32(frame, ret);
        put_bits32(&ctx->pb, ret);
        flush_put_bits(&ctx->pb);
    }

    return ret;
}


/**
 * Quantize and rescale a single PARCOR coefficient.
 * @param ctx Encoder context
 * @param parcor double-precision PARCOR coefficient
 * @param index  coefficient index number
 * @param[out] q_parcor 7-bit quantized coefficient
 * @param[out] r_parcor 21-bit reconstructed coefficient
 * @return the number of bits used to encode the coefficient
 */
static int quantize_single_parcor_coeff(ALSEncContext *ctx, double parcor,
                                        int index, int32_t *q_parcor,
                                        int32_t *r_parcor)
{
    int rice_param, offset;
    int sign = !index - index;

    // compand coefficient for index 0 or 1
    if (index < 2)
        parcor = sqrt(2.0 * (sign * parcor + 1.0)) - 1.0;

    // quantize to signed 7-bit
    *q_parcor = av_clip((int32_t)floor(64.0 * parcor), -64, 63);

    // rescale to signed 21-bit
    if (index < 2)
        *r_parcor =  sign * 32 * ff_als_parcor_scaled_values[*q_parcor + 64];
    else
        *r_parcor = (*q_parcor << 14) + (1 << 13);

    // count bits used for this coefficient
    if (index < 20) {
        rice_param = ff_als_parcor_rice_table[ctx->sconf.coef_table][index][1];
        offset     = ff_als_parcor_rice_table[ctx->sconf.coef_table][index][0];
    } else if (index < 127) {
        rice_param = 2;
        offset     = index & 1;
    } else {
        rice_param = 1;
        offset     = 0;
    }

    return rice_count(*q_parcor - offset, rice_param);
}


/**
 * Quantize all PARCOR coefficients up to max_order and set the cumulative
 * bit counts for each order.
 */
static void quantize_parcor_coeffs(ALSEncContext *ctx, ALSBlock *block,
                                   const double *parcor, int max_order)
{
    int i;

    block->bits_parcor_coeff[0] = 0;
    for (i = 0; i < max_order; i++) {
        block->bits_parcor_coeff[i+1] = block->bits_parcor_coeff[i] +
                                        quantize_single_parcor_coeff(ctx, parcor[i], i,
                                                                     &block->q_parcor_coeff[i],
                                                                     &ctx->r_parcor_coeff[i]);
    }
}


/**
 * Count bits needed to encode all symbols of a given subblock using the
 * given parameters.
 */
static unsigned int subblock_ec_count_exact(const int32_t *res_ptr,
                                            int b_length, int sb_length,
                                            int s, int sx, int max_param,
                                            int ra_subblock, int order,
                                            int bgmc)
{
    unsigned int count = 0;
    unsigned int len   = 0;

    if (ra_subblock) {
        if (order > 0) {
            int32_t v = *res_ptr++;
            len++;
            count += rice_count(v, max_param - 3);
            if (order > 1) {
                v = sb_length <= 1 ? 0 : *res_ptr++;
                len++;
                count += rice_count(v, FFMIN(s+3, max_param));
                if (order > 2) {
                    v = sb_length <= 2 ? 0 : *res_ptr++;
                    len++;
                    count += rice_count(v, FFMIN(s+1, max_param));
                }
            }
        }
    }

    if (bgmc) {
        unsigned int high, low, follow;
        unsigned int delta, k, max, b;
        int c;

        // count msb's
        ff_bgmc_encode_init(&high, &low, &follow);

        b     = av_clip((av_ceil_log2(b_length) - 3) >> 1, 0, 5);
        k     = s > b ? s - b : 0;
        delta = 5 - s + k;
        max = ff_bgmc_max[sx] >> delta;

        c = ff_bgmc_encode_msb(NULL, res_ptr, sb_length - len, k, delta, max,
                               s, sx, &high, &low, &follow);
        if (c < 0)
            return -1;
        count += c;

        c = ff_bgmc_encode_end(NULL, &low, &follow);
        if (c < 0)
            return -1;
        count += c;

        // count lsb's
        c = bgmc_encode_lsb(NULL, res_ptr, sb_length - len, k, max, s);
        if (c < 0)
            return -1;
        count += c;
    } else {
        int i;
        for (i = len; i < sb_length; i++) {
            int32_t v = *res_ptr++;
            count    += rice_count(v, s);
        }
    }

    return count;
}


/**
 * Count bits needed to encode all the entropy coding parameters for a block.
 */
static unsigned int block_ec_param_count(ALSEncContext *ctx, ALSBlock *block,
                                         int sub_blocks, int *s, int *sx,
                                         int bgmc)
{
    unsigned int count = 0;
    int k              = bgmc ? 2 : 0;
    int sb;

    count += (4 << bgmc) + (ctx->max_rice_param > 15);
    if (sub_blocks) {
        for (sb = 1; sb < sub_blocks; sb++) {
            int ep_diff;
            if (bgmc)
                ep_diff = ((s[sb    ] << 4) | sx[sb    ]) -
                          ((s[sb - 1] << 4) | sx[sb - 1]);
            else
                ep_diff = s[sb] - s[sb - 1];
            count += rice_count(ep_diff, k);
        }
    }

    count += (!!ctx->sconf.sb_part) << bgmc;

    return count;
}


/**
 * Count bits needed to encode all symbols and entropy coding parameters of a
 * given block using given parameters.
 */
static unsigned int block_ec_count_exact(ALSEncContext *ctx, ALSBlock *block,
                                         int sub_blocks, int *s, int *sx,
                                         int order, int bgmc)
{
    int32_t *res_ptr   = block->cur_ptr;
    unsigned int count = 0;
    int sb_length, sb;

    sb_length = block->length / sub_blocks;

    for (sb = 0; sb < sub_blocks; sb++) {
        count += subblock_ec_count_exact(res_ptr, block->length, sb_length,
                                         s[sb], sx?sx[sb]:0,
                                         ctx->max_rice_param,
                                         !sb && block->ra_block, order, bgmc);
        res_ptr += sb_length;
    }

    count += block_ec_param_count(ctx, block, sub_blocks, s, sx, bgmc);

    return count;
}


#define rice_encode_count(sum, n, k) (((n)*((k)+1))+((sum-(n>>1))>>(k)))


/**
 * Estimate the best Rice parameter using the sum of unsigned residual samples.
 */
static inline int estimate_rice_param(uint64_t sum, int length, int max_param)
{
    int k;

    if (sum <= length >> 1)
        return 0;

    if (sum > UINT32_MAX) {
        sum = FFMAX((sum - (length >> 1)) / length, 1);
        k   = (int)floor(log2(sum));
    } else {
        unsigned int sum1 = sum - (length >> 1);
        k    = av_log2(length < 256 ? FASTDIV(sum1, length) : sum1 / length);
    }

    return FFMIN(k, max_param);
}


/**
 * Get an estimated Rice parameter and split it into its LSB and MSB for
 * further processing in BGMC.
 */
static inline void estimate_bgmc_params(uint64_t sum, unsigned int n, int *s,
                                        int *sx)
{
#define OFFSET 0.97092725747512664825  /* 0.5 + log2(1.386) */

    if (!sum) { // avoid log2(0)
        *sx = *s = 0;
    } else {
        int tmp = (int)(16.0 * (log2(sum) - log2(n) + OFFSET));
        tmp     = FFMAX(tmp, 0);
        *sx     = tmp & 0x0F;
        *s      = tmp >> 4;
    }
}


static void find_block_rice_params_est(ALSEncContext *ctx, ALSBlock *block,
                                       int order)
{
    int i, sb, sb_max, sb_length, p0;
    uint64_t sum[5] = {0,};
    int param[5];
    unsigned int count1, count4;
    ALSEncStage *stage     = ctx->cur_stage;
    ALSLTPInfo *ltp        = &block->ltp_info[block->js_block];
    ALSEntropyInfo *ent    = &block->ent_info[ltp->use_ltp];
    const int32_t *res_ptr = block->cur_ptr;

    if (!stage->sb_part || block->length & 0x3 || block->length < 16)
        sb_max = 1;
    else
        sb_max = 4;
    sb_length = block->length / sb_max;

    for (sb = 0; sb < sb_max; sb++) {
        for (i = 0; i < sb_length; i++) {
            int32_t v = *res_ptr++;
            sum[sb]  += (unsigned int)((2LL*v) ^ (int64_t)(v>>31));
        }
        sum[4] += sum[sb];

        param[sb] = estimate_rice_param(sum[sb], sb_length, ctx->max_rice_param);
    }

    param[4] = estimate_rice_param(sum[4], block->length, ctx->max_rice_param);

    if (stage->count_algorithm == EC_BIT_COUNT_ALGORITHM_EXACT) {
        count1 = block_ec_count_exact(ctx, block, 1, &param[4], NULL, order, 0);
    } else {
        count1  = rice_encode_count(sum[4], block->length, param[4]);
        count1 += 4 + (ctx->max_rice_param > 15);
    }

    p0 = param[0];
    if (sb_max == 1 || ((p0 == param[1]) && (p0 == param[2]) &&
        (p0 == param[3]))) {
        ent->sub_blocks            = 1;
        ent->rice_param[0]         = param[4];
        ent->bits_ec_param_and_res = count1;
        return;
    }

    if (stage->count_algorithm == EC_BIT_COUNT_ALGORITHM_EXACT) {
        count4 = block_ec_count_exact(ctx, block, 4, param, NULL, order, 0);
    } else {
        count4 = 0;
        for (sb = 0; sb < sb_max; sb++) {
            count4 += rice_encode_count(sum[sb], sb_length, param[sb]);

            if (!sb)
                count4 += 4 + (ctx->max_rice_param > 15);
            else
                count4 += rice_count(param[sb] - param[sb-1], 0);
        }
    }

    if (count1 <= count4) {
        ent->sub_blocks    = 1;
        ent->rice_param[0] = param[4];
        ent->bits_ec_param_and_res = count1;
    } else {
        ent->sub_blocks    = 4;
        ent->rice_param[0] = param[0];
        ent->rice_param[1] = param[1];
        ent->rice_param[2] = param[2];
        ent->rice_param[3] = param[3];
        ent->bits_ec_param_and_res = count4;
    }
}


/**
 * Perform full search for optimal BGMC parameters and sub-block division.
 */
static void find_block_bgmc_params_est(ALSEncContext *ctx, ALSBlock *block,
                                       int order)
{
    ALSEncStage    *stage = ctx->cur_stage;
    ALSLTPInfo     *ltp   = &block->ltp_info[block->js_block];
    ALSEntropyInfo *ent   = &block->ent_info[ltp->use_ltp];
    int s[4][8], sx[4][8];
    int p, sb, i;
    int p_max;
    int p_best;
    unsigned int count_best = UINT_MAX;
    uint64_t sum[4][8];

    if (!stage->sb_part || block->length & 0x3 || block->length < 16)
        p_max = 0;
    else
        p_max = 3;

    p_best = p_max;

    for (p = p_max; p >= 0; p--) {
        int num_subblocks  = 1 << p;
        int sb_length      = block->length / num_subblocks;
        unsigned int count = 0;
        int32_t *res_ptr   = block->cur_ptr;

        for (sb = 0; sb < num_subblocks; sb++) {
            if (p == p_max) {
                int32_t *r_ptr = res_ptr;
                sum[p][sb]     = 0;
                for (i = 0; i < sb_length; i++)
                    sum[p][sb] += abs(*r_ptr++);
            } else {
                sum[p][sb] = sum[p+1][sb<<1] + sum[p+1][(sb<<1)+1];
            }
            estimate_bgmc_params(sum[p][sb], sb_length, &s[p][sb], &sx[p][sb]);

            if (stage->ecsub_algorithm == EC_SUB_ALGORITHM_RICE_ESTIMATE) {
                int k = estimate_rice_param (sum[p][sb], sb_length,
                                             ctx->max_rice_param);
                count += rice_encode_count(sum[p][sb], sb_length, k);
            }

            res_ptr += sb_length;
        }

        if (stage->ecsub_algorithm == EC_SUB_ALGORITHM_BGMC_EXACT) {
            count = block_ec_count_exact(ctx, block, num_subblocks,
                                         s[p], sx[p], order, 1);
        }

        if (count <= count_best) {
            count_best = count;
            p_best     = p;
        }
    }

    ent->sub_blocks = 1 << p_best;
    for (sb = 0; sb < ent->sub_blocks; sb++) {
        ent->rice_param[sb] = s [p_best][sb];
        ent->bgmc_param[sb] = sx[p_best][sb];
    }

    if (stage->ecsub_algorithm == EC_SUB_ALGORITHM_RICE_ESTIMATE &&
        stage->count_algorithm == EC_BIT_COUNT_ALGORITHM_EXACT) {
        ent->bits_ec_param_and_res = block_ec_count_exact(ctx, block, ent->sub_blocks,
                                                          ent->rice_param, ent->bgmc_param,
                                                          order, 1);
    } else {
        ent->bits_ec_param_and_res = count_best;
    }
}


static void find_block_rice_params_exact(ALSEncContext *ctx, ALSBlock *block,
                                         int order)
{
    unsigned int count[4] = {0,};
    int param[4], p0;
    int k, step, sb, sb_max, sb_length;
    int best_k;
    unsigned int count1, count4;
    ALSEncStage *stage  = ctx->cur_stage;
    ALSLTPInfo *ltp     = &block->ltp_info[block->js_block];
    ALSEntropyInfo *ent = &block->ent_info[ltp->use_ltp];

    if (!stage->sb_part || block->length & 0x3 || block->length < 16)
        sb_max = 1;
    else
        sb_max = 4;
    sb_length = block->length / sb_max;

    best_k = ctx->max_rice_param / 3;

    for (sb = 0; sb < sb_max; sb++) {
        int32_t *res_ptr = block->cur_ptr + (sb * sb_length);
        unsigned int c1, c2;
        k = FFMIN(best_k, ctx->max_rice_param-1);
        c1 = subblock_ec_count_exact(res_ptr,
                                     block->length, sb_length, k, 0,
                                     ctx->max_rice_param,
                                     !sb && block->ra_block, order, 0);
        k++;
        c2 = subblock_ec_count_exact(res_ptr,
                                     block->length, sb_length, k, 0,
                                     ctx->max_rice_param,
                                     !sb && block->ra_block, order, 0);
        if (c2 < c1) {
            best_k = k;
            step   = 1;
            k++;
        } else {
            best_k = k - 1;
            c2     = c1;
            step   = -1;
            k     -= 2;
        }

        for (; k >= 0 && k <= ctx->max_rice_param; k += step) {
            c1 = subblock_ec_count_exact(res_ptr,
                                         block->length, sb_length, k, 0,
                                         ctx->max_rice_param,
                                         !sb && block->ra_block, order, 0);

            if (c1 < c2) {
                best_k = k;
                c2     = c1;
            } else {
                break;
            }
        }
        param[sb] = best_k;
        count[sb] = c2;
    }

    /* if sub-block partitioning is not used, stop here */
    p0 = param[0];
    if (sb_max == 1 || (p0 == param[1] && p0 == param[2] && p0 == param[3])) {
        ent->sub_blocks    = 1;
        ent->rice_param[0] = param[0];
        ent->bits_ec_param_and_res = block_ec_count_exact(ctx, block, 1,
                                                          param, NULL, order, 0);
        return;
    }

    p0 = (param[0] + param[1] + param[2] + param[3]) >> 2;

    count1 = block_ec_count_exact(ctx, block, 1, &p0, NULL, order, 0);
    count4 = count[0] + count[1] + count[2] + count[3] +
             block_ec_param_count(ctx, block, 4, param, NULL, 0);
    if (count1 <= count4) {
        ent->sub_blocks    = 1;
        ent->rice_param[0] = p0;
        ent->bits_ec_param_and_res = count1;
    } else {
        ent->sub_blocks    = 4;
        ent->rice_param[0] = param[0];
        ent->rice_param[1] = param[1];
        ent->rice_param[2] = param[2];
        ent->rice_param[3] = param[3];
        ent->bits_ec_param_and_res = count4;
    }
}


static void find_block_bgmc_params_exact(ALSEncContext *ctx, ALSBlock *block, int order)
{
    ALSEncStage    *stage = ctx->cur_stage;
    ALSLTPInfo     *ltp   = &block->ltp_info[block->js_block];
    ALSEntropyInfo *ent   = &block->ent_info[ltp->use_ltp];
    int s[4][8];
    int sx[4][8];
    int p, sb;
    int p_max;
    int p_best;
    unsigned int count_best = UINT_MAX;

    if (!stage->sb_part || block->length & 0x3 || block->length < 16)
        p_max = 0;
    else
        p_max = 3;

    p_best = p_max;

    for (p = p_max; p >= 0; p--) {
        int num_subblocks  = 1 << p;
        int sb_length      = block->length / num_subblocks;
        int32_t *res_ptr   = block->cur_ptr;
        unsigned int count;

        for (sb = 0; sb < num_subblocks; sb++) {
            int s0, si, sxi;
            int step;
            int best_s0 = 0;
            unsigned int s0_count[256];
            int dc = 0;

            /* guess starting point for search */
            if (!sb) {
                if (p < p_max)
                    s0 = av_clip((s[p+1][sb>>1] << 4) | sx[p+1][sb>>1], 5, 250);
                else
                    s0 = 127;
            } else {
                s0 = av_clip((s[p][sb-1] << 4) | sx[p][sb-1], 5, 250);
            }
            s0_count[s0] = subblock_ec_count_exact(res_ptr, block->length,
                                sb_length, s0 >> 4, s0 & 0xF,
                                ctx->max_rice_param, !sb && block->ra_block,
                                order, 1);

            /* determine search direction */
            s0 += 5;
            s0_count[s0] = subblock_ec_count_exact(res_ptr, block->length,
                                sb_length, s0 >> 4, s0 & 0xF,
                                ctx->max_rice_param, !sb && block->ra_block,
                                order, 1);
            s0 -= 10;
            s0_count[s0] = subblock_ec_count_exact(res_ptr, block->length,
                                sb_length, s0 >> 4, s0 & 0xF,
                                ctx->max_rice_param, !sb && block->ra_block,
                                order, 1);
            s0 += 5;
            if (s0_count[s0+5] < s0_count[s0]) {
                step = 1;
            } else if (s0_count[s0-5] < s0_count[s0]) {
                step = -1;
            } else {
                /* lowest count is likely between s0-4 and s0+4 */
                int max_s0 = s0 + 5;
                best_s0    = s0;
                for (s0 = s0 - 4; s0 < max_s0; s0++) {
                    s0_count[s0] = subblock_ec_count_exact(res_ptr, block->length,
                                        sb_length, s0 >> 4, s0 & 0xF,
                                        ctx->max_rice_param, !sb && block->ra_block,
                                        order, 1);
                    if (s0_count[s0] < s0_count[best_s0])
                        best_s0 = s0;
                }
                dc = 1;
            }

            /* search for best parameters */
            if (!dc) {
                best_s0 = s0;
                s0     += step;

                for (; s0 >= 0 && s0 < 256; s0 += step) {
                    si  = s0 >> 4;
                    sxi = s0 & 0xF;
                    s0_count[s0] = subblock_ec_count_exact(res_ptr,
                                        block->length, sb_length, si, sxi,
                                        ctx->max_rice_param, !sb && block->ra_block,
                                        order, 1);
                    if (s0_count[s0] < s0_count[best_s0]) {
                        best_s0 = s0;
                        dc      = 0;
                    } else {
                        dc++;
                        if (dc > 5)
                            break;
                    }
                }
            }

            /* save best parameters for this sub-block */
            s [p][sb] = best_s0 >> 4;
            sx[p][sb] = best_s0 & 0xF;

            res_ptr += sb_length;
        }
        count = block_ec_count_exact(ctx, block, num_subblocks, s[p], sx[p],
                                     order, 1);
        if (count < count_best) {
            count_best = count;
            p_best     = p;
        }
    }

    ent->sub_blocks = 1 << p_best;
    for (sb = 0; sb < ent->sub_blocks; sb++) {
        ent->rice_param[sb] = s [p_best][sb];
        ent->bgmc_param[sb] = sx[p_best][sb];
    }

    ent->bits_ec_param_and_res = count_best;
}


/**
 * Calculate optimal sub-block division and Rice parameters for a block.
 * @param ctx                   encoder context
 * @param block                 current block
 * @param ra_block              indicates if this is a random access block
 * @param order                 LPC order
 */
static void find_block_entropy_params(ALSEncContext *ctx, ALSBlock *block,
                                   int order)
{
    ALSEncStage *stage = ctx->cur_stage;

    if        (stage->param_algorithm == EC_PARAM_ALGORITHM_BGMC_ESTIMATE) {
        find_block_bgmc_params_est(ctx, block, order);
    } else if (stage->param_algorithm == EC_PARAM_ALGORITHM_BGMC_EXACT) {
        find_block_bgmc_params_exact(ctx, block, order);
    } else if (stage->param_algorithm == EC_PARAM_ALGORITHM_RICE_ESTIMATE) {
        find_block_rice_params_est(ctx, block, order);
    } else if (stage->param_algorithm == EC_PARAM_ALGORITHM_RICE_EXACT) {
        find_block_rice_params_exact(ctx, block, order);
    }
}


static int calc_short_term_prediction(ALSEncContext *ctx, ALSBlock *block,
                                       int order)
{
    ALSSpecificConfig *sconf = &ctx->sconf;
    int i, j;

    int32_t *res_ptr = block->res_ptr;
    int32_t *smp_ptr = block->cur_ptr;

    av_assert0(order > 0);

#define LPC_PREDICT_SAMPLE(lpc, smp_ptr, res_ptr, order)\
{\
    int64_t y = 1 << 19;\
    for (j = 1; j <= order; j++)\
        y += MUL64(lpc[j-1], (smp_ptr)[-j]);\
    *(res_ptr++) = *(smp_ptr++) + (int32_t)(y >> 20);\
}

    i = 0;
    if (block->ra_block) {
        int ra_order = FFMIN(order, block->length);

        // copy first residual sample verbatim
        *(res_ptr++) = *(smp_ptr++);

        // progressive prediction
        ff_als_parcor_to_lpc(0, ctx->r_parcor_coeff, ctx->lpc_coeff);
        for (i = 1; i < ra_order; i++) {
            LPC_PREDICT_SAMPLE(ctx->lpc_coeff, smp_ptr, res_ptr, i);
            if (ff_als_parcor_to_lpc(i, ctx->r_parcor_coeff, ctx->lpc_coeff))
                return -1;
        }
        // zero unused coeffs for small frames since they are all written
        // to the bitstream if adapt_order is not used.
        if (!sconf->adapt_order) {
            for (; i < sconf->max_order; i++)
                block->q_parcor_coeff[i] = ctx->r_parcor_coeff[i] = 0;
        }
    } else {
        for (j = 0; j < order; j++)
            if (ff_als_parcor_to_lpc(j, ctx->r_parcor_coeff, ctx->lpc_coeff))
                return -1;
    }
    // remaining residual samples
    for (; i < block->length; i++) {
        LPC_PREDICT_SAMPLE(ctx->lpc_coeff, smp_ptr, res_ptr, order);
    }
    return 0;
}


/**
 * Test given block samples to be of constant value.
 * Set block->const_block_bits to the number of bits used for encoding the
 * constant block, or to zero if the block is not a constant block.
 */
static void test_const_value(ALSEncContext *ctx, ALSBlock *block)
{
    if (ctx->cur_stage->check_constant) {
        unsigned int n   = block->length;
        int32_t *smp_ptr = block->cur_ptr;
        int32_t val      = *smp_ptr++;

        block->constant = 1;
        while (--n > 0) {
            if (*smp_ptr++ != val) {
                block->constant = 0;
                break;
            }
        }

        block->bits_const_block = 0;
        if (block->constant) {
            block->constant_value    = val;
            block->bits_const_block += 6;   // const_block + reserved
            if (block->constant_value) {
                block->bits_const_block += ctx->sconf.floating ? 24 :
                                        ctx->avctx->bits_per_raw_sample;
            }
        }
    } else {
        block->constant = 0;
    }
}


/**
 * Test given block samples to share common zero LSBs.
 * Set block->shift_lsbs to the number common zero bits.
 */
static void test_zero_lsb(ALSEncContext *ctx, ALSBlock *block)
{
    int i;
    int32_t common = 0;

    block->shift_lsbs = 0;

    if (ctx->cur_stage->check_lsbs) {
        // test for zero LSBs
        for (i = 0; i < (int)block->length; i++) {
            common |= block->cur_ptr[i];

            if (common & 1)
                return;
        }

        while (!(common & 1)) {
            block->shift_lsbs++;
            common >>= 1;
        }

        // generate shifted signal and point block->cur_ptr to the LSB buffer
        if (block->shift_lsbs) {
            for (i = -ctx->sconf.max_order; i < (int)block->length; i++)
                block->lsb_ptr[i] = block->cur_ptr[i] >> block->shift_lsbs;

            block->cur_ptr = block->lsb_ptr;
        }
    }
}


/**
 * Generate a weighted residual signal for autocorrelation detection used in
 * LTP mode.
 */
static void get_weighted_signal(ALSEncContext *ctx, ALSBlock *block,
                                int lag_max)
{
    int len          = (int)block->length;
    int32_t *cur_ptr = block->cur_ptr;
    uint64_t sum     = 0;
    double *corr_ptr = ctx->corr_samples;
    double mean_quot;
    int i;

    // determine absolute mean of residual signal,
    // including previous samples
    for (i = -lag_max; i < len; i++)
        sum += abs(cur_ptr[i]);
    mean_quot = (double)(sum) / (block->length + lag_max);

    // apply weighting:
    // x *= 1 / [ sqrt(abs(x)) / 5*sqrt(mean) + 1 ]
    mean_quot = (sqrt(mean_quot) * 5);
    for (i = -lag_max-2; i < len; i++)
        corr_ptr[i] = cur_ptr[i] / (sqrt(abs(cur_ptr[i])) / mean_quot + 1.0);
}


/**
 * Calculate a generic autocorrelation with optional normalization.
 * double source data, no windowing, data-lag assumed to be valid pointer.
 */
static void compute_autocorr_norm(const double *data, int len, int lag,
                                  int normalize, double *autoc)
{
    int i, j;

    for (j = 0; j < lag; j++) {
        double sum = 1.0;
        for (i = j; i < len; i++)
            sum += data[i] * data[i-j];
        autoc[j] = sum;
        if (normalize)
            autoc[j] /= autoc[0];
    }
}


/**
 * Generate the autocorrelation function and find its positive maximum value to
 * be used for LTP lag.
 */
static void find_best_autocorr(ALSEncContext *ctx, ALSBlock *block,
                               int lag_max, int start)
{
    int i, i_max;
    double autoc_max;
    double *autoc = av_malloc_array(lag_max, sizeof(*autoc));

    compute_autocorr_norm(ctx->corr_samples, block->length, lag_max, 1, autoc);

    autoc_max = autoc[start];
    i_max     = start;
    for (i = start + 1; i < lag_max; i++) {
        // find best positive autocorrelation
        if (autoc[i] > 0 && autoc[i] > autoc_max) {
            autoc_max = autoc[i];
            i_max     = i;
        }
    }

    block->ltp_info[block->js_block].lag = i_max;
    av_freep(&autoc);
}


/**
 * Set fixed values for LTP coefficients.
 */
static void get_ltp_coeffs_fixed(ALSEncContext *ctx, ALSBlock *block)
{
    int *ltp_gain = block->ltp_info[block->js_block].gain;

    ltp_gain[0] = 8;
    ltp_gain[1] = 8;
    ltp_gain[2] = 16;
    ltp_gain[3] = 8;
    ltp_gain[4] = 8;
}


/**
 * Calculate LTP coefficients using Cholesky factorization.
 */
static void get_ltp_coeffs_cholesky(ALSEncContext *ctx, ALSBlock *block)
{
    int icc, quant;
    int smp, i;
    int len          = (int)block->length;
    int taumax       = block->ltp_info[block->js_block].lag;
    int *ltp_gain    = block->ltp_info[block->js_block].gain;
    double *corr_ptr = ctx->corr_samples;
    double *corr_ptr_lag;
    LLSModel m;
    double *c = &m.covariance[0][1];
    double *coeff = m.coeff[4];

    avpriv_init_lls(&m, 5);

    corr_ptr_lag = corr_ptr - 2 - taumax;
    for (smp = 0; smp < len - 2; smp++) {
        for (int i = 0; i <= m.indep_count; i++) {
            for (int j = i; j <= m.indep_count; j++) {
                m.covariance[i][j] += corr_ptr_lag[i] * corr_ptr_lag[j];
            }
        }
        corr_ptr_lag++;
    }

    corr_ptr_lag = corr_ptr - 2 - taumax;
    memset(c, 0, 5 * sizeof(*c));
    for (smp = 0; smp < len - 2; smp++) {
        double v = *corr_ptr++;
        c[0] += v * corr_ptr_lag[0];
        c[1] += v * corr_ptr_lag[1];
        c[2] += v * corr_ptr_lag[2];
        c[3] += v * corr_ptr_lag[3];
        c[4] += v * corr_ptr_lag[4];
        corr_ptr_lag++;
    }

    avpriv_solve_lls(&m, 0.0, 0);

    // quantize coefficients

    // 0,1 and 3,4  (linear quantization)
    for (icc = 0; icc < 5; icc++) {
        int g = lrint(coeff[icc] * 16.0);
        if (icc & 1)
            ltp_gain[icc] = av_clip(g, -8, 7) * 8;
        else
            ltp_gain[icc] = av_clip(g, -6, 5) * 8;
    }

    // 2 (vector quantization, roughly logarithmic)
    quant       = lrint(coeff[2] * 256.0);
    ltp_gain[2] = 0;
    for(i = 15; i > 0; i--) {
        uint8_t a = ff_als_ltp_gain_values[ i    >> 2][ i    & 3];
        uint8_t b = ff_als_ltp_gain_values[(i-1) >> 2][(i-1) & 3];
        if (quant > a + b) {
            ltp_gain[2] = a;
            return;
        }
    }
}


/**
 * Select the best set of LTP parameters based on maximum autocorrelation
 * value of the weighted residual signal.
 */
static void find_block_ltp_params(ALSEncContext *ctx, ALSBlock *block)
{
    AVCodecContext *avctx    = ctx->avctx;

    int start = FFMAX(4, block->opt_order + 1);
    int end   = FFMIN(ALS_MAX_LTP_LAG, block->length);
    int lag   = 256 << (  (avctx->sample_rate >=  96000)
                        + (avctx->sample_rate >= 192000));
    int lag_max;
    if (lag + start > end - 3)
        lag = end - start - 3;
    lag_max = FFMIN(lag + start, end);

    if (block->length <= start || lag <= 0 ) {
        memset(block->ltp_info[block->js_block].gain, 0, 5 * sizeof(int));
        block->ltp_info[block->js_block].lag = start;
        return;
    }

    get_weighted_signal(ctx, block, lag_max);
    find_best_autocorr (ctx, block, lag_max, start);
    if (ctx->cur_stage->ltp_coeff_algorithm == LTP_COEFF_ALGORITHM_FIXED)
        get_ltp_coeffs_fixed(ctx, block);
    else
        get_ltp_coeffs_cholesky(ctx, block);
}


static void check_ltp(ALSEncContext *ctx, ALSBlock *block, int *bit_count)
{
    ALSLTPInfo *ltp = &block->ltp_info[block->js_block];
    int bit_count_ltp;
    int32_t *save_ptr  = block->cur_ptr;
    int ltp_lag_length = 8 + (ctx->avctx->sample_rate >=  96000) +
                             (ctx->avctx->sample_rate >= 192000);

    find_block_ltp_params(ctx, block);
    gen_ltp_residuals(ctx, block);

    // generate bit count for LTP signal
    block->cur_ptr = block->ltp_ptr;
    ltp->use_ltp   = 1;
    find_block_entropy_params(ctx, block, block->opt_order);

    ltp->bits_ltp = 1 + ltp_lag_length +
                    rice_count(ltp->gain[0],               1) +
                    rice_count(ltp->gain[1],               2) +
                   urice_count(map_to_index(ltp->gain[2]), 2) +
                    rice_count(ltp->gain[3],               2) +
                    rice_count(ltp->gain[4],               1);

    // test if LTP pays off
    bit_count_ltp = block->bits_misc + block->bits_adapt_order +
                    block->bits_parcor_coeff[block->opt_order] +
                    block->ent_info[ltp->use_ltp].bits_ec_param_and_res +
                    ltp->bits_ltp;
    bit_count_ltp += (8 - (bit_count_ltp & 7)) & 7;

    if (bit_count_ltp < *bit_count) {
        *bit_count = bit_count_ltp;
    } else {
        ltp->use_ltp   = 0;
        ltp->bits_ltp  = 1;
        block->cur_ptr = save_ptr;
    }
}


static int calc_block_size_fixed_order(ALSEncContext *ctx, ALSBlock *block,
                                       int order)
{
    int32_t count;
    int32_t *save_ptr   = block->cur_ptr;
    ALSLTPInfo *ltp     = &block->ltp_info[block->js_block];
    ALSEntropyInfo *ent = &block->ent_info[ltp->use_ltp];

    if (order) {
        if (calc_short_term_prediction(ctx, block, order))
            return -1;
        block->cur_ptr = block->res_ptr;
    }

    find_block_entropy_params (ctx, block, order);

    count  = block->bits_misc + block->bits_adapt_order +
             block->bits_parcor_coeff[order] + ent->bits_ec_param_and_res;
    count += (8 - (count & 7)) & 7; // byte align
#if 0
    if (ctx->sconf.long_term_prediction)
        check_ltp(ctx, block, &count);
#endif
    block->cur_ptr = save_ptr;

    return count;
}


static void find_block_adapt_order(ALSEncContext *ctx, ALSBlock *block,
                                   int max_order)
{
    int i;
    int32_t *count;
    int best             = 0;
    int valley_detect    = (ctx->cur_stage->adapt_search_algorithm ==
                            ADAPT_SEARCH_ALGORITHM_VALLEY_DETECT);
    int valley_threshold = FFMAX(2, max_order/6);
    int exact_count      = (ctx->cur_stage->adapt_count_algorithm ==
                            ADAPT_COUNT_ALGORITHM_EXACT);

    count = av_malloc_array(max_order+1, sizeof(*count));
    count[0] = INT32_MAX;

    for (i = 0; i <= max_order; i++) {
        if (exact_count) {
            count[i] = calc_block_size_fixed_order(ctx, block, i);
        } else {
            if (i && ctx->parcor_error[i-1] >= 1.0) {
                count[i]  = block->bits_misc + block->bits_adapt_order +
                            block->bits_parcor_coeff[i];
                count[i] += 0.5 * log2(ctx->parcor_error[i-1]) * block->length;
            } else {
                count[i]  = INT32_MAX;
            }
        }

        if (count[i] >= 0 && count[i] < count[best])
            best = i;
        else if (valley_detect && (i - best) > valley_threshold)
            break;
    }
    block->opt_order = best;
    av_freep(&count);
}


/**
 * Encode a given block of a given channel.
 * @return number of bits that will be used to encode the block using the
 *         determined parameters
 */
static int find_block_params(ALSEncContext *ctx, ALSBlock *block)
{
    ALSSpecificConfig *sconf = &ctx->sconf;
    int bit_count, max_order;
    ALSLTPInfo *ltp     = &block->ltp_info[block->js_block];
    ALSEntropyInfo *ent = &block->ent_info[ltp->use_ltp];

    block->cur_ptr = block->js_block ? block->dif_ptr : block->smp_ptr;

    block->bits_misc = 1;   // block_type

    // check for constant block

    test_const_value(ctx, block);

    // shifting samples:
    // determine if all the samples in this block can be right-shifted without
    // any information loss

    if (!block->constant) {
        test_zero_lsb(ctx, block);
        block->bits_misc++;         // shift_lsbs
        if (block->shift_lsbs)
            block->bits_misc += 4;  // shift_pos
    }

    block->bits_misc++; // add one bit for block->js_block

    // if this is a constant block, we don't need to find any other parameters
    if (block->constant)
        return block->bits_misc + block->bits_const_block;

    // short-term prediction:
    // use the mode chosen at encode_init() to find optimal parameters
    //
    // LPC / PARCOR coefficients to be stored in context
    // they depend on js_block and opt_order which may be changing later on

    // calculate bits needed to store adaptive LPC order
    if (sconf->adapt_order)
        block->bits_adapt_order = av_ceil_log2(av_clip((block->length >> 3) - 1,
                                               2, sconf->max_order + 1));
    else
        block->bits_adapt_order = 0;

    max_order = ctx->cur_stage->max_order;
    if (sconf->max_order) {
        double *corr_ptr;

        if (sconf->adapt_order)
            max_order = FFMIN(max_order, (1 << block->bits_adapt_order) - 1);

        // calculate PARCOR coefficients
        corr_ptr = ctx->corr_buffer;
        while (corr_ptr < ctx->corr_samples)
            *corr_ptr++ = 0.0;
        ff_window_apply(&ctx->acf_window[FFMAX(0, block->div_block)],
                        block->cur_ptr, corr_ptr, block->length);

        ctx->lpc.lpc_compute_autocorr(corr_ptr, block->length, max_order,
                                      ctx->acf_coeff);

        compute_ref_coefs(ctx->acf_coeff, max_order, ctx->parcor_coeff,
                          ctx->parcor_error);

        // quantize PARCOR coefficients to 7-bit and reconstruct to 21-bit
        quantize_parcor_coeffs(ctx, block, ctx->parcor_coeff, max_order);
    }

    // Determine optimal LPC order:
    //
    // quick estimate for LPC order. better searches will give better
    // significantly better results.
    if (sconf->max_order && sconf->adapt_order && ctx->cur_stage->adapt_order) {
        find_block_adapt_order(ctx, block, max_order);
    } else {
        block->opt_order = max_order;
    }

    // generate residuals using parameters:

    if (block->opt_order) {
        if (calc_short_term_prediction(ctx, block, block->opt_order)) {
            // if PARCOR to LPC conversion has 32-bit integer overflow,
            // fallback to using 1st order prediction
            double *parcor;

            if (ctx->cur_stage->adapt_order)
                block->opt_order = 1;

            parcor = av_malloc_array(block->opt_order, sizeof(*parcor));
            memset(parcor, 0, sizeof(parcor[0]));
            parcor[0] = -0.9;
            quantize_parcor_coeffs(ctx, block, parcor, block->opt_order);

            calc_short_term_prediction(ctx, block, block->opt_order);
            av_freep(&parcor);
        }
        block->cur_ptr = block->res_ptr;
    }

    // search for entropy coding (Rice/BGMC) parameters
    ltp->use_ltp = 0;
    ent = &block->ent_info[ltp->use_ltp];
    find_block_entropy_params(ctx, block, block->opt_order);

    ltp->bits_ltp = !!sconf->long_term_prediction;
    bit_count     = block->bits_misc + block->bits_parcor_coeff[block->opt_order] +
                    ent->bits_ec_param_and_res + ltp->bits_ltp;
    bit_count    += (8 - (bit_count & 7)) & 7; // byte align

    // determine lag and gain values for long-term prediction and
    // check if long-term prediction pays off for this block
    // and use LTP for this block if it does
    if (sconf->long_term_prediction)
        check_ltp(ctx, block, &bit_count);

    return bit_count;
}


/**
 * Generate all possible block sizes for all possible block-switching stages.
 */
static void gen_block_sizes(ALSEncContext *ctx, unsigned int channel, int stage)
{
    ALSSpecificConfig *sconf = &ctx->sconf;
    ALSBlock *block          = ctx->blocks[channel];
    unsigned int num_blocks  = sconf->block_switching ? (1 << stage) : 1;
    uint32_t bs_info_tmp     = 0;
    unsigned int b;

    ctx->num_blocks[channel] = num_blocks;

    if (stage) {
        for (b = 1; b < num_blocks; b++) {
            bs_info_tmp |= (1 << (31 - b));
        }
    }

    set_blocks(ctx, &bs_info_tmp, channel, channel);

    for (b = 0; b < num_blocks; b++) {
        unsigned int *bs_sizes = ctx->bs_sizes[channel     ] + num_blocks - 1;
        unsigned int *js_sizes = ctx->js_sizes[channel >> 1] + num_blocks - 1;

        // count residuals + block overhead
        block->js_block = 0;
        bs_sizes[b]     = find_block_params(ctx, block);

        if (sconf->joint_stereo && !(channel & 1)) {
            block->js_block = 1;
            js_sizes[b]     = find_block_params(ctx, block);
            block->js_block = 0;
        }

        block++;
    }

    if (sconf->block_switching && stage < sconf->block_switching)
        gen_block_sizes(ctx, channel, stage + 1);
    else
        ctx->bs_info[channel] = bs_info_tmp;
}


/**
 * Generate all suitable difference coding infos for all possible
 * block-switching stages.
 */
static void gen_js_infos(ALSEncContext *ctx, unsigned int channel, int stage)
{
    ALSSpecificConfig *sconf = &ctx->sconf;
    unsigned int num_blocks  = sconf->block_switching ? (1 << stage) : 1;
    unsigned int b;

    for (b = 0; b < num_blocks; b++) {
        unsigned int *block_size = ctx->bs_sizes[channel     ] + num_blocks - 1;
        unsigned int *buddy_size = ctx->bs_sizes[channel +  1] + num_blocks - 1;
        unsigned int *js_size    = ctx->js_sizes[channel >> 1] + num_blocks - 1;
        uint8_t      *js_info    = ctx->js_infos[channel >> 1] + num_blocks - 1;

        // replace normal signal with difference signal if suitable
        if (js_size[b] < block_size[b] ||
            js_size[b] < buddy_size[b]) {
            // mark the larger encoded block with the difference signal
            // and update this blocks size in bs_sizes
            if (block_size[b] > buddy_size[b]) {
                js_info[b] = 1;
            } else {
                js_info[b] = 2;
            }
        } else {
            js_info[b] = 0;
        }
    }

    if (sconf->block_switching && stage < sconf->block_switching)
        gen_js_infos(ctx, channel, stage + 1);
}


/**
 * Generate the difference signals for each channel pair channel & channel+1.
 */
static void gen_dif_signal(ALSEncContext *ctx, unsigned int channel)
{
    unsigned int n;
    unsigned int max_order = (ctx->ra_counter != 1) ? ctx->sconf.max_order : 0;

    int32_t *c1 = ctx->raw_samples    [channel     ] - max_order;
    int32_t *c2 = ctx->raw_samples    [channel  + 1] - max_order;
    int32_t *d  = ctx->raw_dif_samples[channel >> 1] - max_order;

    for (n = 0; n < ctx->avctx->frame_size + max_order; n++) {
        *d++ = *c2 - *c1;
        c1++;
        c2++;
    }
}


/**
 * Choose the appropriate method for difference channel coding for the current
 * frame.
 */
static void select_difference_coding_mode(ALSEncContext *ctx)
{
    AVCodecContext *avctx    = ctx->avctx;
    ALSSpecificConfig *sconf = &ctx->sconf;
    unsigned int c;

    // to be implemented
    // depends on sconf->joint_stereo and sconf->mc_coding
    // selects either joint_stereo or mcc mode
    // sets js_switch (js && mcc) and/or independent_bs
    // both parameters to be added to the context...
    // until mcc mode is implemented, syntax could be tested
    // by using js_switch all the time if mcc is enabled globally
    //
    // while not implemented, output most simple mode:
    //  -> 0 if !mcc and while js is not implemented
    //  -> 1 if  mcc (would require js to be implemented and
    //                correct output of mcc frames/block)

    ctx->js_switch = sconf->mc_coding;
    c              = 0;

    // if joint-stereo is enabled, dependently code each channel pair
    if (sconf->joint_stereo) {
        for (; c < avctx->channels - 1; c += 2) {
            ctx->independent_bs[c    ] = 0;
            ctx->independent_bs[c + 1] = 0;
        }
    }

    // set (the remaining) channels to independent coding
    for (; c < avctx->channels; c++)
        ctx->independent_bs[c] = 1;

    // generate difference signal if needed
    if (sconf->joint_stereo) {
        for (c = 0; c < avctx->channels - 1; c+=2) {
            gen_dif_signal(ctx, c);
        }
    }

    // generate all block sizes for this frame
    for (c = 0; c < avctx->channels; c++) {
        gen_block_sizes(ctx, c, 0);
    }

    // select difference signals wherever suitable
    if (sconf->joint_stereo) {
        for (c = 0; c < avctx->channels - 1; c+=2) {
            gen_js_infos(ctx, c, 0);
        }
    }
}


/**
 * Write an ALSSpecificConfig structure.
 * @return 0 on success, AVERROR(x) otherwise
 */
static int write_specific_config(AVCodecContext *avctx)
{
    ALSEncContext *ctx       = avctx->priv_data;
    ALSSpecificConfig *sconf = &ctx->sconf;
    MPEG4AudioConfig m4ac;
    int config_offset;

    unsigned int header_size = 6; // Maximum size of AudioSpecificConfig before ALSSpecificConfig

    // determine header size
    // crc & aux_data not yet supported
    header_size += ALS_SPECIFIC_CFG_SIZE;
    header_size += (sconf->chan_config > 0) << 1;                       // chan_config_info
    header_size += avctx->channels          << 1;                       // chan_pos[c]
    header_size += (sconf->crc_enabled > 0) << 2;                       // crc
    if (sconf->ra_flag == RA_FLAG_HEADER && sconf->ra_distance > 0)     // ra_unit_size
        header_size += (sconf->samples / sconf->frame_length + 1) << 2;

    if (!avctx->extradata)
        avctx->extradata = av_mallocz(header_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!avctx->extradata)
        return AVERROR(ENOMEM);

    memset(avctx->extradata, 0, header_size + AV_INPUT_BUFFER_PADDING_SIZE);
    init_put_bits(&ctx->pb, avctx->extradata, header_size + AV_INPUT_BUFFER_PADDING_SIZE);

    // AudioSpecificConfig, reference to ISO/IEC 14496-3 section 1.6.2.1 & 1.6.3
    memset(&m4ac, 0, sizeof(MPEG4AudioConfig));
    m4ac.object_type    = AOT_ALS;
    m4ac.sampling_index = 0x0f;
    m4ac.sample_rate    = avctx->sample_rate;
    m4ac.chan_config    = 0;
    m4ac.sbr            = -1;

    avctx->extradata_size = (header_size + AV_INPUT_BUFFER_PADDING_SIZE);

    config_offset = ff_mpeg4audio_write_config(&m4ac, avctx->extradata,
                                               avctx->extradata_size);

    if (config_offset < 0)
        return config_offset;

    skip_put_bits(&ctx->pb, config_offset);
    // switch(AudioObjectType) -> case 36: ALSSpecificConfig

    // fillBits (align)
    avpriv_align_put_bits(&ctx->pb);

    put_bits32(&ctx->pb,     MKBETAG('A', 'L', 'S', '\0'));
    put_bits32(&ctx->pb,     avctx->sample_rate);
    put_bits32(&ctx->pb,     sconf->samples);
    put_bits  (&ctx->pb, 16, avctx->channels - 1);
    put_bits  (&ctx->pb,  3, 1);                      // original file_type (0 = unknown, 1 = wav, ...)
    put_bits  (&ctx->pb,  3, sconf->resolution);
    put_bits  (&ctx->pb,  1, sconf->floating);
    put_bits  (&ctx->pb,  1, sconf->msb_first);       // msb first (0 = LSB, 1 = MSB)
    put_bits  (&ctx->pb, 16, sconf->frame_length - 1);
    put_bits  (&ctx->pb,  8, sconf->ra_distance);
    put_bits  (&ctx->pb,  2, sconf->ra_flag);
    put_bits  (&ctx->pb,  1, sconf->adapt_order);
    put_bits  (&ctx->pb,  2, sconf->coef_table);
    put_bits  (&ctx->pb,  1, sconf->long_term_prediction);
    put_bits  (&ctx->pb, 10, sconf->max_order);
    put_bits  (&ctx->pb,  2, sconf->block_switching ? FFMAX(1, (sconf->block_switching - 2)) : 0);
    put_bits  (&ctx->pb,  1, sconf->bgmc);
    put_bits  (&ctx->pb,  1, sconf->sb_part);
    put_bits  (&ctx->pb,  1, sconf->joint_stereo);
    put_bits  (&ctx->pb,  1, sconf->mc_coding);
    put_bits  (&ctx->pb,  1, sconf->chan_config);
    put_bits  (&ctx->pb,  1, sconf->chan_sort);
    put_bits  (&ctx->pb,  1, sconf->crc_enabled);    // crc_enabled
    put_bits  (&ctx->pb,  1, sconf->rlslms);
    put_bits  (&ctx->pb,  5, 0);                     // reserved bits
    put_bits  (&ctx->pb,  1, 0);                     // aux_data_enabled (0 = false)

    // align
    avpriv_align_put_bits(&ctx->pb);

    put_bits32(&ctx->pb, 0);                         // original header size
    put_bits32(&ctx->pb, 0);                         // original trailer size
    if (sconf->crc_enabled)
        put_bits32(&ctx->pb, ~ctx->crc);             // CRC

    // writing in local header finished,
    flush_put_bits(&ctx->pb);

    // set the real size
    avctx->extradata_size = put_bits_count(&ctx->pb) >> 3;

    return 0;
}


/**
 * Encode a single frame.
 * @return Overall bit count for the frame
 */
static int encode_frame(AVCodecContext *avctx, const AVPacket *avpkt,
                        const AVFrame *frame)
{
    ALSEncContext *ctx       = avctx->priv_data;
    ALSSpecificConfig *sconf = &ctx->sconf;
    uint8_t *data            = frame->data[0];
    unsigned int b, c;
    int frame_data_size;

    // determine if this is an RA frame
    if (sconf->ra_distance) {
        for (c = 0; c < avctx->channels; c++)
            ctx->blocks[c][0].ra_block = !ctx->ra_counter;

        ctx->ra_counter++;

        if (sconf->ra_distance == ctx->ra_counter)
            ctx->ra_counter = 0;
    }

    // update CRC
    if (sconf->crc_enabled) {
        if (sconf->resolution != 2) {
            frame_data_size = ctx->avctx->bits_per_raw_sample >> 3;
            ctx->crc        = av_crc(ctx->crc_table, ctx->crc, data,
                                     frame->nb_samples * avctx->channels *
                                     frame_data_size);
        } else {
            int i;
            int frame_values = frame->nb_samples * avctx->channels;
            int32_t *samples = (int32_t*) data;
            for (i = 0; i < frame_values; i++) {
                int32_t v = *samples++;
                if (!HAVE_BIGENDIAN)
                    v >>= 8;
                ctx->crc = av_crc(ctx->crc_table, ctx->crc, (uint8_t *)(&v), 3);
            }
        }
 
    }

    // preprocessing
    ctx->avctx->frame_size = frame->nb_samples;
    deinterleave_raw_samples(ctx, data);

    // find optimal encoding parameters

    SET_OPTIONS(STAGE_JOINT_STEREO)
    select_difference_coding_mode(ctx);

    SET_OPTIONS(STAGE_BLOCK_SWITCHING);
    block_partitioning(ctx);

    SET_OPTIONS(STAGE_FINAL);
    if (!sconf->mc_coding || ctx->js_switch) {
        for (b = 0; b < ALS_MAX_BLOCKS; b++) {
            for (c = 0; c < avctx->channels; c++) {
                if (b >= ctx->num_blocks[c])
                    continue;

                if (ctx->independent_bs[c]) {
                    find_block_params(ctx, &ctx->blocks[c][b]);
                } else {
                    find_block_params(ctx, &ctx->blocks[c    ][b]);
                    find_block_params(ctx, &ctx->blocks[c + 1][b]);
                    c++;
                }
            }
        }
    } else {
        // MCC: to be implemented
    }

    // bitstream assembly
    frame_data_size = write_frame(ctx, avpkt, sconf->frame_length * avctx->channels * 32);
    if (frame_data_size < 0)
        av_log(avctx, AV_LOG_ERROR, "Error writing frame\n");

    // update sample count
    if (frame_data_size >= 0)
        sconf->samples += frame->nb_samples;

    // store previous samples
    for (c = 0; c < avctx->channels; c++) {
        memcpy(ctx->raw_samples[c] - sconf->max_order,
                ctx->raw_samples[c] + avctx->frame_size - sconf->max_order,
                sizeof(*ctx->raw_samples[c]) * sconf->max_order);
    }

    return frame_data_size;
}


/**
 * Encode all frames of a random access unit.
 * @return Overall bit count read, AVERROR(x) otherwise
 */
static int als_encode_frame(AVCodecContext *avctx, AVPacket *avpkt,
                             const AVFrame *frame, int *got_packet_ptr)
{

    ALSEncContext *ctx       = avctx->priv_data;
    ALSSpecificConfig *sconf = &ctx->sconf;
    unsigned int encoded;

    if (!frame) {
        int ret;

        ret = write_specific_config(avctx);
        if (ret) {
            return ret;
        }

        if (!ctx->flushed) {

            uint8_t *side_data = av_packet_new_side_data(avpkt, AV_PKT_DATA_NEW_EXTRADATA,
                                                         avctx->extradata_size);
            if (!side_data)
                return AVERROR(ENOMEM);
            memcpy(side_data, avctx->extradata, avctx->extradata_size);

            avpkt->pts      = ctx->next_pts;
            *got_packet_ptr = 1;
            ctx->flushed    = 1;
        }

        return 0;
    }

    // no need to take special care of always/never using ra-frames
    // just encode frame-by-frame
    if (sconf->ra_distance < 2) {
        encoded = encode_frame(avctx, avpkt, frame);
        avpkt->pts      = frame->pts;
        avpkt->duration = ff_samples_to_time_base(avctx, frame->nb_samples);
        avpkt->size     = encoded;
        *got_packet_ptr = 1;

        ctx->next_pts = avpkt->pts + avpkt->duration;
        return 0;
    }

    encoded = encode_frame(avctx, avpkt, frame);

    if (ctx->ra_counter + 1 == sconf->ra_distance ||
        avctx->frame_size   != sconf->frame_length) {
        avpkt->pts      = frame->pts;
        avpkt->duration = ff_samples_to_time_base(avctx, frame->nb_samples);
        avpkt->size     = encoded;
    }
    *got_packet_ptr = 1;
    return 0;
}


/**
 * Rearrange internal order of channels to optimize joint-channel coding.
 */
static void channel_sorting(ALSEncContext *ctx)
{
    // to be implemented...
    // just arrange ctx->raw_samples array
    // according to specific channel order
}


/**
 * Determine the number of samples in each frame, which is constant for all
 * frames in the stream except the very last one which may be smaller.
 */
static void frame_partitioning(ALSEncContext *ctx)
{
    AVCodecContext *avctx    = ctx->avctx;
    ALSSpecificConfig *sconf = &ctx->sconf;

    // choose default frame size if not specified by the user
    if (avctx->frame_size <= 0) {
        if (avctx->sample_rate <= 24000)
            avctx->frame_size = 1024;
        else if(avctx->sample_rate <= 48000)
            avctx->frame_size = 2048;
        else if(avctx->sample_rate <= 96000)
            avctx->frame_size = 4096;
        else
            avctx->frame_size = 8192;

        // increase frame size if block switching is used
        if (sconf->block_switching)
            avctx->frame_size <<= sconf->block_switching >> 1;
    }

    // ensure a certain boundary for the frame size
    // frame length - 1 in ALSSpecificConfig is 16-bit, so max value is 65536
    // frame size == 1 is not allowed because it is used in ffmpeg as a
    // special-case value to indicate PCM audio
    avctx->frame_size   = av_clip(avctx->frame_size, 2, 65536);
    sconf->frame_length = avctx->frame_size;

    // determine distance between ra-frames. 0 = no ra, 1 = all ra
    // defaults to 10s intervals for random access
    sconf->ra_distance = avctx->gop_size;
    /* There is an API issue where the required output audio buffer size cannot
       be known to the user, and the default buffer size in ffmpeg.c is too
       small to consistently fit more than about 7 frames.  Once this issue
       is resolved, the maximum value can be changed from 7 to 255. */
    sconf->ra_distance = av_clip(sconf->ra_distance, 0, 7);
}


/**
 * Determine the ALSSpecificConfig structure used to encode.
 * @return 0 on success, -1 otherwise
 */
static av_cold int get_specific_config(AVCodecContext *avctx)
{
    ALSEncContext *ctx       = avctx->priv_data;
    ALSSpecificConfig *sconf = &ctx->sconf;

    // set default compression level and clip to allowed range
    if (avctx->compression_level == FF_COMPRESSION_DEFAULT)
        avctx->compression_level = 1;
    else
        avctx->compression_level = av_clip(avctx->compression_level, 0, 2);

    // set compression level defaults
    *sconf = *spc_config_settings[avctx->compression_level];

    // total number of samples unknown
    sconf->samples = 0xFFFFFFFF;

    // determine sample format
    switch (avctx->sample_fmt) {
    case AV_SAMPLE_FMT_U8:
        sconf->resolution = 0; break;
    case AV_SAMPLE_FMT_S16:
        sconf->resolution = 1; break;
    case AV_SAMPLE_FMT_FLT:
        sconf->floating   = 1;
        avpriv_report_missing_feature(avctx, "floating-point samples\n");
    case AV_SAMPLE_FMT_S32:
        if (avctx->bits_per_raw_sample <= 24)
            sconf->resolution = 2;
        else
            sconf->resolution = 3;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "unsupported sample format: %s\n",
               av_get_sample_fmt_name(avctx->sample_fmt));
        return -1;
    }

    if (!avctx->bits_per_raw_sample)
        avctx->bits_per_raw_sample = (sconf->resolution + 1) << 3;
    ctx->max_rice_param        = sconf->resolution > 1 ? 31 : 15;

/*
    // user-override for block switching using AVCodecContext.max_partition_order
    if (avctx->max_partition_order >= 0) {
        sconf->block_switching = FFMIN(5, avctx->max_partition_order);
    }
*/
    // determine frame length
    frame_partitioning(ctx);

    // limit the block_switching depth based on whether the full frame length
    // is evenly divisible by the minimum block size.
    while (sconf->block_switching > 0 &&
           sconf->frame_length % (1 << sconf->block_switching)) {
        sconf->block_switching--;
    }

    // determine where to store ra_flag (01: beginning of frame_data)
    // default for now = RA_FLAG_NONE.
    // Using RA_FLAG_FRAMES would make decoding more robust in case of seeking
    // with raw ALS.  However, raw ALS is not supported in FFmpeg yet.
    sconf->ra_flag = RA_FLAG_NONE;

    // determine the coef_table to be used
    sconf->coef_table = (avctx->sample_rate > 48000) +
                        (avctx->sample_rate > 96000);

    // user-specified maximum prediction order
    if (avctx->max_prediction_order >= 0)
        sconf->max_order = av_clip(avctx->max_prediction_order, 0, 1023);

    // user-specified use of BGMC entropy coding mode
    if (avctx->coder_type == FF_CODER_TYPE_AC)
        sconf->bgmc = 1;

    // determine manual channel configuration
    // using avctx->channel_layout
    // to be implemented
    sconf->chan_config      = 0;
    sconf->chan_config_info = 0;

    // determine channel sorting
    // using avctx->channel_layout
    // to be implemented
    sconf->chan_sort = 0;
    sconf->chan_pos  = NULL;

    // Use native-endian sample byte order.
    // We don't really know the original byte order, so this is only done to
    // speed up the CRC calculation.
#if HAVE_BIGENDIAN
    sconf->msb_first = 1;
#else
    sconf->msb_first = 0;
#endif

    // print ALSSpecificConfig info
    ff_als_dprint_specific_config(avctx, sconf);

    return 0;
}


static av_cold int als_encode_end(AVCodecContext *avctx)
{
    int b;
    ALSEncContext *ctx = avctx->priv_data;

    av_freep(&ctx->stages);
    av_freep(&ctx->independent_bs);
    av_freep(&ctx->raw_buffer);
    av_freep(&ctx->raw_samples);
    av_freep(&ctx->raw_dif_buffer);
    av_freep(&ctx->raw_dif_samples);
    av_freep(&ctx->raw_lsb_buffer);
    av_freep(&ctx->raw_lsb_samples);
    av_freep(&ctx->res_buffer);
    av_freep(&ctx->res_samples);
    av_freep(&ctx->block_buffer);
    av_freep(&ctx->blocks);
    av_freep(&ctx->bs_info);
    av_freep(&ctx->num_blocks);
    av_freep(&ctx->bs_sizes_buffer);
    av_freep(&ctx->bs_sizes);
    av_freep(&ctx->js_sizes_buffer);
    av_freep(&ctx->js_sizes);
    av_freep(&ctx->js_infos_buffer);
    av_freep(&ctx->js_infos);
    av_freep(&ctx->q_parcor_coeff_buffer);
    av_freep(&ctx->acf_coeff);
    av_freep(&ctx->parcor_coeff);
    av_freep(&ctx->r_parcor_coeff);
    av_freep(&ctx->lpc_coeff);
    av_freep(&ctx->parcor_error);
    av_freep(&ctx->ltp_buffer);
    av_freep(&ctx->ltp_samples);
    av_freep(&ctx->corr_buffer);
    // av_freep(&ctx->frame_buffer);

    if (ctx->sconf.max_order) {
        for (b = 0; b < 6; b++)
            ff_window_close(&ctx->acf_window[b]);
    }

    av_freep(&avctx->extradata);
    avctx->extradata_size = 0;
    ff_lpc_end(&ctx->lpc);

    return 0;
}


static av_cold int als_encode_init(AVCodecContext *avctx)
{
    ALSEncContext *ctx       = avctx->priv_data;
    ALSSpecificConfig *sconf = &ctx->sconf;
    unsigned int channel_size, channel_offset;
    int ret, b, c;
    int num_bs_sizes;

    ctx->avctx = avctx;

    // determine ALSSpecificConfig
    if (get_specific_config(avctx))
        return -1;

    // write AudioSpecificConfig & ALSSpecificConfig to extradata
    ret = write_specific_config(avctx);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Allocating buffer memory failed.\n");
        als_encode_end(avctx);
        return AVERROR(ENOMEM);
    }

    // initialize sample count
    sconf->samples = 0;

    channel_offset = sconf->long_term_prediction ? ALS_MAX_LTP_LAG : sconf->max_order;
    if (channel_offset & 3)
        channel_offset = (channel_offset & ~3) + 4;
    channel_size   = sconf->frame_length + channel_offset;
    if (channel_size & 3)
        channel_size = (channel_size & ~3) + 4;

    // set up stage options
    ctx->stages = av_malloc_array(NUM_STAGES, sizeof(*ctx->stages));
    if (!ctx->stages) {
        av_log(avctx, AV_LOG_ERROR, "Allocating buffer memory failed.\n");
        als_encode_end(avctx);
        return AVERROR(ENOMEM);
    }

    // fill stage options based on compression level
    ctx->stages[STAGE_JOINT_STEREO]    = *stage_js_settings[avctx->compression_level];
    ctx->stages[STAGE_BLOCK_SWITCHING] = *stage_bs_settings[avctx->compression_level];
    ctx->stages[STAGE_FINAL]           = *stage_final_settings[avctx->compression_level];

    // joint-stereo stage sconf overrides
    ctx->stages[STAGE_JOINT_STEREO].adapt_order = sconf->adapt_order;
    ctx->stages[STAGE_JOINT_STEREO].sb_part     = sconf->sb_part;
    if (avctx->compression_level > 1)
        ctx->stages[STAGE_JOINT_STEREO].max_order = sconf->max_order;
    else
        ctx->stages[STAGE_JOINT_STEREO].max_order = FFMIN(sconf->max_order,
                                    ctx->stages[STAGE_JOINT_STEREO].max_order);

    // block switching stage sconf overrides
    ctx->stages[STAGE_BLOCK_SWITCHING].adapt_order = sconf->adapt_order;
    ctx->stages[STAGE_BLOCK_SWITCHING].sb_part     = sconf->sb_part;
    if (avctx->compression_level > 0)
        ctx->stages[STAGE_BLOCK_SWITCHING].max_order = sconf->max_order;
    else
        ctx->stages[STAGE_BLOCK_SWITCHING].max_order = FFMIN(sconf->max_order,
                                ctx->stages[STAGE_BLOCK_SWITCHING].max_order);

    // final stage sconf overrides
    ctx->stages[STAGE_FINAL].adapt_order = sconf->adapt_order;
    ctx->stages[STAGE_FINAL].sb_part     = sconf->sb_part;
    ctx->stages[STAGE_FINAL].max_order   = sconf->max_order;
    if (sconf->bgmc && avctx->compression_level < 2) {
        ctx->stages[STAGE_FINAL].ecsub_algorithm = EC_SUB_ALGORITHM_RICE_ESTIMATE;
        ctx->stages[STAGE_FINAL].param_algorithm = EC_PARAM_ALGORITHM_BGMC_ESTIMATE;
    }

    // debug print stage options
    av_log(avctx, AV_LOG_DEBUG, "\n");
    if (sconf->joint_stereo) {
        av_log(avctx, AV_LOG_DEBUG, "Joint-Stereo:\n");
        dprint_stage_options(avctx, &ctx->stages[STAGE_JOINT_STEREO]);
    } else {
        av_log(avctx, AV_LOG_DEBUG, "Joint-Stereo: N/A\n");
    }
    av_log(avctx, AV_LOG_DEBUG, "\n");
    if (sconf->block_switching) {
        av_log(avctx, AV_LOG_DEBUG, "Block-Switching:\n");
        dprint_stage_options(avctx, &ctx->stages[STAGE_BLOCK_SWITCHING]);
    } else {
        av_log(avctx, AV_LOG_DEBUG, "Block-Switching: N/A\n");
    }
    av_log(avctx, AV_LOG_DEBUG, "\n");
    av_log(avctx, AV_LOG_DEBUG, "Final:\n");
    dprint_stage_options(avctx, &ctx->stages[STAGE_FINAL]);
    av_log(avctx, AV_LOG_DEBUG, "\n");

    // set cur_stage pointer to the first stage
    ctx->cur_stage = ctx->stages;

    // allocate buffers
    ctx->independent_bs  = av_malloc_array(avctx->channels, sizeof(*ctx->independent_bs));
    ctx->raw_buffer      = av_mallocz_array(avctx->channels * channel_size, sizeof(*ctx->raw_buffer));
    ctx->raw_samples     = av_malloc_array(avctx->channels, sizeof(*ctx->raw_samples));
    ctx->raw_dif_buffer  = av_mallocz_array((avctx->channels >> 1) * channel_size, sizeof(*ctx->raw_dif_buffer));
    ctx->raw_dif_samples = av_malloc_array((avctx->channels >> 1), sizeof(*ctx->raw_dif_samples));
    ctx->raw_lsb_buffer  = av_mallocz_array((avctx->channels) * channel_size, sizeof(*ctx->raw_lsb_buffer));
    ctx->raw_lsb_samples = av_malloc_array(avctx->channels, sizeof(*ctx->raw_lsb_samples));
    ctx->res_buffer      = av_mallocz_array(avctx->channels * channel_size, sizeof(*ctx->res_buffer));
    ctx->res_samples     = av_malloc_array(avctx->channels, sizeof(*ctx->res_samples));
    ctx->num_blocks      = av_malloc_array(avctx->channels, sizeof(*ctx->num_blocks));
    ctx->bs_info         = av_malloc_array(avctx->channels, sizeof(*ctx->bs_info));
    ctx->block_buffer    = av_mallocz_array(avctx->channels * ALS_MAX_BLOCKS, sizeof(*ctx->block_buffer));
    ctx->blocks          = av_malloc_array(avctx->channels, sizeof(*ctx->blocks));

    // check buffers
    if (!ctx->independent_bs    ||
        !ctx->raw_buffer        || !ctx->raw_samples     ||
        !ctx->raw_dif_buffer    || !ctx->raw_dif_samples ||
        !ctx->raw_lsb_buffer    || !ctx->raw_lsb_samples ||
        !ctx->res_buffer        || !ctx->res_samples     ||
        !ctx->num_blocks        || !ctx->bs_info         ||
        !ctx->block_buffer      || !ctx->blocks) {
        av_log(avctx, AV_LOG_ERROR, "Allocating buffer memory failed.\n");
        als_encode_end(avctx);
        return AVERROR(ENOMEM);
    }

    ctx->blocks[0] = ctx->block_buffer;
    for (c = 1; c < avctx->channels; c++)
        ctx->blocks[c] = ctx->blocks[c - 1] + 32;

    // allocate short-term prediction coefficient buffers
    if (sconf->max_order) {
        ctx->q_parcor_coeff_buffer = av_malloc_array(avctx->channels * ALS_MAX_BLOCKS * sconf->max_order, sizeof(*ctx->q_parcor_coeff_buffer));
        ctx->acf_coeff             = av_malloc_array(sconf->max_order + 1, sizeof(*ctx->acf_coeff));
        ctx->parcor_coeff          = av_malloc_array(sconf->max_order, sizeof(*ctx->parcor_coeff));
        ctx->lpc_coeff             = av_malloc_array(sconf->max_order, sizeof(*ctx->lpc_coeff));
        ctx->parcor_error          = av_malloc_array(sconf->max_order, sizeof(*ctx->parcor_error));
        ctx->r_parcor_coeff        = av_malloc_array(sconf->max_order, sizeof(*ctx->r_parcor_coeff));

        if (!ctx->q_parcor_coeff_buffer || !ctx->acf_coeff      ||
            !ctx->parcor_coeff          || !ctx->lpc_coeff      ||
            !ctx->parcor_error          || !ctx->r_parcor_coeff) {
            av_log(avctx, AV_LOG_ERROR, "Allocating buffer memory failed.\n");
            als_encode_end(avctx);
            return AVERROR(ENOMEM);
        }

        ctx->blocks[0][0].q_parcor_coeff = ctx->q_parcor_coeff_buffer;
        for (c = 0; c < avctx->channels; c++) {
            for (b = 0; b < ALS_MAX_BLOCKS; b++) {
                if (b)
                    ctx->blocks[c][b].q_parcor_coeff = ctx->blocks[c][b-1].q_parcor_coeff + sconf->max_order;
                else if (c)
                    ctx->blocks[c][b].q_parcor_coeff = ctx->blocks[c-1][0].q_parcor_coeff + ALS_MAX_BLOCKS * sconf->max_order;
            }
        }
    }

    // allocate long-term prediction buffers
    if (sconf->long_term_prediction) {
        ctx->ltp_buffer = av_malloc_array(avctx->channels * channel_size, sizeof(*ctx->ltp_buffer));
        ctx->ltp_samples = av_malloc_array(avctx->channels, sizeof(*ctx->ltp_samples));

        if (!ctx->ltp_buffer || !ctx->ltp_samples) {
            av_log(avctx, AV_LOG_ERROR, "Allocating buffer memory failed.\n");
            als_encode_end(avctx);
            return AVERROR(ENOMEM);
        }

        // assign ltp buffer pointers
        ctx->ltp_samples[0] = ctx->ltp_buffer + channel_offset;
        for (c = 1; c < avctx->channels; c++)
            ctx->ltp_samples[c] = ctx->ltp_samples[c - 1] + channel_size;
    }

    // allocate autocorrelation buffers (used for short-term and long-term prediction)
    if (sconf->long_term_prediction || sconf->max_order) {
        int corr_pad = FFMIN(ALS_MAX_LTP_LAG, sconf->frame_length);
        corr_pad     = FFMAX(corr_pad, sconf->max_order + 1);
        if (corr_pad & 1)
            corr_pad++;

        ctx->corr_buffer = av_mallocz_array(sconf->frame_length + 1 + corr_pad, sizeof(*ctx->corr_buffer));
        if (!ctx->corr_buffer) {
            av_log(avctx, AV_LOG_ERROR, "Allocating buffer memory failed.\n");
            als_encode_end(avctx);
            return AVERROR(ENOMEM);
        }
        ctx->corr_samples = ctx->corr_buffer + corr_pad;
    }

    // assign buffer pointers
    ctx->raw_samples    [0] = ctx->raw_buffer     + channel_offset;
    ctx->raw_dif_samples[0] = ctx->raw_dif_buffer + channel_offset;
    ctx->raw_lsb_samples[0] = ctx->raw_lsb_buffer + channel_offset;
    ctx->res_samples    [0] = ctx->res_buffer     + channel_offset;

    for (c = 1; c < avctx->channels; c++) {
        ctx->raw_samples    [c] = ctx->raw_samples[c - 1] + channel_size;
        ctx->res_samples    [c] = ctx->res_samples[c - 1] + channel_size;
        ctx->raw_lsb_samples[c] = ctx->raw_lsb_samples[c - 1] + channel_size;
    }

    for (c = 1; c < (avctx->channels >> 1); c++) {
        ctx->raw_dif_samples[c] = ctx->raw_dif_samples[c - 1] + channel_size;
    }

    // channel sorting
    if ((sconf->joint_stereo || sconf->mc_coding) && sconf->chan_sort)
        channel_sorting(ctx);

    // allocate block-switching and joint-stereo buffers
    num_bs_sizes = (2 << sconf->block_switching) - 1;

    ctx->bs_sizes_buffer = av_malloc_array(num_bs_sizes * avctx->channels, sizeof(*ctx->bs_sizes_buffer));
    ctx->bs_sizes        = av_malloc_array(num_bs_sizes * avctx->channels, sizeof(*ctx->bs_sizes));
    ctx->js_sizes_buffer = av_malloc_array(num_bs_sizes * ((avctx->channels + 1) >> 1), sizeof(*ctx->js_sizes_buffer));
    ctx->js_sizes        = av_malloc_array(num_bs_sizes * avctx->channels, sizeof(*ctx->js_sizes));
    ctx->js_infos_buffer = av_malloc_array(num_bs_sizes * ((avctx->channels + 1) >> 1), sizeof(*ctx->js_infos_buffer));
    ctx->js_infos        = av_malloc_array(num_bs_sizes * avctx->channels, sizeof(*ctx->js_infos));

    if (!ctx->bs_sizes || !ctx->bs_sizes_buffer ||
        !ctx->js_sizes || !ctx->js_sizes_buffer ||
        !ctx->js_infos || !ctx->js_infos_buffer) {
        av_log(avctx, AV_LOG_ERROR, "Allocating buffer memory failed.\n");
        als_encode_end(avctx);
        return AVERROR(ENOMEM);
    }

    // assign buffer pointers for block-switching and joint-stereo
    for (c = 0; c < avctx->channels; c++) {
        ctx->bs_sizes[c] = ctx->bs_sizes_buffer + c * num_bs_sizes;
    }

    for (c = 0; c < avctx->channels - 1; c += 2) {
        ctx->js_sizes[c    ] = ctx->js_sizes_buffer + (c    ) * num_bs_sizes;
        ctx->js_sizes[c + 1] = ctx->js_sizes_buffer + (c + 1) * num_bs_sizes;
        ctx->js_infos[c    ] = ctx->js_infos_buffer + (c    ) * num_bs_sizes;
        ctx->js_infos[c + 1] = ctx->js_infos_buffer + (c + 1) * num_bs_sizes;
    }

    // initialize autocorrelation window for each block size
    if (sconf->max_order) {
        for (b = 0; b <= sconf->block_switching; b++) {
            int block_length = sconf->frame_length / (1 << b);
            if (block_length & 1)
                block_length++;

            if (avctx->sample_rate <= 48000)
                ff_window_init(&ctx->acf_window[b], WINDOW_TYPE_SINERECT, block_length, 4.0);
            else
                ff_window_init(&ctx->acf_window[b], WINDOW_TYPE_HANNRECT, block_length, 4.0);

            if (!sconf->block_switching)
                break;
        }
    }

    // initialize CRC calculation
    if (sconf->crc_enabled) {
        ctx->crc_table = av_crc_get_table(AV_CRC_32_IEEE_LE);
        ctx->crc       = 0xFFFFFFFF;
    }

    // allocate local frame buffer if necessary
    if (sconf->ra_distance > 1) {
        // TODO: use realloc() to increase buffer size if necessary
        ctx->frame_buffer_size = sconf->ra_distance * sconf->frame_length *
                                 (avctx->channels * avctx->bits_per_raw_sample / 8) *
                                 5 / 4 + 1024;
    }
    if ((ret = ff_lpc_init(&ctx->lpc, avctx->frame_size, sconf->max_order, FF_LPC_TYPE_FIXED)) < 0)
        return ret;

    return 0;
}


AVCodec ff_als_encoder = {
    .name           = "als",
    .long_name      = NULL_IF_CONFIG_SMALL("MPEG-4 Audio Lossless Coding (ALS)"),
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_MP4ALS,
    .priv_data_size = sizeof(ALSEncContext),
    .init           = als_encode_init,
    .encode2        = als_encode_frame,
    .close          = als_encode_end,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_EXPERIMENTAL,
    .sample_fmts = (const enum AVSampleFormat[]){ AV_SAMPLE_FMT_U8,  AV_SAMPLE_FMT_S16,
                                                AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_NONE },
};
