#include "miniz.h"

#define MZ_MAXBITS 15

typedef struct
{
    const mz_uint8 *src;
    mz_ulong src_len;
    mz_ulong src_pos;
    mz_uint32 bitbuf;
    int bitcount;
} mz_bitreader;

typedef struct
{
    mz_uint16 count[MZ_MAXBITS + 1];
    mz_uint16 symbol[320];
} mz_huffman;

static const int mz_length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10,
    11, 13, 15, 17,
    19, 23, 27, 31,
    35, 43, 51, 59,
    67, 83, 99, 115,
    131, 163, 195, 227, 258
};

static const int mz_length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1,
    2, 2, 2, 2,
    3, 3, 3, 3,
    4, 4, 4, 4,
    5, 5, 5, 5, 0
};

static const int mz_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25,
    33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};

static const int mz_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3,
    4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static const mz_uint8 mz_cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10,
    5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int mz_need_bits(mz_bitreader *br, int need)
{
    while (br->bitcount < need)
    {
        if (br->src_pos >= br->src_len)
            return 0;
        br->bitbuf |= ((mz_uint32)br->src[br->src_pos++]) << br->bitcount;
        br->bitcount += 8;
    }
    return 1;
}

static int mz_read_bits(mz_bitreader *br, int need, mz_uint32 *out)
{
    if (need == 0)
    {
        *out = 0;
        return 1;
    }
    if (!mz_need_bits(br, need))
        return 0;
    *out = br->bitbuf & ((1u << need) - 1u);
    br->bitbuf >>= need;
    br->bitcount -= need;
    return 1;
}

static void mz_align_byte(mz_bitreader *br)
{
    int drop = br->bitcount & 7;
    br->bitbuf >>= drop;
    br->bitcount -= drop;
}

static int mz_build_huffman(mz_huffman *h, const mz_uint8 *lengths, int n)
{
    int i;
    mz_uint16 offs[MZ_MAXBITS + 1];

    for (i = 0; i <= MZ_MAXBITS; ++i)
        h->count[i] = 0;

    for (i = 0; i < n; ++i)
    {
        if (lengths[i] > MZ_MAXBITS)
            return 0;
        h->count[lengths[i]]++;
    }

    offs[0] = 0;
    for (i = 1; i <= MZ_MAXBITS; ++i)
        offs[i] = (mz_uint16)(offs[i - 1] + h->count[i - 1]);

    for (i = 0; i < n; ++i)
    {
        mz_uint8 len = lengths[i];
        if (len != 0)
            h->symbol[offs[len]++] = (mz_uint16)i;
    }

    return 1;
}

static int mz_decode_symbol(mz_bitreader *br, const mz_huffman *h, int *sym)
{
    int len;
    int first = 0;
    int index = 0;
    int code = 0;

    for (len = 1; len <= MZ_MAXBITS; ++len)
    {
        mz_uint32 bit;
        int count;

        if (!mz_read_bits(br, 1, &bit))
            return 0;

        code |= (int)bit;
        count = h->count[len];

        if (code < first + count)
        {
            *sym = h->symbol[index + (code - first)];
            return 1;
        }

        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }

    return 0;
}

static int mz_emit_byte(mz_uint8 *dst, mz_ulong *dst_pos, mz_ulong dst_cap, mz_uint8 b)
{
    if (*dst_pos >= dst_cap)
        return 0;
    dst[(*dst_pos)++] = b;
    return 1;
}

static int mz_copy_match(mz_uint8 *dst, mz_ulong *dst_pos, mz_ulong dst_cap, int distance, int length)
{
    mz_ulong pos = *dst_pos;
    int i;

    if (distance <= 0 || (mz_ulong)distance > pos)
        return 0;
    if (pos + (mz_ulong)length > dst_cap)
        return 0;

    for (i = 0; i < length; ++i)
    {
        dst[pos] = dst[pos - (mz_ulong)distance];
        pos++;
    }

    *dst_pos = pos;
    return 1;
}

static int mz_build_fixed_tables(mz_huffman *litlen, mz_huffman *dist)
{
    mz_uint8 ll[288];
    mz_uint8 dd[32];
    int i;

    for (i = 0; i <= 143; ++i) ll[i] = 8;
    for (; i <= 255; ++i) ll[i] = 9;
    for (; i <= 279; ++i) ll[i] = 7;
    for (; i <= 287; ++i) ll[i] = 8;

    for (i = 0; i < 32; ++i) dd[i] = 5;

    if (!mz_build_huffman(litlen, ll, 288))
        return 0;
    if (!mz_build_huffman(dist, dd, 32))
        return 0;

    return 1;
}

static int mz_build_dynamic_tables(mz_bitreader *br, mz_huffman *litlen, mz_huffman *dist)
{
    mz_uint32 hlit_bits, hdist_bits, hclen_bits;
    int hlit, hdist, hclen;
    mz_uint8 cl_lengths[19];
    mz_huffman cl;
    mz_uint8 lengths[320];
    int total, i;

    if (!mz_read_bits(br, 5, &hlit_bits)) return 0;
    if (!mz_read_bits(br, 5, &hdist_bits)) return 0;
    if (!mz_read_bits(br, 4, &hclen_bits)) return 0;

    hlit = (int)hlit_bits + 257;
    hdist = (int)hdist_bits + 1;
    hclen = (int)hclen_bits + 4;
    total = hlit + hdist;

    if (hlit > 286 || hdist > 30 || total > 320)
        return 0;

    for (i = 0; i < 19; ++i)
        cl_lengths[i] = 0;
    for (i = 0; i < hclen; ++i)
    {
        mz_uint32 v;
        if (!mz_read_bits(br, 3, &v))
            return 0;
        cl_lengths[mz_cl_order[i]] = (mz_uint8)v;
    }

    if (!mz_build_huffman(&cl, cl_lengths, 19))
        return 0;

    for (i = 0; i < total;)
    {
        int sym;
        if (!mz_decode_symbol(br, &cl, &sym))
            return 0;

        if (sym <= 15)
        {
            lengths[i++] = (mz_uint8)sym;
        }
        else if (sym == 16)
        {
            mz_uint32 extra;
            int repeat, prev;
            if (i == 0)
                return 0;
            if (!mz_read_bits(br, 2, &extra))
                return 0;
            repeat = (int)extra + 3;
            prev = lengths[i - 1];
            while (repeat-- > 0)
            {
                if (i >= total)
                    return 0;
                lengths[i++] = (mz_uint8)prev;
            }
        }
        else if (sym == 17)
        {
            mz_uint32 extra;
            int repeat;
            if (!mz_read_bits(br, 3, &extra))
                return 0;
            repeat = (int)extra + 3;
            while (repeat-- > 0)
            {
                if (i >= total)
                    return 0;
                lengths[i++] = 0;
            }
        }
        else if (sym == 18)
        {
            mz_uint32 extra;
            int repeat;
            if (!mz_read_bits(br, 7, &extra))
                return 0;
            repeat = (int)extra + 11;
            while (repeat-- > 0)
            {
                if (i >= total)
                    return 0;
                lengths[i++] = 0;
            }
        }
        else
        {
            return 0;
        }
    }

    if (!mz_build_huffman(litlen, lengths, hlit))
        return 0;
    if (!mz_build_huffman(dist, lengths + hlit, hdist))
        return 0;

    return 1;
}

int mz_uncompress(unsigned char *pDest, mz_ulong *pDest_len, const unsigned char *pSource, mz_ulong source_len)
{
    mz_bitreader br;
    mz_ulong dst_pos = 0;
    mz_ulong dst_cap;
    int final_block = 0;
    mz_uint8 *dst;

    if (!pDest || !pDest_len || !pSource)
        return MZ_PARAM_ERROR;

    dst = (mz_uint8 *)pDest;
    dst_cap = *pDest_len;

    br.src = (const mz_uint8 *)pSource;
    br.src_len = source_len;
    br.src_pos = 0;
    br.bitbuf = 0;
    br.bitcount = 0;

    if (source_len >= 2)
    {
        mz_uint8 cmf = pSource[0];
        mz_uint8 flg = pSource[1];
        if ((cmf & 0x0F) == 8 && (((int)cmf << 8) + flg) % 31 == 0)
        {
            if (flg & 0x20)
                return MZ_DATA_ERROR;
            br.src_pos = 2;
        }
    }

    while (!final_block)
    {
        mz_uint32 bfinal, btype;
        if (!mz_read_bits(&br, 1, &bfinal))
            return MZ_DATA_ERROR;
        if (!mz_read_bits(&br, 2, &btype))
            return MZ_DATA_ERROR;
        final_block = (int)bfinal;

        if (btype == 0)
        {
            mz_uint32 len, nlen;
            mz_ulong i;

            mz_align_byte(&br);
            if (!mz_read_bits(&br, 16, &len))
                return MZ_DATA_ERROR;
            if (!mz_read_bits(&br, 16, &nlen))
                return MZ_DATA_ERROR;
            if (((len ^ 0xFFFFu) & 0xFFFFu) != (nlen & 0xFFFFu))
                return MZ_DATA_ERROR;

            for (i = 0; i < (mz_ulong)len; ++i)
            {
                mz_uint32 v;
                if (!mz_read_bits(&br, 8, &v))
                    return MZ_DATA_ERROR;
                if (!mz_emit_byte(dst, &dst_pos, dst_cap, (mz_uint8)v))
                    return MZ_BUF_ERROR;
            }
        }
        else if (btype == 1 || btype == 2)
        {
            mz_huffman litlen, dist;

            if (btype == 1)
            {
                if (!mz_build_fixed_tables(&litlen, &dist))
                    return MZ_DATA_ERROR;
            }
            else
            {
                if (!mz_build_dynamic_tables(&br, &litlen, &dist))
                    return MZ_DATA_ERROR;
            }

            for (;;)
            {
                int sym;
                if (!mz_decode_symbol(&br, &litlen, &sym))
                    return MZ_DATA_ERROR;

                if (sym < 256)
                {
                    if (!mz_emit_byte(dst, &dst_pos, dst_cap, (mz_uint8)sym))
                        return MZ_BUF_ERROR;
                }
                else if (sym == 256)
                {
                    break;
                }
                else
                {
                    int lcode = sym - 257;
                    int dcode;
                    int len, distv;
                    mz_uint32 extra;

                    if (lcode < 0 || lcode >= 29)
                        return MZ_DATA_ERROR;

                    len = mz_length_base[lcode];
                    if (mz_length_extra[lcode] > 0)
                    {
                        if (!mz_read_bits(&br, mz_length_extra[lcode], &extra))
                            return MZ_DATA_ERROR;
                        len += (int)extra;
                    }

                    if (!mz_decode_symbol(&br, &dist, &dcode))
                        return MZ_DATA_ERROR;
                    if (dcode < 0 || dcode >= 30)
                        return MZ_DATA_ERROR;

                    distv = mz_dist_base[dcode];
                    if (mz_dist_extra[dcode] > 0)
                    {
                        if (!mz_read_bits(&br, mz_dist_extra[dcode], &extra))
                            return MZ_DATA_ERROR;
                        distv += (int)extra;
                    }

                    if (!mz_copy_match(dst, &dst_pos, dst_cap, distv, len))
                        return MZ_BUF_ERROR;
                }
            }
        }
        else
        {
            return MZ_DATA_ERROR;
        }
    }

    *pDest_len = dst_pos;
    return MZ_OK;
}
