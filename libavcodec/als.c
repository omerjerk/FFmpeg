/*
 * MPEG-4 ALS shared functions
 * Copyright (c) 2010 Thilo Borgmann <thilo.borgmann _at_ mail.de>
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
 * @file libavcodec/als.c
 * MPEG-4 ALS functions shared between ALS decoder and ALS encoder
 * @author Thilo Borgmann <thilo.borgmann _at_ mail.de>
 */

#include "avcodec.h"
#include "als.h"
#include "mathops.h"


void ff_als_dprint_specific_config(AVCodecContext *avctx,
                                   ALSSpecificConfig *sconf)
{
#ifdef DEBUG
    av_log(avctx, AV_LOG_DEBUG, "samples = %i\n",              sconf->samples);
    av_log(avctx, AV_LOG_DEBUG, "resolution = %i\n",           sconf->resolution);
    av_log(avctx, AV_LOG_DEBUG, "floating = %i\n",             sconf->floating);
    av_log(avctx, AV_LOG_DEBUG, "msb_first = %i\n",            sconf->msb_first);
    av_log(avctx, AV_LOG_DEBUG, "frame_length = %i\n",         sconf->frame_length);
    av_log(avctx, AV_LOG_DEBUG, "ra_distance = %i\n",          sconf->ra_distance);
    av_log(avctx, AV_LOG_DEBUG, "ra_flag = %i\n",              sconf->ra_flag);
    av_log(avctx, AV_LOG_DEBUG, "adapt_order = %i\n",          sconf->adapt_order);
    av_log(avctx, AV_LOG_DEBUG, "coef_table = %i\n",           sconf->coef_table);
    av_log(avctx, AV_LOG_DEBUG, "long_term_prediction = %i\n", sconf->long_term_prediction);
    av_log(avctx, AV_LOG_DEBUG, "max_order = %i\n",            sconf->max_order);
    av_log(avctx, AV_LOG_DEBUG, "block_switching = %i\n",      sconf->block_switching);
    av_log(avctx, AV_LOG_DEBUG, "bgmc = %i\n",                 sconf->bgmc);
    av_log(avctx, AV_LOG_DEBUG, "sb_part = %i\n",              sconf->sb_part);
    av_log(avctx, AV_LOG_DEBUG, "joint_stereo = %i\n",         sconf->joint_stereo);
    av_log(avctx, AV_LOG_DEBUG, "mc_coding = %i\n",            sconf->mc_coding);
    av_log(avctx, AV_LOG_DEBUG, "chan_config = %i\n",          sconf->chan_config);
    av_log(avctx, AV_LOG_DEBUG, "chan_sort = %i\n",            sconf->chan_sort);
    av_log(avctx, AV_LOG_DEBUG, "RLSLMS = %i\n",               sconf->rlslms);
    av_log(avctx, AV_LOG_DEBUG, "chan_config_info = %i\n",     sconf->chan_config_info);
    av_log(avctx, AV_LOG_DEBUG, "crc_enabled = %i\n",          sconf->crc_enabled);
#endif
}


int ff_als_parcor_to_lpc(unsigned int k, const int32_t *par, int32_t *cof)
{
    int i, j;

    for (i = 0, j = k - 1; i < j; i++, j--) {
        int64_t tmp1, tmp2;
        tmp1 = cof[i] + ((MUL64(par[k], cof[j]) + (1 << 19)) >> 20);
        if (tmp1 > INT32_MAX || tmp1 < INT32_MIN)
            return -1;
        tmp2 = cof[j] + ((MUL64(par[k], cof[i]) + (1 << 19)) >> 20);
        if (tmp2 > INT32_MAX || tmp2 < INT32_MIN)
            return -1;
        cof[j]  = tmp2;
        cof[i]  = tmp1;
    }
    if (i == j) {
        int64_t tmp1 = cof[i] + ((MUL64(par[k], cof[j]) + (1 << 19)) >> 20);
        if (tmp1 > INT32_MAX || tmp1 < INT32_MIN)
            return -1;
        cof[i] = tmp1;
    }

    cof[k] = par[k];

    return 0;
}

void ff_als_parse_bs_info(const uint32_t bs_info, unsigned int n,
                          unsigned int div, unsigned int **div_blocks,
                          unsigned int *num_blocks)
{
    if (n < 31 && ((bs_info << n) & 0x40000000)) {
        // if the level is valid and the investigated bit n is set
        // then recursively check both children at bits (2n+1) and (2n+2)
        n   *= 2;
        div += 1;
        ff_als_parse_bs_info(bs_info, n + 1, div, div_blocks, num_blocks);
        ff_als_parse_bs_info(bs_info, n + 2, div, div_blocks, num_blocks);
    } else {
        // else the bit is not set or the last level has been reached
        // (bit implicitly not set)
        **div_blocks = div;
        (*div_blocks)++;
        (*num_blocks)++;
    }
}
