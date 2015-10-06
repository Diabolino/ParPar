/*
 * GF-Complete: A Comprehensive Open Source Library for Galois Field Arithmetic
 * James S. Plank, Ethan L. Miller, Kevin M. Greenan,
 * Benjamin A. Arnold, John A. Burnum, Adam W. Disney, Allen C. McBride.
 *
 * gf_w16.c
 *
 * Routines for 16-bit Galois fields
 */

#include "gf_int.h"
#include <stdio.h>
#include <stdlib.h>
#include "gf_w16.h"
#include "platform.h"
#include "gf_w16_additions.c"

#ifdef _MSC_VER
#define inline __inline
#endif

/* #define GF_FIRST_BIT (1 << 15)
#define GF_MULTBY_TWO(p) (((p) & GF_FIRST_BIT) ? (((p) << 1) ^ h->prim_poly) : (p) << 1) */
#define GF_MULTBY_TWO(p) (((p) << 1) ^ (h->prim_poly & -((p) >> 15)))


/* KMG: GF_MULT_LOGTABLE: */

static
void
gf_w16_log_multiply_region(gf_t *gf, void *src, void *dest, gf_val_32_t val, int bytes, int xor)
{
  uint16_t *s16, *d16;
  int lv;
  struct gf_w16_logtable_data *ltd;
  gf_region_data rd;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 2, 2);
  gf_do_initial_region_alignment(&rd);

  ltd = (struct gf_w16_logtable_data *) ((gf_internal_t *) gf->scratch)->private;
  s16 = (uint16_t *) rd.s_start;
  d16 = (uint16_t *) rd.d_start;

  lv = ltd->log_tbl[val];

  if (xor) {
    while (d16 < (uint16_t *) rd.d_top) {
      *d16 ^= (*s16 == 0 ? 0 : ltd->antilog_tbl[lv + ltd->log_tbl[*s16]]);
      d16++;
      s16++;
    }
  } else {
    while (d16 < (uint16_t *) rd.d_top) {
      *d16 = (*s16 == 0 ? 0 : ltd->antilog_tbl[lv + ltd->log_tbl[*s16]]);
      d16++;
      s16++;
    }
  }
  gf_do_final_region_alignment(&rd);
}

static
inline
gf_val_32_t
gf_w16_log_multiply(gf_t *gf, gf_val_32_t a, gf_val_32_t b)
{
  struct gf_w16_logtable_data *ltd;

  ltd = (struct gf_w16_logtable_data *) ((gf_internal_t *) gf->scratch)->private;
  return (a == 0 || b == 0) ? 0 : ltd->antilog_tbl[(int) ltd->log_tbl[a] + (int) ltd->log_tbl[b]];
}

static
int gf_w16_log_init(gf_t *gf)
{
  gf_internal_t *h;
  struct gf_w16_logtable_data *ltd;
  int i, b;

  h = (gf_internal_t *) gf->scratch;
  ltd = h->private;
  
  for (i = 0; i < GF_MULT_GROUP_SIZE+1; i++)
    ltd->log_tbl[i] = 0;
  ltd->d_antilog = ltd->antilog_tbl + GF_MULT_GROUP_SIZE;

  b = 1;
  for (i = 0; i < GF_MULT_GROUP_SIZE; i++) {
      ltd->log_tbl[b] = i;
      ltd->antilog_tbl[i] = b;
      ltd->antilog_tbl[i+GF_MULT_GROUP_SIZE] = b;
      b <<= 1;
      if (b & GF_FIELD_SIZE) {
          b = b ^ h->prim_poly;
      }
  }

  ltd->inv_tbl[0] = 0;  /* Not really, but we need to fill it with something  */
  ltd->inv_tbl[1] = 1;
  for (i = 2; i < GF_FIELD_SIZE; i++) {
    ltd->inv_tbl[i] = ltd->antilog_tbl[GF_MULT_GROUP_SIZE-ltd->log_tbl[i]];
  }

  gf->multiply.w32 = gf_w16_log_multiply;
  gf->multiply_region.w32 = gf_w16_log_multiply_region;

  return 1;
}

/* JSP: GF_MULT_SPLIT_TABLE: Using 8 multiplication tables to leverage SSE instructions.
*/


/* Ben: Does alternate mapping multiplication using a split table in the
 lazy method without sse instructions*/

static 
void
gf_w16_split_4_16_lazy_nosse_altmap_multiply_region(gf_t *gf, void *src, void *dest, gf_val_32_t val, int bytes, int xor)
{
  uint64_t i, j, c, prod;
  uint8_t *s8, *d8, *top;
  uint16_t table[4][16];
  gf_region_data rd;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 16, 32);
  gf_do_initial_region_alignment(&rd);    

  /*Ben: Constructs lazy multiplication table*/

  for (j = 0; j < 16; j++) {
    for (i = 0; i < 4; i++) {
      c = (j << (i*4));
      table[i][j] = gf->multiply.w32(gf, c, val);
    }
  }

  /*Ben: s8 is the start of source, d8 is the start of dest, top is end of dest region. */
  
  s8 = (uint8_t *) rd.s_start;
  d8 = (uint8_t *) rd.d_start;
  top = (uint8_t *) rd.d_top;


  while (d8 < top) {
    
    /*Ben: Multiplies across 16 two byte quantities using alternate mapping 
       high bits are on the left, low bits are on the right. */
  
    for (j=0;j<16;j++) {
    
      /*Ben: If the xor flag is set, the product should include what is in dest */
      prod = (xor) ? ((uint16_t)(*d8)<<8) ^ *(d8+16) : 0;

      /*Ben: xors all 4 table lookups into the product variable*/
      
      prod ^= ((table[0][*(s8+16)&0xf]) ^
          (table[1][(*(s8+16)&0xf0)>>4]) ^
          (table[2][*(s8)&0xf]) ^
          (table[3][(*(s8)&0xf0)>>4]));

      /*Ben: Stores product in the destination and moves on*/
      
      *d8 = (uint8_t)(prod >> 8);
      *(d8+16) = (uint8_t)(prod & 0x00ff);
      s8++;
      d8++;
    }
    s8+=16;
    d8+=16;
  }
  gf_do_final_region_alignment(&rd);
}

static
  void
gf_w16_split_4_16_lazy_multiply_region(gf_t *gf, void *src, void *dest, gf_val_32_t val, int bytes, int xor)
{
  uint64_t i, j, a, c, prod;
  uint16_t *s16, *d16, *top;
  uint16_t table[4][16];
  gf_region_data rd;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 2, 2);
  gf_do_initial_region_alignment(&rd);    

  for (j = 0; j < 16; j++) {
    for (i = 0; i < 4; i++) {
      c = (j << (i*4));
      table[i][j] = gf->multiply.w32(gf, c, val);
    }
  }

  s16 = (uint16_t *) rd.s_start;
  d16 = (uint16_t *) rd.d_start;
  top = (uint16_t *) rd.d_top;

  while (d16 < top) {
    a = *s16;
    prod = (xor) ? *d16 : 0;
    for (i = 0; i < 4; i++) {
      prod ^= table[i][a&0xf];
      a >>= 4;
    }
    *d16 = prod;
    s16++;
    d16++;
  }
}

static
void
gf_w16_split_8_16_lazy_multiply_region(gf_t *gf, void *src, void *dest, gf_val_32_t val, int bytes, int xor)
{
  FAST_U32 j, k, v, *d64, *top64;
  unsigned char *s8;
  gf_internal_t *h;
  FAST_U32 htable[256], ltable[256];
  gf_region_data rd;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  gf_w16_log_region_alignment(&rd, gf, src, dest, bytes, val, xor, sizeof(FAST_U32), sizeof(FAST_U32));
  
  h = (gf_internal_t *) gf->scratch;

  v = val;
  ltable[0] = 0;
  for (j = 1; j < 256; j <<= 1) {
    for (k = 0; k < j; k++) ltable[k^j] = (v ^ ltable[k]);
    v = GF_MULTBY_TWO(v);
  }
  htable[0] = 0;
  for (j = 1; j < 256; j <<= 1) {
    for (k = 0; k < j; k++) htable[k^j] = (v ^ htable[k]);
    v = GF_MULTBY_TWO(v);
  }

  s8 = (unsigned char *) rd.s_start;
  d64 = (FAST_U32 *) rd.d_start;
  top64 = (FAST_U32 *) rd.d_top;
    
  if (xor) {
    while (d64 != top64) {
#if FAST_U32_SIZE == 4 || FAST_U32_SIZE == 8
      *d64 ^=
          ltable[s8[0]] ^ htable[s8[1]] ^
        ((ltable[s8[2]] ^ htable[s8[3]]) << 16)
  #if FAST_U32_SIZE == 8
      ^ ((ltable[s8[4]] ^ htable[s8[5]]) << 32) ^
        ((ltable[s8[6]] ^ htable[s8[7]]) << 48)
  #endif
      ;
#else
      FAST_U32 prod = 0;
      for (j = sizeof(FAST_U32); j > 0; j-=2) {
        prod <<= 16;
        prod ^= ltable[s8[j]] ^ htable[s8[j+1]];
      }
      d64 ^= prod;
#endif
      s8 += sizeof(FAST_U32);
      d64++;
    }
  }
  else
    while (d64 != top64) {
#if FAST_U32_SIZE == 4 || FAST_U32_SIZE == 8
      *d64 =
          ltable[s8[0]] ^ htable[s8[1]] ^
        ((ltable[s8[2]] ^ htable[s8[3]]) << 16)
  #if FAST_U32_SIZE == 8
      ^ ((ltable[s8[4]] ^ htable[s8[5]]) << 32) ^
        ((ltable[s8[6]] ^ htable[s8[7]]) << 48)
  #endif
      ;
#else
      FAST_U32 prod = 0;
      for (j = sizeof(FAST_U32); j > 0; j-=2) {
        prod <<= 16;
        prod ^= ltable[s8[j]] ^ htable[s8[j+1]];
      }
      d64 = prod;
#endif
      s8 += sizeof(FAST_U32);
      d64++;
    }
}

static
void
gf_w16_split_4_16_lazy_sse_multiply_region(gf_t *gf, void *src, void *dest, gf_val_32_t val, int bytes, int xor)
{
#ifdef INTEL_SSSE3
  uint64_t i, j, *s64, *d64, *top64;
  uint64_t c, prod;
  uint8_t low[4][16];
  uint8_t high[4][16];
  gf_region_data rd;

  __m128i  mask, ta, tb, ti, tpl, tph, tlow[4], thigh[4], tta, ttb, lmask;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 16, 32);
  gf_do_initial_region_alignment(&rd);

/*
  lmask = _mm_set1_epi16 (0xff);
  for (i = 0; i < 16; i+=4) {
    ta = _mm_set_epi16(
      ltd->antilog_tbl[(int) ltd->log_tbl[7 << i] + log_val],
      ltd->antilog_tbl[(int) ltd->log_tbl[6 << i] + log_val],
      ltd->antilog_tbl[(int) ltd->log_tbl[5 << i] + log_val],
      ltd->antilog_tbl[(int) ltd->log_tbl[4 << i] + log_val],
      ltd->antilog_tbl[(int) ltd->log_tbl[3 << i] + log_val],
      ltd->antilog_tbl[(int) ltd->log_tbl[2 << i] + log_val],
      ltd->antilog_tbl[(int) ltd->log_tbl[1 << i] + log_val],
      0
    );
    tb = _mm_set_epi16(
      ltd->antilog_tbl[(int) ltd->log_tbl[15 << i] + log_val],
      ltd->antilog_tbl[(int) ltd->log_tbl[14 << i] + log_val],
      ltd->antilog_tbl[(int) ltd->log_tbl[13 << i] + log_val],
      ltd->antilog_tbl[(int) ltd->log_tbl[12 << i] + log_val],
      ltd->antilog_tbl[(int) ltd->log_tbl[11 << i] + log_val],
      ltd->antilog_tbl[(int) ltd->log_tbl[10 << i] + log_val],
      ltd->antilog_tbl[(int) ltd->log_tbl[ 9 << i] + log_val],
      ltd->antilog_tbl[(int) ltd->log_tbl[ 8 << i] + log_val]
    );
    tlow[i>>2] = _mm_packus_epi16(_mm_and_si128(ta, lmask), _mm_and_si128(tb, lmask));
    thigh[i>>2] = _mm_packus_epi16(_mm_srli_epi16(ta, 8), _mm_srli_epi16(tb, 8));
  }
*/
  for (j = 0; j < 16; j++) {
    for (i = 0; i < 4; i++) {
      c = (j << (i*4));
      prod = gf->multiply.w32(gf, c, val);
      low[i][j] = (prod & 0xff);
      high[i][j] = (prod >> 8);
    }
  }

  for (i = 0; i < 4; i++) {
    tlow[i] = _mm_loadu_si128((__m128i *)low[i]);
    thigh[i] = _mm_loadu_si128((__m128i *)high[i]);
  }

  s64 = (uint64_t *) rd.s_start;
  d64 = (uint64_t *) rd.d_start;
  top64 = (uint64_t *) rd.d_top;

  mask = _mm_set1_epi8 (0x0f);
  lmask = _mm_set1_epi16 (0xff);

  if (xor) {
    while (d64 != top64) {
      
      ta = _mm_load_si128((__m128i *) s64);
      tb = _mm_load_si128((__m128i *) (s64+2));

      tta = _mm_srli_epi16(ta, 8);
      ttb = _mm_srli_epi16(tb, 8);
      tpl = _mm_and_si128(tb, lmask);
      tph = _mm_and_si128(ta, lmask);

      tb = _mm_packus_epi16(tpl, tph);
      ta = _mm_packus_epi16(ttb, tta);

      ti = _mm_and_si128 (mask, tb);
      tph = _mm_shuffle_epi8 (thigh[0], ti);
      tpl = _mm_shuffle_epi8 (tlow[0], ti);
  
      tb = _mm_srli_epi16(tb, 4);
      ti = _mm_and_si128 (mask, tb);
      tpl = _mm_xor_si128(_mm_shuffle_epi8 (tlow[1], ti), tpl);
      tph = _mm_xor_si128(_mm_shuffle_epi8 (thigh[1], ti), tph);

      ti = _mm_and_si128 (mask, ta);
      tpl = _mm_xor_si128(_mm_shuffle_epi8 (tlow[2], ti), tpl);
      tph = _mm_xor_si128(_mm_shuffle_epi8 (thigh[2], ti), tph);
  
      ta = _mm_srli_epi16(ta, 4);
      ti = _mm_and_si128 (mask, ta);
      tpl = _mm_xor_si128(_mm_shuffle_epi8 (tlow[3], ti), tpl);
      tph = _mm_xor_si128(_mm_shuffle_epi8 (thigh[3], ti), tph);

      ta = _mm_unpackhi_epi8(tpl, tph);
      tb = _mm_unpacklo_epi8(tpl, tph);

      tta = _mm_load_si128((__m128i *) d64);
      ta = _mm_xor_si128(ta, tta);
      ttb = _mm_load_si128((__m128i *) (d64+2));
      tb = _mm_xor_si128(tb, ttb); 
      _mm_store_si128 ((__m128i *)d64, ta);
      _mm_store_si128 ((__m128i *)(d64+2), tb);

      d64 += 4;
      s64 += 4;
      
    }
  } else {
    while (d64 != top64) {
      
      ta = _mm_load_si128((__m128i *) s64);
      tb = _mm_load_si128((__m128i *) (s64+2));

      tta = _mm_srli_epi16(ta, 8);
      ttb = _mm_srli_epi16(tb, 8);
      tpl = _mm_and_si128(tb, lmask);
      tph = _mm_and_si128(ta, lmask);

      tb = _mm_packus_epi16(tpl, tph);
      ta = _mm_packus_epi16(ttb, tta);

      ti = _mm_and_si128 (mask, tb);
      tph = _mm_shuffle_epi8 (thigh[0], ti);
      tpl = _mm_shuffle_epi8 (tlow[0], ti);
  
      tb = _mm_srli_epi16(tb, 4);
      ti = _mm_and_si128 (mask, tb);
      tpl = _mm_xor_si128(_mm_shuffle_epi8 (tlow[1], ti), tpl);
      tph = _mm_xor_si128(_mm_shuffle_epi8 (thigh[1], ti), tph);

      ti = _mm_and_si128 (mask, ta);
      tpl = _mm_xor_si128(_mm_shuffle_epi8 (tlow[2], ti), tpl);
      tph = _mm_xor_si128(_mm_shuffle_epi8 (thigh[2], ti), tph);
  
      ta = _mm_srli_epi16(ta, 4);
      ti = _mm_and_si128 (mask, ta);
      tpl = _mm_xor_si128(_mm_shuffle_epi8 (tlow[3], ti), tpl);
      tph = _mm_xor_si128(_mm_shuffle_epi8 (thigh[3], ti), tph);

      ta = _mm_unpackhi_epi8(tpl, tph);
      tb = _mm_unpacklo_epi8(tpl, tph);

      _mm_store_si128 ((__m128i *)d64, ta);
      _mm_store_si128 ((__m128i *)(d64+2), tb);

      d64 += 4;
      s64 += 4;
    }
  }

  gf_do_final_region_alignment(&rd);
#endif
}

/*
static
void
gf_w16_split_4_16_lazy_sse_altmap_multiply_region(gf_t *gf, void *src, void *dest, gf_val_32_t val, int bytes, int xor)
{
#ifdef INTEL_SSSE3
  uint64_t i, j, *s64, *d64, *top64;
  uint64_t c, prod;
  uint8_t low[4][16];
  uint8_t high[4][16];
  gf_region_data rd;
  __m128i  mask, ta, tb, ti, tpl, tph, tlow[4], thigh[4];

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 16, 32);
  gf_do_initial_region_alignment(&rd);

  for (j = 0; j < 16; j++) {
    for (i = 0; i < 4; i++) {
      c = (j << (i*4));
      prod = gf->multiply.w32(gf, c, val);
      low[i][j] = (prod & 0xff);
      high[i][j] = (prod >> 8);
    }
  }

  for (i = 0; i < 4; i++) {
    tlow[i] = _mm_loadu_si128((__m128i *)low[i]);
    thigh[i] = _mm_loadu_si128((__m128i *)high[i]);
  }

  s64 = (uint64_t *) rd.s_start;
  d64 = (uint64_t *) rd.d_start;
  top64 = (uint64_t *) rd.d_top;

  mask = _mm_set1_epi8 (0x0f);

  if (xor) {
    while (d64 != top64) {

      ta = _mm_load_si128((__m128i *) s64);
      tb = _mm_load_si128((__m128i *) (s64+2));

      ti = _mm_and_si128 (mask, tb);
      tph = _mm_shuffle_epi8 (thigh[0], ti);
      tpl = _mm_shuffle_epi8 (tlow[0], ti);
  
      tb = _mm_srli_epi16(tb, 4);
      ti = _mm_and_si128 (mask, tb);
      tpl = _mm_xor_si128(_mm_shuffle_epi8 (tlow[1], ti), tpl);
      tph = _mm_xor_si128(_mm_shuffle_epi8 (thigh[1], ti), tph);

      ti = _mm_and_si128 (mask, ta);
      tpl = _mm_xor_si128(_mm_shuffle_epi8 (tlow[2], ti), tpl);
      tph = _mm_xor_si128(_mm_shuffle_epi8 (thigh[2], ti), tph);
  
      ta = _mm_srli_epi16(ta, 4);
      ti = _mm_and_si128 (mask, ta);
      tpl = _mm_xor_si128(_mm_shuffle_epi8 (tlow[3], ti), tpl);
      tph = _mm_xor_si128(_mm_shuffle_epi8 (thigh[3], ti), tph);

      ta = _mm_load_si128((__m128i *) d64);
      tph = _mm_xor_si128(tph, ta);
      _mm_store_si128 ((__m128i *)d64, tph);
      tb = _mm_load_si128((__m128i *) (d64+2));
      tpl = _mm_xor_si128(tpl, tb);
      _mm_store_si128 ((__m128i *)(d64+2), tpl);

      d64 += 4;
      s64 += 4;
    }
  } else {
    while (d64 != top64) {

      ta = _mm_load_si128((__m128i *) s64);
      tb = _mm_load_si128((__m128i *) (s64+2));

      ti = _mm_and_si128 (mask, tb);
      tph = _mm_shuffle_epi8 (thigh[0], ti);
      tpl = _mm_shuffle_epi8 (tlow[0], ti);
  
      tb = _mm_srli_epi16(tb, 4);
      ti = _mm_and_si128 (mask, tb);
      tpl = _mm_xor_si128(_mm_shuffle_epi8 (tlow[1], ti), tpl);
      tph = _mm_xor_si128(_mm_shuffle_epi8 (thigh[1], ti), tph);

      ti = _mm_and_si128 (mask, ta);
      tpl = _mm_xor_si128(_mm_shuffle_epi8 (tlow[2], ti), tpl);
      tph = _mm_xor_si128(_mm_shuffle_epi8 (thigh[2], ti), tph);
  
      ta = _mm_srli_epi16(ta, 4);
      ti = _mm_and_si128 (mask, ta);
      tpl = _mm_xor_si128(_mm_shuffle_epi8 (tlow[3], ti), tpl);
      tph = _mm_xor_si128(_mm_shuffle_epi8 (thigh[3], ti), tph);

      _mm_store_si128 ((__m128i *)d64, tph);
      _mm_store_si128 ((__m128i *)(d64+2), tpl);

      d64 += 4;
      s64 += 4;
      
    }
  }
  gf_do_final_region_alignment(&rd);

#endif
}
*/

#ifdef ARM_NEON
#include "gf_w16_neon.c"
#endif

static 
int gf_w16_split_init(gf_t *gf)
{
  gf_internal_t *h;

  h = (gf_internal_t *) gf->scratch;

  /* We'll be using LOG for multiplication, unless the pp isn't primitive.
     In that case, we'll be using SHIFT. */

  gf_w16_log_init(gf);

#ifdef ARM_NEON
  gf_w16_neon_split_init(gf);
  gf->mult_method = GF_SPLIT4_NEON;
  
  /* TODO: altmap stuff */
  gf->altmap_region = gf_w16_split_null;
  gf->unaltmap_region = gf_w16_split_null;
  
  gf->alignment = 16;
  gf->walignment = 32;
  gf->using_altmap = (h->region_type & GF_REGION_ALTMAP);
  
#else
  
  /* Defaults */
  if (!h->arg1 || !h->arg2) {
    h->arg1 = has_ssse3 ? 4 : 8;
    h->arg2 = 16;
  }

  if ((h->arg1 == 8 && h->arg2 == 16) || (h->arg2 == 8 && h->arg1 == 16)) {
    gf->multiply_region.w32 = gf_w16_split_8_16_lazy_multiply_region;
    gf->mult_method = GF_SPLIT8;
    gf->alignment = sizeof(FAST_U32);
    gf->walignment = sizeof(FAST_U32);
  } else if ((h->arg1 == 4 && h->arg2 == 16) || (h->arg2 == 4 && h->arg1 == 16)) {
    gf->mult_method = GF_SPLIT4;
    gf->alignment = 16;
    if (has_ssse3) {
      if(h->region_type & GF_REGION_ALTMAP && h->region_type & GF_REGION_NOSIMD)
        gf->multiply_region.w32 = gf_w16_split_4_16_lazy_nosse_altmap_multiply_region;
      else if(h->region_type & GF_REGION_NOSIMD)
        gf->multiply_region.w32 = gf_w16_split_4_16_lazy_multiply_region;
#ifdef INTEL_SSSE3
      else if(h->region_type & GF_REGION_ALTMAP && has_ssse3) {
        FUNC_ASSIGN(gf->multiply_region.w32, gf_w16_split_4_16_lazy_altmap_multiply_region)
        FUNC_ASSIGN(gf->multiply_regionX.w16, gf_w16_split_4_16_lazy_altmap_multiply_regionX)
        if(has_avx512bw) {
          gf->alignment = 64;
          gf->mult_method = GF_SPLIT4_AVX512;
        } else if(has_avx2) {
          gf->alignment = 32;
          gf->mult_method = GF_SPLIT4_AVX2;
        } else {
          gf->alignment = 16;
          gf->mult_method = GF_SPLIT4_SSSE3;
        }
      }
#endif
    } else {
      if(h->region_type & GF_REGION_SIMD)
        return 0;
      else if(h->region_type & GF_REGION_ALTMAP)
        gf->multiply_region.w32 = gf_w16_split_4_16_lazy_nosse_altmap_multiply_region;
      else
        gf->multiply_region.w32 = gf_w16_split_4_16_lazy_multiply_region;
    }
    gf->walignment = gf->alignment << 1;
  }

  if ((h->region_type & GF_REGION_ALTMAP) && gf->multiply_region.w32 != gf_w16_split_8_16_lazy_multiply_region) {
    /* !! There's no fallback if SSE not supported !!
     * ParPar never uses ALTMAP if SSSE3 isn't available, but this isn't ideal in gf-complete
     * Also: ALTMAP implementations differ on SSE/AVX support, so it doesn't make too much sense for a fallback */
#ifdef FUNC_ASSIGN
    FUNC_ASSIGN(gf->altmap_region, gf_w16_split_start)
    FUNC_ASSIGN(gf->unaltmap_region, gf_w16_split_final)
#endif
    gf->using_altmap = 1;
  } else {
    gf->altmap_region = gf_w16_split_null;
    gf->unaltmap_region = gf_w16_split_null;
    gf->using_altmap = 0;
  }
  
#endif /* ARM_NEON */

  return 1;
}

#ifdef INTEL_SSE2
#include "x86_jit.c"
static 
int gf_w16_xor_init(gf_t *gf)
{
  gf_internal_t *h = (gf_internal_t *) gf->scratch;
  jit_t* jit = &(h->jit);

  /* We'll be using LOG for multiplication, unless the pp isn't primitive.
     In that case, we'll be using SHIFT. */

  gf_w16_log_init(gf);
  
  /* alloc JIT region */
  jit->code = jit_alloc(jit->len = 4096); /* 4KB should be enough for everyone */
  
  /* if JIT allocation was successful (no W^X issue), use slightly faster JIT version, otherwise fall back to static code version */
  gf->multiply_region.w32 = jit->code ? gf_w16_xor_lazy_sse_jit_altmap_multiply_region : gf_w16_xor_lazy_sse_altmap_multiply_region;
  gf->mult_method = jit->code ? GF_XOR_JIT_SSE2 : GF_XOR_SSE2;
  gf->alignment = 16;
  gf->walignment = 256;
  gf->using_altmap = 1;
  gf->altmap_region = gf_w16_xor_start;
  gf->unaltmap_region = gf_w16_xor_final;
  return 1;
}
#endif

int gf_w16_scratch_size(int mult_type, int region_type, int divide_type, int arg1, int arg2)
{
  return sizeof(gf_internal_t) + sizeof(struct gf_w16_logtable_data) + 64;
}



int gf_w16_init(gf_t *gf)
{
  gf_internal_t *h;

  detect_cpu();

  h = (gf_internal_t *) gf->scratch;

  /* Allen: set default primitive polynomial / irreducible polynomial if needed */

  if (h->prim_poly == 0) {

     /* Allen: use the following primitive polynomial to make 
               carryless multiply work more efficiently for GF(2^16).

        h->prim_poly = 0x1002d;

        The following is the traditional primitive polynomial for GF(2^16) */

      h->prim_poly = 0x1100b;
  }

  gf->multiply.w32 = NULL;
  gf->multiply_region.w32 = NULL;
  gf->multiply_regionX.w16 = NULL;
  gf->alignment = 16;
  gf->walignment = 16;
  gf->using_altmap = 0;

  /* select an appropriate default - always use some variant of SPLIT unless SSSE3 is unavailable but SSE2 is */
#ifdef INTEL_SSE2
  if((h->mult_type == GF_MULT_DEFAULT && !has_ssse3 && (h->region_type & GF_REGION_ALTMAP)) || h->mult_type == GF_MULT_XOR_DEPENDS) {
    return gf_w16_xor_init(gf);
  }
  else
#endif
    return gf_w16_split_init(gf);
}
