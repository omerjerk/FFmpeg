/*
 * Copyright (c) 2016 Umair Khan <omerjerk@gmail.com>
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

#ifndef AVUTIL_SOFTFLOAT_IEEE754_H
#define AVUTIL_SOFTFLOAT_IEEE754_H

#include <stdint.h>

#define EXP_BIAS 127
#define MANT_BITS 23

typedef struct SoftFloat_IEEE754 {
    int32_t sign;
    uint64_t mant;
    int32_t  exp;
} SoftFloat_IEEE754;

static const SoftFloat_IEEE754 FLOAT_0 = {0, 0, -126};
static const SoftFloat_IEEE754 FLOAT_1 = {0, 0,    0};

/** Normalize the softfloat as defined by IEEE 754 single-recision floating
 *  point specification
 */
static inline SoftFloat_IEEE754 av_normalize_sf_ieee754(SoftFloat_IEEE754 sf) {
    while( sf.mant >= 0x1000000UL ) {
        sf.exp++;
        sf.mant >>= 1;
    }
    sf.mant &= 0x007fffffUL;
    return sf;
}

/** Convert integer to softfloat.
 *  @return softfloat with value n * 2^e
 */
static inline SoftFloat_IEEE754 av_int2sf_ieee754(int64_t n, int e) {
    int sign = 0;

    if (n < 0) {
        sign = 1;
        n    *= -1;
    }
    return av_normalize_sf_ieee754((SoftFloat_IEEE754) {sign, n << MANT_BITS, 0 + e});
}

/** Make a softfloat out of the bitstream. Assumes the bits are in the form as defined
 *  by the IEEE 754 spec.
 */
static inline SoftFloat_IEEE754 av_bits2sf_ieee754(uint32_t n) {
    return ((SoftFloat_IEEE754) { (n & 0x80000000UL), (n & 0x7FFFFFUL), (n & 0x7F800000UL) });
}

/** Convert the softfloat to integer
 */
static inline int av_sf2int_ieee754(SoftFloat_IEEE754 a) {
    if(a.exp >= 0) return a.mant <<  a.exp ;
    else           return a.mant >>(-a.exp);
}

/** Divide a by b. b should not be zero.
 *  @return normalized result
 */
static inline SoftFloat_IEEE754 av_div_sf_ieee754(SoftFloat_IEEE754 a, SoftFloat_IEEE754 b) {
    int32_t mant, exp, sign;
    a    = av_normalize_sf_ieee754(a);
    b    = av_normalize_sf_ieee754(b);
    sign = a.sign ^ b.sign;
    mant = ((((uint64_t) (a.mant | 0x00800000UL)) << MANT_BITS) / (b.mant| 0x00800000UL));
    exp  = a.exp - b.exp;
    return av_normalize_sf_ieee754((SoftFloat_IEEE754) {sign, mant, exp});
}

/** Multiply a with b
 *  #return normalized result
 */
static inline SoftFloat_IEEE754 av_mul_sf_ieee754(SoftFloat_IEEE754 a, SoftFloat_IEEE754 b) {
    int32_t sign, mant, exp;
    a    = av_normalize_sf_ieee754(a);
    b    = av_normalize_sf_ieee754(b);
    sign = a.sign ^ b.sign;
    mant = (((uint64_t)(a.mant|0x00800000UL) * (uint64_t)(b.mant|0x00800000UL))>>MANT_BITS);
    exp  = a.exp + b.exp;
    return av_normalize_sf_ieee754((SoftFloat_IEEE754) {sign, mant, exp});
}

/** Compare a with b strictly
 *  @returns 1 if the a and b are equal, 0 otherwise.
 */
static inline int av_cmp_sf_ieee754(SoftFloat_IEEE754 a, SoftFloat_IEEE754 b) {
    a = av_normalize_sf_ieee754(a);
    b = av_normalize_sf_ieee754(b);
    if (a.sign != b.sign) return 0;
    if (a.mant != b.mant) return 0;
    if (a.exp  != b.exp ) return 0;
    return 1;
}


/** difference a with b
 * (first stupid version)
 *  #return normalized result
 */
static inline SoftFloat_IEEE754 av_diff_sf_ieee754(SoftFloat_IEEE754 a, SoftFloat_IEEE754 b) {
    int64_t a_temp = 0, b_temp = 0;
    a_temp = a.mant;
    b_temp = b.mant;
    int32_t sign;
    int i;
    a_temp = a_temp | 0x800000UL;// add 24th bit. float = sign * (2 ^ exp) * 1.mant
    b_temp = b_temp | 0x800000UL;

    if(a.exp > b.exp)
        a_temp = a_temp << (a.exp - b.exp); 
    else 
        b_temp = b_temp << (b.exp - a.exp); 
    
    //printf("a = %ld\n", a_temp);
    //printf("b = %ld\n", b_temp);
    if(a.sign == 1)
        a_temp *= -1;
    if(b.sign == 1)
        b_temp *= -1;

    a_temp -= b_temp;

    //printf("res = %ld\n", a_temp);

    if(a_temp >= 0)
        sign = 0;
    else
        sign = 1;

    if(sign) a_temp *= -1;


    i = 31;
    while ((a_temp & (0x1 << i) == 0))
        i--;

    a_temp = a_temp & ~(0x1 << i);

    //printf("i = %d\n", 0x1 << i);
    //printf("res_1 = %ld\n", a_temp);

    return av_normalize_sf_ieee754((SoftFloat_IEEE754) {sign, a.exp > b.exp ? a.exp : b.exp, 128});
}

/**truncation a 
 * 
 */
static inline int av_trunc_sf_ieee754(SoftFloat_IEEE754 a) {
    uint64_t result;
    int i;
    if(a.exp < 127)
        result = 0;
    else
    {
        result = 0x1;
        for(i = 0; i < a.exp - 127; i++)
        {
            result = result << 1;
            if(a.mant & 0x400000UL)
                result += 0x1;
            a.mant = a.mant << 1;
        }
    }

    if(a.sign == 0)
        return result;
    else
        return (-1) * result;
}


static inline void test_trunc()
{
    SoftFloat_IEEE754 a, b, c;
    a.sign = 1; //-5894.78125
    a.exp = 139;
    a.mant = 3683904;

    b.sign = 0;//24.875
    b.exp = 131;
    b.mant = 4653056;

    c = av_diff_sf_ieee754(a, b);

    //printf("%d\n",av_trunc_sf_ieee754(a));
    
    printf("%d\n%d\n%u\n",c.sign,c.exp, c.mant);


}


#endif /*AVUTIL_SOFTFLOAT_IEEE754_H*/

