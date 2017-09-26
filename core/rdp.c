#include "rdp.h"
#include "vi.h"
#include "common.h"
#include "plugin.h"
#include "rdram.h"
#include "trace_write.h"
#include "msg.h"
#include "irand.h"
#include "file.h"
#include "parallel_c.hpp"

#include <memory.h>
#include <string.h>

// tctables.h
static const int32_t norm_point_table[64] = {
    0x4000, 0x3f04, 0x3e10, 0x3d22, 0x3c3c, 0x3b5d, 0x3a83, 0x39b1,
    0x38e4, 0x381c, 0x375a, 0x369d, 0x35e5, 0x3532, 0x3483, 0x33d9,
    0x3333, 0x3291, 0x31f4, 0x3159, 0x30c3, 0x3030, 0x2fa1, 0x2f15,
    0x2e8c, 0x2e06, 0x2d83, 0x2d03, 0x2c86, 0x2c0b, 0x2b93, 0x2b1e,
    0x2aab, 0x2a3a, 0x29cc, 0x2960, 0x28f6, 0x288e, 0x2828, 0x27c4,
    0x2762, 0x2702, 0x26a4, 0x2648, 0x25ed, 0x2594, 0x253d, 0x24e7,
    0x2492, 0x243f, 0x23ee, 0x239e, 0x234f, 0x2302, 0x22b6, 0x226c,
    0x2222, 0x21da, 0x2193, 0x214d, 0x2108, 0x20c5, 0x2082, 0x2041
};

static const int32_t norm_slope_table[64] = {
    0xf03, 0xf0b, 0xf11, 0xf19, 0xf20, 0xf25, 0xf2d, 0xf32,
    0xf37, 0xf3d, 0xf42, 0xf47, 0xf4c, 0xf50, 0xf55, 0xf59,
    0xf5d, 0xf62, 0xf64, 0xf69, 0xf6c, 0xf70, 0xf73, 0xf76,
    0xf79, 0xf7c, 0xf7f, 0xf82, 0xf84, 0xf87, 0xf8a, 0xf8c,
    0xf8e, 0xf91, 0xf93, 0xf95, 0xf97, 0xf99, 0xf9b, 0xf9d,
    0xf9f, 0xfa1, 0xfa3, 0xfa4, 0xfa6, 0xfa8, 0xfa9, 0xfaa,
    0xfac, 0xfae, 0xfaf, 0xfb0, 0xfb2, 0xfb3, 0xfb5, 0xfb5,
    0xfb7, 0xfb8, 0xfb9, 0xfba, 0xfbc, 0xfbc, 0xfbe, 0xfbe
};

#define SIGN16(x)   ((int16_t)(x))
#define SIGN8(x)    ((int8_t)(x))


#define SIGN(x, numb)   (((x) & ((1 << numb) - 1)) | -((x) & (1 << (numb - 1))))
#define SIGNF(x, numb)  ((x) | -((x) & (1 << (numb - 1))))







#define GET_LOW_RGBA16_TMEM(x)  (replicated_rgba[((x) >> 1) & 0x1f])
#define GET_MED_RGBA16_TMEM(x)  (replicated_rgba[((x) >> 6) & 0x1f])
#define GET_HI_RGBA16_TMEM(x)   (replicated_rgba[(x) >> 11])

// bit constants for DP_STATUS
#define DP_STATUS_XBUS_DMA      0x001   // DMEM DMA mode is set
#define DP_STATUS_FREEZE        0x002   // Freeze has been set
#define DP_STATUS_FLUSH         0x004   // Flush has been set
#define DP_STATUS_START_GCLK    0x008   // Unknown
#define DP_STATUS_TMEM_BUSY     0x010   // TMEM is in use on the RDP
#define DP_STATUS_PIPE_BUSY     0x020   // Graphics pipe is in use on the RDP
#define DP_STATUS_CMD_BUSY      0x040   // RDP is currently executing a command
#define DP_STATUS_CBUF_BUSY     0x080   // RDRAM RDP command buffer is in use
#define DP_STATUS_DMA_BUSY      0x100   // DMEM RDP command buffer is in use
#define DP_STATUS_END_VALID     0x200   // Unknown
#define DP_STATUS_START_VALID   0x400   // Unknown

static struct core_config* config;
static struct plugin_api* plugin;

static TLS int blshifta = 0, blshiftb = 0, pastblshifta = 0, pastblshiftb = 0;
static TLS int32_t pastrawdzmem = 0;

struct span
{
    int lx, rx;
    int unscrx;
    int validline;
    int32_t r, g, b, a, s, t, w, z;
    int32_t majorx[4];
    int32_t minorx[4];
    int32_t invalyscan[4];
};

static TLS struct span span[1024];

// span states
static TLS struct
{
    int ds;
    int dt;
    int dw;
    int dr;
    int dg;
    int db;
    int da;
    int dz;
    int dzpix;

    int drdy;
    int dgdy;
    int dbdy;
    int dady;
    int dzdy;
    int cdr;
    int cdg;
    int cdb;
    int cda;
    int cdz;

    int dsdy;
    int dtdy;
    int dwdy;
} spans;



struct color
{
    int32_t r, g, b, a;
};

struct fbcolor
{
    uint8_t r, g, b;
};

struct rectangle
{
    uint16_t xl, yl, xh, yh;
};

struct tex_rectangle
{
    int tilenum;
    uint16_t xl, yl, xh, yh;
    int16_t s, t;
    int16_t dsdx, dtdy;
    uint32_t flip;
};

struct tile
{
    int format;
    int size;
    int line;
    int tmem;
    int palette;
    int ct, mt, cs, ms;
    int mask_t, shift_t, mask_s, shift_s;

    uint16_t sl, tl, sh, th;

    struct
    {
        int clampdiffs, clampdifft;
        int clampens, clampent;
        int masksclamped, masktclamped;
        int notlutswitch, tlutswitch;
    } f;
};

struct modederivs
{
    int stalederivs;
    int dolod;
    int partialreject_1cycle;
    int partialreject_2cycle;
    int special_bsel0;
    int special_bsel1;
    int rgb_alpha_dither;
    int realblendershiftersneeded;
    int interpixelblendershiftersneeded;
};

struct other_modes
{
    int cycle_type;
    int persp_tex_en;
    int detail_tex_en;
    int sharpen_tex_en;
    int tex_lod_en;
    int en_tlut;
    int tlut_type;
    int sample_type;
    int mid_texel;
    int bi_lerp0;
    int bi_lerp1;
    int convert_one;
    int key_en;
    int rgb_dither_sel;
    int alpha_dither_sel;
    int blend_m1a_0;
    int blend_m1a_1;
    int blend_m1b_0;
    int blend_m1b_1;
    int blend_m2a_0;
    int blend_m2a_1;
    int blend_m2b_0;
    int blend_m2b_1;
    int force_blend;
    int alpha_cvg_select;
    int cvg_times_alpha;
    int z_mode;
    int cvg_dest;
    int color_on_cvg;
    int image_read_en;
    int z_update_en;
    int z_compare_en;
    int antialias_en;
    int z_source_sel;
    int dither_alpha_en;
    int alpha_compare_en;
    struct modederivs f;
};



#define PIXEL_SIZE_4BIT         0
#define PIXEL_SIZE_8BIT         1
#define PIXEL_SIZE_16BIT        2
#define PIXEL_SIZE_32BIT        3

#define CYCLE_TYPE_1            0
#define CYCLE_TYPE_2            1
#define CYCLE_TYPE_COPY         2
#define CYCLE_TYPE_FILL         3


#define FORMAT_RGBA             0
#define FORMAT_YUV              1
#define FORMAT_CI               2
#define FORMAT_IA               3
#define FORMAT_I                4


#define TEXEL_RGBA4             0
#define TEXEL_RGBA8             1
#define TEXEL_RGBA16            2
#define TEXEL_RGBA32            3
#define TEXEL_YUV4              4
#define TEXEL_YUV8              5
#define TEXEL_YUV16             6
#define TEXEL_YUV32             7
#define TEXEL_CI4               8
#define TEXEL_CI8               9
#define TEXEL_CI16              0xa
#define TEXEL_CI32              0xb
#define TEXEL_IA4               0xc
#define TEXEL_IA8               0xd
#define TEXEL_IA16              0xe
#define TEXEL_IA32              0xf
#define TEXEL_I4                0x10
#define TEXEL_I8                0x11
#define TEXEL_I16               0x12
#define TEXEL_I32               0x13



static TLS struct other_modes other_modes;

static TLS struct color combined_color;
static TLS struct color texel0_color;
static TLS struct color texel1_color;
static TLS struct color nexttexel_color;
static TLS struct color shade_color;
static TLS int32_t noise = 0;
static TLS int32_t primitive_lod_frac = 0;
static int32_t one_color = 0x100;
static int32_t zero_color = 0x00;
static int32_t blenderone   = 0xff;

static TLS struct color pixel_color;
static TLS struct color inv_pixel_color;
static TLS struct color blended_pixel_color;
static TLS struct color memory_color;
static TLS struct color pre_memory_color;

static TLS uint32_t fill_color;

static TLS uint32_t primitive_z;
static TLS uint16_t primitive_delta_z;

static TLS int ti_format = FORMAT_RGBA;
static TLS int ti_size = PIXEL_SIZE_4BIT;
static TLS int ti_width = 0;
static TLS uint32_t ti_address = 0;

static TLS struct tile tile[8];

static TLS uint8_t tmem[0x1000];

#define tlut ((uint16_t*)(&tmem[0x800]))

#define PIXELS_TO_BYTES(pix, siz) (((pix) << (siz)) >> 1)

struct spansigs {
    int startspan;
    int endspan;
    int preendspan;
    int nextspan;
    int midspan;
    int longspan;
    int onelessthanmid;
};

static INLINE void fetch_texel(struct color *color, int s, int t, uint32_t tilenum);
static INLINE void fetch_texel_quadro(struct color *color0, struct color *color1, struct color *color2, struct color *color3, int s0, int sdiff, int t0, int tdiff, uint32_t tilenum, int unequaluppers);
static INLINE void fetch_texel_entlut_quadro(struct color *color0, struct color *color1, struct color *color2, struct color *color3, int s0, int sdiff, int t0, int tdiff, uint32_t tilenum, int isupper, int isupperrg);
static INLINE void fetch_texel_entlut_quadro_nearest(struct color *color0, struct color *color1, struct color *color2, struct color *color3, int s0, int t0, uint32_t tilenum, int isupper, int isupperrg);
static void tile_tlut_common_cs_decoder(const uint32_t* args);
static void loading_pipeline(int start, int end, int tilenum, int coord_quad, int ltlut);
static void get_tmem_idx(int s, int t, uint32_t tilenum, uint32_t* idx0, uint32_t* idx1, uint32_t* idx2, uint32_t* idx3, uint32_t* bit3flipped, uint32_t* hibit);
static void sort_tmem_idx(uint32_t *idx, uint32_t idxa, uint32_t idxb, uint32_t idxc, uint32_t idxd, uint32_t bankno);
static void sort_tmem_shorts_lowhalf(uint32_t* bindshort, uint32_t short0, uint32_t short1, uint32_t short2, uint32_t short3, uint32_t bankno);
static void compute_color_index(uint32_t* cidx, uint32_t readshort, uint32_t nybbleoffset, uint32_t tilenum);
static void read_tmem_copy(int s, int s1, int s2, int s3, int t, uint32_t tilenum, uint32_t* sortshort, int* hibits, int* lowbits);
static STRICTINLINE void texture_pipeline_cycle(struct color* TEX, struct color* prev, int32_t SSS, int32_t SST, uint32_t tilenum, uint32_t cycle);
static STRICTINLINE void tc_pipeline_copy(int32_t* sss0, int32_t* sss1, int32_t* sss2, int32_t* sss3, int32_t* sst, int tilenum);
static STRICTINLINE void tc_pipeline_load(int32_t* sss, int32_t* sst, int tilenum, int coord_quad);
static STRICTINLINE void tcclamp_generic(int32_t* S, int32_t* T, int32_t* SFRAC, int32_t* TFRAC, int32_t maxs, int32_t maxt, int32_t num);
static STRICTINLINE void tcclamp_cycle(int32_t* S, int32_t* T, int32_t* SFRAC, int32_t* TFRAC, int32_t maxs, int32_t maxt, int32_t num);
static STRICTINLINE void tcclamp_cycle_light(int32_t* S, int32_t* T, int32_t maxs, int32_t maxt, int32_t num);
static STRICTINLINE void tcshift_cycle(int32_t* S, int32_t* T, int32_t* maxs, int32_t* maxt, uint32_t num);
static STRICTINLINE void tcshift_copy(int32_t* S, int32_t* T, uint32_t num);
static INLINE void precalculate_everything(void);
static STRICTINLINE int alpha_compare(int32_t comb_alpha);
static STRICTINLINE int finalize_spanalpha(uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg);
static STRICTINLINE int32_t normalize_dzpix(int32_t sum);
static STRICTINLINE int32_t clamp(int32_t value,int32_t min,int32_t max);
static void tcdiv_persp(int32_t ss, int32_t st, int32_t sw, int32_t* sss, int32_t* sst);
static void tcdiv_nopersp(int32_t ss, int32_t st, int32_t sw, int32_t* sss, int32_t* sst);
static STRICTINLINE void tclod_4x17_to_15(int32_t scurr, int32_t snext, int32_t tcurr, int32_t tnext, int32_t previous, int32_t* lod);
static STRICTINLINE void tclod_tcclamp(int32_t* sss, int32_t* sst);
static STRICTINLINE void lodfrac_lodtile_signals(int lodclamp, int32_t lod, uint32_t* l_tile, uint32_t* magnify, uint32_t* distant, int32_t* lfdst);
static STRICTINLINE void tclod_1cycle_current(int32_t* sss, int32_t* sst, int32_t nexts, int32_t nextt, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs);
static STRICTINLINE void tclod_1cycle_current_simple(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs);
static STRICTINLINE void tclod_1cycle_next(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs, int32_t* prelodfrac);
static STRICTINLINE void tclod_2cycle_current(int32_t* sss, int32_t* sst, int32_t nexts, int32_t nextt, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2);
static STRICTINLINE void tclod_2cycle_current_simple(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2);
static STRICTINLINE void tclod_2cycle_current_notexel1(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1);
static STRICTINLINE void tclod_2cycle_next(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2, int32_t* prelodfrac);
static STRICTINLINE void tclod_copy(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1);
static STRICTINLINE void get_texel1_1cycle(int32_t* s1, int32_t* t1, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, struct spansigs* sigs);
static STRICTINLINE void get_nexttexel0_2cycle(int32_t* s1, int32_t* t1, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc);
static INLINE void calculate_clamp_diffs(uint32_t tile);
static INLINE void calculate_tile_derivs(uint32_t tile);
static void rgb_dither_complete(int* r, int* g, int* b, int dith);
static void rgb_dither_nothing(int* r, int* g, int* b, int dith);
static void get_dither_noise_complete(int x, int y, int* cdith, int* adith);
static void get_dither_only(int x, int y, int* cdith, int* adith);
static void get_dither_nothing(int x, int y, int* cdith, int* adith);
static void deduce_derivatives(void);

static TLS int32_t k0_tf = 0, k1_tf = 0, k2_tf = 0, k3_tf = 0;
static TLS int32_t k4 = 0, k5 = 0;
static TLS int32_t lod_frac = 0;

static void (*get_dither_noise_func[3])(int, int, int*, int*) =
{
    get_dither_noise_complete, get_dither_only, get_dither_nothing
};

static void (*rgb_dither_func[2])(int*, int*, int*, int) =
{
    rgb_dither_complete, rgb_dither_nothing
};

static void (*tcdiv_func[2])(int32_t, int32_t, int32_t, int32_t*, int32_t*) =
{
    tcdiv_nopersp, tcdiv_persp
};

static TLS void (*get_dither_noise_ptr)(int, int, int*, int*);
static TLS void (*rgb_dither_ptr)(int*, int*, int*, int);
static TLS void (*tcdiv_ptr)(int32_t, int32_t, int32_t, int32_t*, int32_t*);

static uint8_t replicated_rgba[32];
static int32_t maskbits_table[16];
static int32_t ge_two_table[128];
static int32_t log2table[256];
static int32_t tcdiv_table[0x8000];
static int32_t clamp_t_diff[8];
static int32_t clamp_s_diff[8];

static struct
{
    int copymstrangecrashes, fillmcrashes, fillmbitcrashes, syncfullcrash;
} onetimewarnings;

static TLS uint32_t max_level = 0;
static TLS int32_t min_level = 0;
static int rdp_pipeline_crashed = 0;

#include "rdp/cmd.c"
#include "rdp/blender.c"
#include "rdp/combiner.c"
#include "rdp/coverage.c"
#include "rdp/zbuffer.c"
#include "rdp/fbuffer.c"
#include "rdp/rasterizer.c"

static STRICTINLINE void tcmask(int32_t* S, int32_t* T, int32_t num)
{
    int32_t wrap;



    if (tile[num].mask_s)
    {
        if (tile[num].ms)
        {
            wrap = *S >> tile[num].f.masksclamped;
            wrap &= 1;
            *S ^= (-wrap);
        }
        *S &= maskbits_table[tile[num].mask_s];
    }

    if (tile[num].mask_t)
    {
        if (tile[num].mt)
        {
            wrap = *T >> tile[num].f.masktclamped;
            wrap &= 1;
            *T ^= (-wrap);
        }

        *T &= maskbits_table[tile[num].mask_t];
    }
}


static STRICTINLINE void tcmask_coupled(int32_t* S, int32_t* sdiff, int32_t* T, int32_t* tdiff, int32_t num)
{
    int32_t wrap;
    int32_t maskbits;
    int32_t wrapthreshold;


    if (tile[num].mask_s)
    {
        maskbits = maskbits_table[tile[num].mask_s];

        if (tile[num].ms)
        {
            wrapthreshold = tile[num].f.masksclamped;

            wrap = (*S >> wrapthreshold) & 1;
            *S ^= (-wrap);
            *S &= maskbits;


            if (((*S - wrap) & maskbits) == maskbits)
                *sdiff = 0;
            else
                *sdiff = 1 - (wrap << 1);
        }
        else
        {
            *S &= maskbits;
            if (*S == maskbits)
                *sdiff = -(*S);
            else
                *sdiff = 1;
        }
    }
    else
        *sdiff = 1;

    if (tile[num].mask_t)
    {
        maskbits = maskbits_table[tile[num].mask_t];

        if (tile[num].mt)
        {
            wrapthreshold = tile[num].f.masktclamped;

            wrap = (*T >> wrapthreshold) & 1;
            *T ^= (-wrap);
            *T &= maskbits;

            if (((*T - wrap) & maskbits) == maskbits)
                *tdiff = 0;
            else
                *tdiff = 1 - (wrap << 1);
        }
        else
        {
            *T &= maskbits;
            if (*T == maskbits)
                *tdiff = -(*T & 0xff);
            else
                *tdiff = 1;
        }
    }
    else
        *tdiff = 1;
}


static STRICTINLINE void tcmask_copy(int32_t* S, int32_t* S1, int32_t* S2, int32_t* S3, int32_t* T, int32_t num)
{
    int32_t wrap;
    int32_t maskbits_s;
    int32_t swrapthreshold;

    if (tile[num].mask_s)
    {
        if (tile[num].ms)
        {
            swrapthreshold = tile[num].f.masksclamped;

            wrap = (*S >> swrapthreshold) & 1;
            *S ^= (-wrap);

            wrap = (*S1 >> swrapthreshold) & 1;
            *S1 ^= (-wrap);

            wrap = (*S2 >> swrapthreshold) & 1;
            *S2 ^= (-wrap);

            wrap = (*S3 >> swrapthreshold) & 1;
            *S3 ^= (-wrap);
        }

        maskbits_s = maskbits_table[tile[num].mask_s];
        *S &= maskbits_s;
        *S1 &= maskbits_s;
        *S2 &= maskbits_s;
        *S3 &= maskbits_s;
    }

    if (tile[num].mask_t)
    {
        if (tile[num].mt)
        {
            wrap = *T >> tile[num].f.masktclamped;
            wrap &= 1;
            *T ^= (-wrap);
        }

        *T &= maskbits_table[tile[num].mask_t];
    }
}


static STRICTINLINE void tcshift_cycle(int32_t* S, int32_t* T, int32_t* maxs, int32_t* maxt, uint32_t num)
{



    int32_t coord = *S;
    int32_t shifter = tile[num].shift_s;


    if (shifter < 11)
    {
        coord = SIGN16(coord);
        coord >>= shifter;
    }
    else
    {
        coord <<= (16 - shifter);
        coord = SIGN16(coord);
    }
    *S = coord;




    *maxs = ((coord >> 3) >= tile[num].sh);



    coord = *T;
    shifter = tile[num].shift_t;

    if (shifter < 11)
    {
        coord = SIGN16(coord);
        coord >>= shifter;
    }
    else
    {
        coord <<= (16 - shifter);
        coord = SIGN16(coord);
    }
    *T = coord;
    *maxt = ((coord >> 3) >= tile[num].th);
}


static STRICTINLINE void tcshift_copy(int32_t* S, int32_t* T, uint32_t num)
{
    int32_t coord = *S;
    int32_t shifter = tile[num].shift_s;

    if (shifter < 11)
    {
        coord = SIGN16(coord);
        coord >>= shifter;
    }
    else
    {
        coord <<= (16 - shifter);
        coord = SIGN16(coord);
    }
    *S = coord;

    coord = *T;
    shifter = tile[num].shift_t;

    if (shifter < 11)
    {
        coord = SIGN16(coord);
        coord >>= shifter;
    }
    else
    {
        coord <<= (16 - shifter);
        coord = SIGN16(coord);
    }
    *T = coord;

}



static STRICTINLINE void tcclamp_cycle(int32_t* S, int32_t* T, int32_t* SFRAC, int32_t* TFRAC, int32_t maxs, int32_t maxt, int32_t num)
{



    int32_t locs = *S, loct = *T;
    if (tile[num].f.clampens)
    {

        if (maxs)
        {
            *S = tile[num].f.clampdiffs;
            *SFRAC = 0;
        }
        else if (!(locs & 0x10000))
            *S = locs >> 5;
        else
        {
            *S = 0;
            *SFRAC = 0;
        }
    }
    else
        *S = (locs >> 5);

    if (tile[num].f.clampent)
    {
        if (maxt)
        {
            *T = tile[num].f.clampdifft;
            *TFRAC = 0;
        }
        else if (!(loct & 0x10000))
            *T = loct >> 5;
        else
        {
            *T = 0;
            *TFRAC = 0;
        }
    }
    else
        *T = (loct >> 5);
}


static STRICTINLINE void tcclamp_cycle_light(int32_t* S, int32_t* T, int32_t maxs, int32_t maxt, int32_t num)
{
    int32_t locs = *S, loct = *T;
    if (tile[num].f.clampens)
    {
        if (maxs)
            *S = tile[num].f.clampdiffs;
        else if (!(locs & 0x10000))
            *S = locs >> 5;
        else
            *S = 0;
    }
    else
        *S = (locs >> 5);

    if (tile[num].f.clampent)
    {
        if (maxt)
            *T = tile[num].f.clampdifft;
        else if (!(loct & 0x10000))
            *T = loct >> 5;
        else
            *T = 0;
    }
    else
        *T = (loct >> 5);
}


int rdp_init(struct core_config* _config)
{
    config = _config;

    get_dither_noise_ptr = get_dither_noise_func[0];
    rgb_dither_ptr = rgb_dither_func[0];
    tcdiv_ptr = tcdiv_func[0];

    uint32_t tmp[2] = {0};
    rdp_set_other_modes(tmp);
    other_modes.f.stalederivs = 1;

    memset(tmem, 0, 0x1000);

    memset(tile, 0, sizeof(tile));

    for (int i = 0; i < 8; i++)
    {
        calculate_tile_derivs(i);
        calculate_clamp_diffs(i);
    }

    memset(&combined_color, 0, sizeof(struct color));
    memset(&prim_color, 0, sizeof(struct color));
    memset(&env_color, 0, sizeof(struct color));
    memset(&key_scale, 0, sizeof(struct color));
    memset(&key_center, 0, sizeof(struct color));

    rdp_pipeline_crashed = 0;
    memset(&onetimewarnings, 0, sizeof(onetimewarnings));

    precalculate_everything();

    return 0;
}


static INLINE void precalculate_everything(void)
{
    int i = 0, k = 0, j = 0;

    precalc_cvmask_derivatives();



    i = 0;
    log2table[0] = log2table[1] = 0;
    for (i = 2; i < 256; i++)
    {
        for (k = 7; k > 0; k--)
        {
            if((i >> k) & 1)
            {
                log2table[i] = k;
                break;
            }
        }
    }










    for (i = 0; i < 32; i++)
        replicated_rgba[i] = (i << 3) | ((i >> 2) & 7);



    maskbits_table[0] = 0x3ff;
    for (i = 1; i < 16; i++)
        maskbits_table[i] = ((uint16_t)(0xffff) >> (16 - i)) & 0x3ff;



    combiner_init();







    int temppoint, tempslope;
    int normout;
    int wnorm;
    int shift, tlu_rcp;

    for (i = 0; i < 0x8000; i++)
    {
        for (k = 1; k <= 14 && !((i << k) & 0x8000); k++)
            ;
        shift = k - 1;
        normout = (i << shift) & 0x3fff;
        wnorm = (normout & 0xff) << 2;
        normout >>= 8;



        temppoint = norm_point_table[normout];
        tempslope = norm_slope_table[normout];

        tempslope = (tempslope | ~0x3ff) + 1;

        tlu_rcp = (((tempslope * wnorm) >> 10) + temppoint) & 0x7fff;

        tcdiv_table[i] = shift | (tlu_rcp << 4);
    }


    z_init();
    fb_init();
    blender_init();
    rasterizer_init();
}


static const uint8_t bayer_matrix[16] =
{
     0,  4,  1, 5,
     4,  0,  5, 1,
     3,  7,  2, 6,
     7,  3,  6, 2
};


static const uint8_t magic_matrix[16] =
{
     0,  6,  1, 7,
     4,  2,  5, 3,
     3,  5,  2, 4,
     7,  1,  6, 0
};


static INLINE void fetch_texel(struct color *color, int s, int t, uint32_t tilenum)
{
    uint32_t tbase = tile[tilenum].line * (t & 0xff) + tile[tilenum].tmem;



    uint32_t tpal   = tile[tilenum].palette;








    uint16_t *tc16 = (uint16_t*)tmem;
    uint32_t taddr = 0;





    switch (tile[tilenum].f.notlutswitch)
    {
    case TEXEL_RGBA4:
        {
            taddr = ((tbase << 4) + s) >> 1;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);
            uint8_t byteval, c;

            byteval = tmem[taddr & 0xfff];
            c = ((s & 1)) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color->r = c;
            color->g = c;
            color->b = c;
            color->a = c;
        }
        break;
    case TEXEL_RGBA8:
        {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p;

            p = tmem[taddr & 0xfff];
            color->r = p;
            color->g = p;
            color->b = p;
            color->a = p;
        }
        break;
    case TEXEL_RGBA16:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);


            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = GET_HI_RGBA16_TMEM(c);
            color->g = GET_MED_RGBA16_TMEM(c);
            color->b = GET_LOW_RGBA16_TMEM(c);
            color->a = (c & 1) ? 0xff : 0;
        }
        break;
    case TEXEL_RGBA32:
        {



            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;


            taddr &= 0x3ff;
            c = tc16[taddr];
            color->r = c >> 8;
            color->g = c & 0xff;
            c = tc16[taddr | 0x400];
            color->b = c >> 8;
            color->a = c & 0xff;
        }
        break;
    case TEXEL_YUV4:
        {
            taddr = (tbase << 3) + s;

            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            int32_t u, save;

            save = tmem[taddr & 0x7ff];

            save &= 0xf0;
            save |= (save >> 4);

            u = save - 0x80;

            color->r = u;
            color->g = u;
            color->b = save;
            color->a = save;
        }
        break;
    case TEXEL_YUV8:
        {
            taddr = (tbase << 3) + s;

            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            int32_t u, save;

            save = u = tmem[taddr & 0x7ff];

            u = u - 0x80;

            color->r = u;
            color->g = u;
            color->b = save;
            color->a = save;
        }
        break;
    case TEXEL_YUV16:
    case TEXEL_YUV32:
        {
            taddr = (tbase << 3) + s;
            int taddrlow = taddr >> 1;

            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);
            taddrlow ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            taddr &= 0x7ff;
            taddrlow &= 0x3ff;

            uint16_t c = tc16[taddrlow];

            int32_t y, u, v;
            y = tmem[taddr | 0x800];
            u = c >> 8;
            v = c & 0xff;

            u = u - 0x80;
            v = v - 0x80;



            color->r = u;
            color->g = v;
            color->b = y;
            color->a = y;
        }
        break;
    case TEXEL_CI4:
        {
            taddr = ((tbase << 4) + s) >> 1;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p;



            p = tmem[taddr & 0xfff];
            p = (s & 1) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color->r = color->g = color->b = color->a = p;
        }
        break;
    case TEXEL_CI8:
        {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p;


            p = tmem[taddr & 0xfff];
            color->r = p;
            color->g = p;
            color->b = p;
            color->a = p;
        }
        break;
    case TEXEL_CI16:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = c >> 8;
            color->g = c & 0xff;
            color->b = color->r;
            color->a = (c & 1) ? 0xff : 0;
        }
        break;
    case TEXEL_CI32:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = c >> 8;
            color->g = c & 0xff;
            color->b = color->r;
            color->a = (c & 1) ? 0xff : 0;

        }
        break;
    case TEXEL_IA4:
        {
            taddr = ((tbase << 4) + s) >> 1;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p, i;


            p = tmem[taddr & 0xfff];
            p = (s & 1) ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color->r = i;
            color->g = i;
            color->b = i;
            color->a = (p & 0x1) ? 0xff : 0;
        }
        break;
    case TEXEL_IA8:
        {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p, i;


            p = tmem[taddr & 0xfff];
            i = p & 0xf0;
            i |= (i >> 4);
            color->r = i;
            color->g = i;
            color->b = i;
            color->a = ((p & 0xf) << 4) | (p & 0xf);
        }
        break;
    case TEXEL_IA16:
        {


            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = color->g = color->b = (c >> 8);
            color->a = c & 0xff;
        }
        break;
    case TEXEL_IA32:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = c >> 8;
            color->g = c & 0xff;
            color->b = color->r;
            color->a = (c & 1) ? 0xff : 0;
        }
        break;
    case TEXEL_I4:
        {
            taddr = ((tbase << 4) + s) >> 1;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t byteval, c;

            byteval = tmem[taddr & 0xfff];
            c = (s & 1) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color->r = c;
            color->g = c;
            color->b = c;
            color->a = c;
        }
        break;
    case TEXEL_I8:
        {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t c;

            c = tmem[taddr & 0xfff];
            color->r = c;
            color->g = c;
            color->b = c;
            color->a = c;
        }
        break;
    case TEXEL_I16:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = c >> 8;
            color->g = c & 0xff;
            color->b = color->r;
            color->a = (c & 1) ? 0xff : 0;
        }
        break;
    case TEXEL_I32:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = c >> 8;
            color->g = c & 0xff;
            color->b = color->r;
            color->a = (c & 1) ? 0xff : 0;
        }
        break;
    }
}


static INLINE void fetch_texel_quadro(struct color *color0, struct color *color1, struct color *color2, struct color *color3, int s0, int sdiff, int t0, int tdiff, uint32_t tilenum, int unequaluppers)
{

    uint32_t tbase0 = tile[tilenum].line * (t0 & 0xff) + tile[tilenum].tmem;

    int t1 = (t0 & 0xff) + tdiff;



    int s1 = s0 + sdiff;

    uint32_t tbase2 = tile[tilenum].line * t1 + tile[tilenum].tmem;
    uint32_t tpal = tile[tilenum].palette;
    uint32_t xort, ands;




    uint16_t *tc16 = (uint16_t*)tmem;
    uint32_t taddr0, taddr1, taddr2, taddr3;
    uint32_t taddrlow0, taddrlow1, taddrlow2, taddrlow3;

    switch (tile[tilenum].f.notlutswitch)
    {
    case TEXEL_RGBA4:
        {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t byteval, c;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            ands = s0 & 1;
            byteval = tmem[taddr0];
            c = (ands) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color0->r = c;
            color0->g = c;
            color0->b = c;
            color0->a = c;
            byteval = tmem[taddr2];
            c = (ands) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color2->r = c;
            color2->g = c;
            color2->b = c;
            color2->a = c;

            ands = s1 & 1;
            byteval = tmem[taddr1];
            c = (ands) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color1->r = c;
            color1->g = c;
            color1->b = c;
            color1->a = c;
            byteval = tmem[taddr3];
            c = (ands) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color3->r = c;
            color3->g = c;
            color3->b = c;
            color3->a = c;
        }
        break;
    case TEXEL_RGBA8:
        {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            p = tmem[taddr0];
            color0->r = p;
            color0->g = p;
            color0->b = p;
            color0->a = p;
            p = tmem[taddr2];
            color2->r = p;
            color2->g = p;
            color2->b = p;
            color2->a = p;
            p = tmem[taddr1];
            color1->r = p;
            color1->g = p;
            color1->b = p;
            color1->a = p;
            p = tmem[taddr3];
            color3->r = p;
            color3->g = p;
            color3->b = p;
            color3->a = p;
        }
        break;
    case TEXEL_RGBA16:
        {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            c1 = tc16[taddr1];
            c2 = tc16[taddr2];
            c3 = tc16[taddr3];
            color0->r = GET_HI_RGBA16_TMEM(c0);
            color0->g = GET_MED_RGBA16_TMEM(c0);
            color0->b = GET_LOW_RGBA16_TMEM(c0);
            color0->a = (c0 & 1) ? 0xff : 0;
            color1->r = GET_HI_RGBA16_TMEM(c1);
            color1->g = GET_MED_RGBA16_TMEM(c1);
            color1->b = GET_LOW_RGBA16_TMEM(c1);
            color1->a = (c1 & 1) ? 0xff : 0;
            color2->r = GET_HI_RGBA16_TMEM(c2);
            color2->g = GET_MED_RGBA16_TMEM(c2);
            color2->b = GET_LOW_RGBA16_TMEM(c2);
            color2->a = (c2 & 1) ? 0xff : 0;
            color3->r = GET_HI_RGBA16_TMEM(c3);
            color3->g = GET_MED_RGBA16_TMEM(c3);
            color3->b = GET_LOW_RGBA16_TMEM(c3);
            color3->a = (c3 & 1) ? 0xff : 0;
        }
        break;
    case TEXEL_RGBA32:
        {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x3ff;
            taddr1 &= 0x3ff;
            taddr2 &= 0x3ff;
            taddr3 &= 0x3ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            c0 = tc16[taddr0 | 0x400];
            color0->b = c0 >>  8;
            color0->a = c0 & 0xff;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            c1 = tc16[taddr1 | 0x400];
            color1->b = c1 >>  8;
            color1->a = c1 & 0xff;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            c2 = tc16[taddr2 | 0x400];
            color2->b = c2 >>  8;
            color2->a = c2 & 0xff;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            c3 = tc16[taddr3 | 0x400];
            color3->b = c3 >>  8;
            color3->a = c3 & 0xff;
        }
        break;
    case TEXEL_YUV4:
        {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1 + sdiff;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1 + sdiff;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;

            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            int32_t u0, u1, u2, u3, save0, save1, save2, save3;

            save0 = tmem[taddr0 & 0x7ff];
            save0 &= 0xf0;
            save0 |= (save0 >> 4);
            u0 = save0 - 0x80;

            save1 = tmem[taddr1 & 0x7ff];
            save1 &= 0xf0;
            save1 |= (save1 >> 4);
            u1 = save1 - 0x80;

            save2 = tmem[taddr2 & 0x7ff];
            save2 &= 0xf0;
            save2 |= (save2 >> 4);
            u2 = save2 - 0x80;

            save3 = tmem[taddr3 & 0x7ff];
            save3 &= 0xf0;
            save3 |= (save3 >> 4);
            u3 = save3 - 0x80;

            color0->r = u0;
            color0->g = u0;
            color1->r = u1;
            color1->g = u1;
            color2->r = u2;
            color2->g = u2;
            color3->r = u3;
            color3->g = u3;

            if (unequaluppers)
            {
                color0->b = color0->a = save3;
                color1->b = color1->a = save2;
                color2->b = color2->a = save1;
                color3->b = color3->a = save0;
            }
            else
            {
                color0->b = color0->a = save0;
                color1->b = color1->a = save1;
                color2->b = color2->a = save2;
                color3->b = color3->a = save3;
            }
        }
        break;
    case TEXEL_YUV8:
        {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1 + sdiff;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1 + sdiff;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            int32_t u0, u1, u2, u3, save0, save1, save2, save3;

            save0 = u0 = tmem[taddr0 & 0x7ff];
            u0 = u0 - 0x80;
            save1 = u1 = tmem[taddr1 & 0x7ff];
            u1 = u1 - 0x80;
            save2 = u2 = tmem[taddr2 & 0x7ff];
            u2 = u2 - 0x80;
            save3 = u3 = tmem[taddr3 & 0x7ff];
            u3 = u3 - 0x80;

            color0->r = u0;
            color0->g = u0;
            color1->r = u1;
            color1->g = u1;
            color2->r = u2;
            color2->g = u2;
            color3->r = u3;
            color3->g = u3;

            if (unequaluppers)
            {
                color0->b = color0->a = save3;
                color1->b = color1->a = save2;
                color2->b = color2->a = save1;
                color3->b = color3->a = save0;
            }
            else
            {
                color0->b = color0->a = save0;
                color1->b = color1->a = save1;
                color2->b = color2->a = save2;
                color3->b = color3->a = save3;
            }
        }
        break;
    case TEXEL_YUV16:
    case TEXEL_YUV32:
        {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;


            taddrlow0 = (taddr0) >> 1;
            taddrlow1 = (taddr1 + sdiff) >> 1;
            taddrlow2 = (taddr2) >> 1;
            taddrlow3 = (taddr3 + sdiff) >> 1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddrlow0 ^= xort;
            taddrlow1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddrlow2 ^= xort;
            taddrlow3 ^= xort;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            taddrlow0 &= 0x3ff;
            taddrlow1 &= 0x3ff;
            taddrlow2 &= 0x3ff;
            taddrlow3 &= 0x3ff;

            uint16_t c0, c1, c2, c3;
            int32_t y0, y1, y2, y3, u0, u1, u2, u3, v0, v1, v2, v3;

            c0 = tc16[taddrlow0];
            c1 = tc16[taddrlow1];
            c2 = tc16[taddrlow2];
            c3 = tc16[taddrlow3];

            y0 = tmem[taddr0 | 0x800];
            u0 = c0 >> 8;
            v0 = c0 & 0xff;
            y1 = tmem[taddr1 | 0x800];
            u1 = c1 >> 8;
            v1 = c1 & 0xff;
            y2 = tmem[taddr2 | 0x800];
            u2 = c2 >> 8;
            v2 = c2 & 0xff;
            y3 = tmem[taddr3 | 0x800];
            u3 = c3 >> 8;
            v3 = c3 & 0xff;

            u0 = u0 - 0x80;
            v0 = v0 - 0x80;
            u1 = u1 - 0x80;
            v1 = v1 - 0x80;
            u2 = u2 - 0x80;
            v2 = v2 - 0x80;
            u3 = u3 - 0x80;
            v3 = v3 - 0x80;

            color0->r = u0;
            color0->g = v0;
            color1->r = u1;
            color1->g = v1;
            color2->r = u2;
            color2->g = v2;
            color3->r = u3;
            color3->g = v3;

            color0->b = color0->a = y0;
            color1->b = color1->a = y1;
            color2->b = color2->a = y2;
            color3->b = color3->a = y3;
        }
        break;
    case TEXEL_CI4:
        {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            ands = s0 & 1;
            p = tmem[taddr0];
            p = (ands) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color0->r = color0->g = color0->b = color0->a = p;
            p = tmem[taddr2];
            p = (ands) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color2->r = color2->g = color2->b = color2->a = p;

            ands = s1 & 1;
            p = tmem[taddr1];
            p = (ands) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color1->r = color1->g = color1->b = color1->a = p;
            p = tmem[taddr3];
            p = (ands) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color3->r = color3->g = color3->b = color3->a = p;
        }
        break;
    case TEXEL_CI8:
        {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            p = tmem[taddr0];
            color0->r = p;
            color0->g = p;
            color0->b = p;
            color0->a = p;
            p = tmem[taddr2];
            color2->r = p;
            color2->g = p;
            color2->b = p;
            color2->a = p;
            p = tmem[taddr1];
            color1->r = p;
            color1->g = p;
            color1->b = p;
            color1->a = p;
            p = tmem[taddr3];
            color3->r = p;
            color3->g = p;
            color3->b = p;
            color3->a = p;
        }
        break;
    case TEXEL_CI16:
        {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            color0->b = c0 >> 8;
            color0->a = (c0 & 1) ? 0xff : 0;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            color1->b = c1 >> 8;
            color1->a = (c1 & 1) ? 0xff : 0;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            color2->b = c2 >> 8;
            color2->a = (c2 & 1) ? 0xff : 0;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            color3->b = c3 >> 8;
            color3->a = (c3 & 1) ? 0xff : 0;

        }
        break;
    case TEXEL_CI32:
        {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            color0->b = c0 >> 8;
            color0->a = (c0 & 1) ? 0xff : 0;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            color1->b = c1 >> 8;
            color1->a = (c1 & 1) ? 0xff : 0;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            color2->b = c2 >> 8;
            color2->a = (c2 & 1) ? 0xff : 0;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            color3->b = c3 >> 8;
            color3->a = (c3 & 1) ? 0xff : 0;

        }
        break;
    case TEXEL_IA4:
        {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p, i;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            ands = s0 & 1;
            p = tmem[taddr0];
            p = ands ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color0->r = i;
            color0->g = i;
            color0->b = i;
            color0->a = (p & 0x1) ? 0xff : 0;
            p = tmem[taddr2];
            p = ands ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color2->r = i;
            color2->g = i;
            color2->b = i;
            color2->a = (p & 0x1) ? 0xff : 0;

            ands = s1 & 1;
            p = tmem[taddr1];
            p = ands ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color1->r = i;
            color1->g = i;
            color1->b = i;
            color1->a = (p & 0x1) ? 0xff : 0;
            p = tmem[taddr3];
            p = ands ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color3->r = i;
            color3->g = i;
            color3->b = i;
            color3->a = (p & 0x1) ? 0xff : 0;

        }
        break;
    case TEXEL_IA8:
        {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p, i;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            p = tmem[taddr0];
            i = p & 0xf0;
            i |= (i >> 4);
            color0->r = i;
            color0->g = i;
            color0->b = i;
            color0->a = ((p & 0xf) << 4) | (p & 0xf);
            p = tmem[taddr1];
            i = p & 0xf0;
            i |= (i >> 4);
            color1->r = i;
            color1->g = i;
            color1->b = i;
            color1->a = ((p & 0xf) << 4) | (p & 0xf);
            p = tmem[taddr2];
            i = p & 0xf0;
            i |= (i >> 4);
            color2->r = i;
            color2->g = i;
            color2->b = i;
            color2->a = ((p & 0xf) << 4) | (p & 0xf);
            p = tmem[taddr3];
            i = p & 0xf0;
            i |= (i >> 4);
            color3->r = i;
            color3->g = i;
            color3->b = i;
            color3->a = ((p & 0xf) << 4) | (p & 0xf);


        }
        break;
    case TEXEL_IA16:
        {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = color0->g = color0->b = c0 >> 8;
            color0->a = c0 & 0xff;
            c1 = tc16[taddr1];
            color1->r = color1->g = color1->b = c1 >> 8;
            color1->a = c1 & 0xff;
            c2 = tc16[taddr2];
            color2->r = color2->g = color2->b = c2 >> 8;
            color2->a = c2 & 0xff;
            c3 = tc16[taddr3];
            color3->r = color3->g = color3->b = c3 >> 8;
            color3->a = c3 & 0xff;

        }
        break;
    case TEXEL_IA32:
        {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            color0->b = c0 >> 8;
            color0->a = (c0 & 1) ? 0xff : 0;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            color1->b = c1 >> 8;
            color1->a = (c1 & 1) ? 0xff : 0;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            color2->b = c2 >> 8;
            color2->a = (c2 & 1) ? 0xff : 0;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            color3->b = c3 >> 8;
            color3->a = (c3 & 1) ? 0xff : 0;

        }
        break;
    case TEXEL_I4:
        {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p, c0, c1, c2, c3;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            ands = s0 & 1;
            p = tmem[taddr0];
            c0 = ands ? (p & 0xf) : (p >> 4);
            c0 |= (c0 << 4);
            color0->r = color0->g = color0->b = color0->a = c0;
            p = tmem[taddr2];
            c2 = ands ? (p & 0xf) : (p >> 4);
            c2 |= (c2 << 4);
            color2->r = color2->g = color2->b = color2->a = c2;

            ands = s1 & 1;
            p = tmem[taddr1];
            c1 = ands ? (p & 0xf) : (p >> 4);
            c1 |= (c1 << 4);
            color1->r = color1->g = color1->b = color1->a = c1;
            p = tmem[taddr3];
            c3 = ands ? (p & 0xf) : (p >> 4);
            c3 |= (c3 << 4);
            color3->r = color3->g = color3->b = color3->a = c3;

        }
        break;
    case TEXEL_I8:
        {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;

            p = tmem[taddr0];
            color0->r = p;
            color0->g = p;
            color0->b = p;
            color0->a = p;
            p = tmem[taddr1];
            color1->r = p;
            color1->g = p;
            color1->b = p;
            color1->a = p;
            p = tmem[taddr2];
            color2->r = p;
            color2->g = p;
            color2->b = p;
            color2->a = p;
            p = tmem[taddr3];
            color3->r = p;
            color3->g = p;
            color3->b = p;
            color3->a = p;
        }
        break;
    case TEXEL_I16:
        {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            color0->b = c0 >> 8;
            color0->a = (c0 & 1) ? 0xff : 0;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            color1->b = c1 >> 8;
            color1->a = (c1 & 1) ? 0xff : 0;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            color2->b = c2 >> 8;
            color2->a = (c2 & 1) ? 0xff : 0;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            color3->b = c3 >> 8;
            color3->a = (c3 & 1) ? 0xff : 0;
        }
        break;
    case TEXEL_I32:
    default:
        {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            color0->b = c0 >> 8;
            color0->a = (c0 & 1) ? 0xff : 0;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            color1->b = c1 >> 8;
            color1->a = (c1 & 1) ? 0xff : 0;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            color2->b = c2 >> 8;
            color2->a = (c2 & 1) ? 0xff : 0;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            color3->b = c3 >> 8;
            color3->a = (c3 & 1) ? 0xff : 0;
        }
        break;
    }
}

static INLINE void fetch_texel_entlut_quadro(struct color *color0, struct color *color1, struct color *color2, struct color *color3, int s0, int sdiff, int t0, int tdiff, uint32_t tilenum, int isupper, int isupperrg)
{
    uint32_t tbase0 = tile[tilenum].line * (t0 & 0xff) + tile[tilenum].tmem;
    int t1 = (t0 & 0xff) + tdiff;
    int s1;

    uint32_t tbase2 = tile[tilenum].line * t1 + tile[tilenum].tmem;
    uint32_t tpal = tile[tilenum].palette << 4;
    uint32_t xort, ands;

    uint16_t *tc16 = (uint16_t*)tmem;
    uint32_t taddr0, taddr1, taddr2, taddr3;
    uint16_t c0, c1, c2, c3;




    uint32_t xorupperrg = isupperrg ? (WORD_ADDR_XOR ^ 3) : WORD_ADDR_XOR;



    switch(tile[tilenum].f.tlutswitch)
    {
    case 0:
    case 1:
    case 2:
        {
            s1 = s0 + sdiff;
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            ands = s0 & 1;
            c0 = tmem[taddr0 & 0x7ff];
            c0 = (ands) ? (c0 & 0xf) : (c0 >> 4);
            taddr0 = (tpal | c0) << 2;
            c2 = tmem[taddr2 & 0x7ff];
            c2 = (ands) ? (c2 & 0xf) : (c2 >> 4);
            taddr2 = ((tpal | c2) << 2) + 2;

            ands = s1 & 1;
            c1 = tmem[taddr1 & 0x7ff];
            c1 = (ands) ? (c1 & 0xf) : (c1 >> 4);
            taddr1 = ((tpal | c1) << 2) + 1;
            c3 = tmem[taddr3 & 0x7ff];
            c3 = (ands) ? (c3 & 0xf) : (c3 >> 4);
            taddr3 = ((tpal | c3) << 2) + 3;
        }
        break;
    case 3:
        {
            s1 = s0 + (sdiff << 1);
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tmem[taddr0 & 0x7ff];
            c0 >>= 4;
            taddr0 = (tpal | c0) << 2;
            c2 = tmem[taddr2 & 0x7ff];
            c2 >>= 4;
            taddr2 = ((tpal | c2) << 2) + 2;

            c1 = tmem[taddr1 & 0x7ff];
            c1 >>= 4;
            taddr1 = ((tpal | c1) << 2) + 1;
            c3 = tmem[taddr3 & 0x7ff];
            c3 >>= 4;
            taddr3 = ((tpal | c3) << 2) + 3;
        }
        break;
    case 4:
    case 5:
    case 6:
        {
            s1 = s0 + sdiff;
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tmem[taddr0 & 0x7ff];
            taddr0 = c0 << 2;
            c2 = tmem[taddr2 & 0x7ff];
            taddr2 = (c2 << 2) + 2;
            c1 = tmem[taddr1 & 0x7ff];
            taddr1 = (c1 << 2) + 1;
            c3 = tmem[taddr3 & 0x7ff];
            taddr3 = (c3 << 2) + 3;
        }
        break;
    case 7:
        {
            s1 = s0 + (sdiff << 1);
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tmem[taddr0 & 0x7ff];
            taddr0 = c0 << 2;
            c2 = tmem[taddr2 & 0x7ff];
            taddr2 = (c2 << 2) + 2;
            c1 = tmem[taddr1 & 0x7ff];
            taddr1 = (c1 << 2) + 1;
            c3 = tmem[taddr3 & 0x7ff];
            taddr3 = (c3 << 2) + 3;
        }
        break;
    case 8:
    case 9:
    case 10:
        {
            s1 = s0 + sdiff;
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tc16[taddr0 & 0x3ff];
            taddr0 = (c0 >> 6) & ~3;
            c1 = tc16[taddr1 & 0x3ff];
            taddr1 = ((c1 >> 6) & ~3) + 1;
            c2 = tc16[taddr2 & 0x3ff];
            taddr2 = ((c2 >> 6) & ~3) + 2;
            c3 = tc16[taddr3 & 0x3ff];
            taddr3 = (c3 >> 6) | 3;
        }
        break;
    case 11:
        {
            s1 = s0 + (sdiff << 1);
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tmem[taddr0 & 0x7ff];
            taddr0 = c0 << 2;
            c2 = tmem[taddr2 & 0x7ff];
            taddr2 = (c2 << 2) + 2;
            c1 = tmem[taddr1 & 0x7ff];
            taddr1 = (c1 << 2) + 1;
            c3 = tmem[taddr3 & 0x7ff];
            taddr3 = (c3 << 2) + 3;
        }
        break;
    case 12:
    case 13:
    case 14:
        {
            s1 = s0 + sdiff;
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;

            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tc16[taddr0 & 0x3ff];
            taddr0 = (c0 >> 6) & ~3;
            c1 = tc16[taddr1 & 0x3ff];
            taddr1 = ((c1 >> 6) & ~3) + 1;
            c2 = tc16[taddr2 & 0x3ff];
            taddr2 = ((c2 >> 6) & ~3) + 2;
            c3 = tc16[taddr3 & 0x3ff];
            taddr3 = (c3 >> 6) | 3;
        }
        break;
    case 15:
    default:
        {
            s1 = s0 + (sdiff << 1);
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tmem[taddr0 & 0x7ff];
            taddr0 = c0 << 2;
            c2 = tmem[taddr2 & 0x7ff];
            taddr2 = (c2 << 2) + 2;
            c1 = tmem[taddr1 & 0x7ff];
            taddr1 = (c1 << 2) + 1;
            c3 = tmem[taddr3 & 0x7ff];
            taddr3 = (c3 << 2) + 3;
        }
        break;
    }

    c0 = tlut[taddr0 ^ xorupperrg];
    c2 = tlut[taddr2 ^ xorupperrg];
    c1 = tlut[taddr1 ^ xorupperrg];
    c3 = tlut[taddr3 ^ xorupperrg];

    if (!other_modes.tlut_type)
    {
        color0->r = GET_HI_RGBA16_TMEM(c0);
        color0->g = GET_MED_RGBA16_TMEM(c0);
        color1->r = GET_HI_RGBA16_TMEM(c1);
        color1->g = GET_MED_RGBA16_TMEM(c1);
        color2->r = GET_HI_RGBA16_TMEM(c2);
        color2->g = GET_MED_RGBA16_TMEM(c2);
        color3->r = GET_HI_RGBA16_TMEM(c3);
        color3->g = GET_MED_RGBA16_TMEM(c3);

        if (isupper == isupperrg)
        {
            color0->b = GET_LOW_RGBA16_TMEM(c0);
            color0->a = (c0 & 1) ? 0xff : 0;
            color1->b = GET_LOW_RGBA16_TMEM(c1);
            color1->a = (c1 & 1) ? 0xff : 0;
            color2->b = GET_LOW_RGBA16_TMEM(c2);
            color2->a = (c2 & 1) ? 0xff : 0;
            color3->b = GET_LOW_RGBA16_TMEM(c3);
            color3->a = (c3 & 1) ? 0xff : 0;
        }
        else
        {
            color0->b = GET_LOW_RGBA16_TMEM(c3);
            color0->a = (c3 & 1) ? 0xff : 0;
            color1->b = GET_LOW_RGBA16_TMEM(c2);
            color1->a = (c2 & 1) ? 0xff : 0;
            color2->b = GET_LOW_RGBA16_TMEM(c1);
            color2->a = (c1 & 1) ? 0xff : 0;
            color3->b = GET_LOW_RGBA16_TMEM(c0);
            color3->a = (c0 & 1) ? 0xff : 0;
        }
    }
    else
    {
        color0->r = color0->g = c0 >> 8;
        color1->r = color1->g = c1 >> 8;
        color2->r = color2->g = c2 >> 8;
        color3->r = color3->g = c3 >> 8;

        if (isupper == isupperrg)
        {
            color0->b = c0 >> 8;
            color0->a = c0 & 0xff;
            color1->b = c1 >> 8;
            color1->a = c1 & 0xff;
            color2->b = c2 >> 8;
            color2->a = c2 & 0xff;
            color3->b = c3 >> 8;
            color3->a = c3 & 0xff;
        }
        else
        {
            color0->b = c3 >> 8;
            color0->a = c3 & 0xff;
            color1->b = c2 >> 8;
            color1->a = c2 & 0xff;
            color2->b = c1 >> 8;
            color2->a = c1 & 0xff;
            color3->b = c0 >> 8;
            color3->a = c0 & 0xff;
        }
    }
}

static INLINE void fetch_texel_entlut_quadro_nearest(struct color *color0, struct color *color1, struct color *color2, struct color *color3, int s0, int t0, uint32_t tilenum, int isupper, int isupperrg)
{
    uint32_t tbase0 = tile[tilenum].line * t0 + tile[tilenum].tmem;
    uint32_t tpal = tile[tilenum].palette << 4;
    uint32_t xort, ands;

    uint16_t *tc16 = (uint16_t*)tmem;
    uint32_t taddr0 = 0;
    uint16_t c0, c1, c2, c3;

    uint32_t xorupperrg = isupperrg ? (WORD_ADDR_XOR ^ 3) : WORD_ADDR_XOR;

    switch(tile[tilenum].f.tlutswitch)
    {
    case 0:
    case 1:
    case 2:
        {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;

            ands = s0 & 1;
            c0 = tmem[taddr0 & 0x7ff];
            c0 = (ands) ? (c0 & 0xf) : (c0 >> 4);

            taddr0 = (tpal | c0) << 2;
        }
        break;
    case 3:
        {
            taddr0 = (tbase0 << 3) + s0;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;

            c0 = tmem[taddr0 & 0x7ff];

            c0 >>= 4;

            taddr0 = (tpal | c0) << 2;
        }
        break;
    case 4:
    case 5:
    case 6:
        {
            taddr0 = (tbase0 << 3) + s0;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;

            c0 = tmem[taddr0 & 0x7ff];

            taddr0 = c0 << 2;
        }
        break;
    case 7:
        {
            taddr0 = (tbase0 << 3) + s0;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;

            c0 = tmem[taddr0 & 0x7ff];

            taddr0 = c0 << 2;
        }
        break;
    case 8:
    case 9:
    case 10:
        {
            taddr0 = (tbase0 << 2) + s0;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;

            c0 = tc16[taddr0 & 0x3ff];

            taddr0 = (c0 >> 6) & ~3;
        }
        break;
    case 11:
        {
            taddr0 = (tbase0 << 3) + s0;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;

            c0 = tmem[taddr0 & 0x7ff];

            taddr0 = c0 << 2;
        }
        break;
    case 12:
    case 13:
    case 14:
        {
            taddr0 = (tbase0 << 2) + s0;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;

            c0 = tc16[taddr0 & 0x3ff];

            taddr0 = (c0 >> 6) & ~3;
        }
        break;
    case 15:
    default:
        {
            taddr0 = (tbase0 << 3) + s0;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;

            c0 = tmem[taddr0 & 0x7ff];

            taddr0 = c0 << 2;
        }
        break;
    }

    c0 = tlut[taddr0 ^ xorupperrg];
    c1 = tlut[(taddr0 + 1) ^ xorupperrg];
    c2 = tlut[(taddr0 + 2) ^ xorupperrg];
    c3 = tlut[(taddr0 + 3) ^ xorupperrg];

    if (!other_modes.tlut_type)
    {
        color0->r = GET_HI_RGBA16_TMEM(c0);
        color0->g = GET_MED_RGBA16_TMEM(c0);
        color1->r = GET_HI_RGBA16_TMEM(c1);
        color1->g = GET_MED_RGBA16_TMEM(c1);
        color2->r = GET_HI_RGBA16_TMEM(c2);
        color2->g = GET_MED_RGBA16_TMEM(c2);
        color3->r = GET_HI_RGBA16_TMEM(c3);
        color3->g = GET_MED_RGBA16_TMEM(c3);

        if (isupper == isupperrg)
        {
            color0->b = GET_LOW_RGBA16_TMEM(c0);
            color0->a = (c0 & 1) ? 0xff : 0;
            color1->b = GET_LOW_RGBA16_TMEM(c1);
            color1->a = (c1 & 1) ? 0xff : 0;
            color2->b = GET_LOW_RGBA16_TMEM(c2);
            color2->a = (c2 & 1) ? 0xff : 0;
            color3->b = GET_LOW_RGBA16_TMEM(c3);
            color3->a = (c3 & 1) ? 0xff : 0;
        }
        else
        {
            color0->b = GET_LOW_RGBA16_TMEM(c3);
            color0->a = (c3 & 1) ? 0xff : 0;
            color1->b = GET_LOW_RGBA16_TMEM(c2);
            color1->a = (c2 & 1) ? 0xff : 0;
            color2->b = GET_LOW_RGBA16_TMEM(c1);
            color2->a = (c1 & 1) ? 0xff : 0;
            color3->b = GET_LOW_RGBA16_TMEM(c0);
            color3->a = (c0 & 1) ? 0xff : 0;
        }
    }
    else
    {
        color0->r = color0->g = c0 >> 8;
        color1->r = color1->g = c1 >> 8;
        color2->r = color2->g = c2 >> 8;
        color3->r = color3->g = c3 >> 8;

        if (isupper == isupperrg)
        {
            color0->b = c0 >> 8;
            color0->a = c0 & 0xff;
            color1->b = c1 >> 8;
            color1->a = c1 & 0xff;
            color2->b = c2 >> 8;
            color2->a = c2 & 0xff;
            color3->b = c3 >> 8;
            color3->a = c3 & 0xff;
        }
        else
        {
            color0->b = c3 >> 8;
            color0->a = c3 & 0xff;
            color1->b = c2 >> 8;
            color1->a = c2 & 0xff;
            color2->b = c1 >> 8;
            color2->a = c1 & 0xff;
            color3->b = c0 >> 8;
            color3->a = c0 & 0xff;
        }
    }
}


static void get_tmem_idx(int s, int t, uint32_t tilenum, uint32_t* idx0, uint32_t* idx1, uint32_t* idx2, uint32_t* idx3, uint32_t* bit3flipped, uint32_t* hibit)
{
    uint32_t tbase = (tile[tilenum].line * t) & 0x1ff;
    tbase += tile[tilenum].tmem;
    uint32_t tsize = tile[tilenum].size;
    uint32_t tformat = tile[tilenum].format;
    uint32_t sshorts = 0;


    if (tsize == PIXEL_SIZE_8BIT || tformat == FORMAT_YUV)
        sshorts = s >> 1;
    else if (tsize >= PIXEL_SIZE_16BIT)
        sshorts = s;
    else
        sshorts = s >> 2;
    sshorts &= 0x7ff;

    *bit3flipped = ((sshorts & 2) ? 1 : 0) ^ (t & 1);

    int tidx_a = ((tbase << 2) + sshorts) & 0x7fd;
    int tidx_b = (tidx_a + 1) & 0x7ff;
    int tidx_c = (tidx_a + 2) & 0x7ff;
    int tidx_d = (tidx_a + 3) & 0x7ff;

    *hibit = (tidx_a & 0x400) ? 1 : 0;

    if (t & 1)
    {
        tidx_a ^= 2;
        tidx_b ^= 2;
        tidx_c ^= 2;
        tidx_d ^= 2;
    }


    sort_tmem_idx(idx0, tidx_a, tidx_b, tidx_c, tidx_d, 0);
    sort_tmem_idx(idx1, tidx_a, tidx_b, tidx_c, tidx_d, 1);
    sort_tmem_idx(idx2, tidx_a, tidx_b, tidx_c, tidx_d, 2);
    sort_tmem_idx(idx3, tidx_a, tidx_b, tidx_c, tidx_d, 3);
}







static void read_tmem_copy(int s, int s1, int s2, int s3, int t, uint32_t tilenum, uint32_t* sortshort, int* hibits, int* lowbits)
{
    uint32_t tbase = (tile[tilenum].line * t) & 0x1ff;
    tbase += tile[tilenum].tmem;
    uint32_t tsize = tile[tilenum].size;
    uint32_t tformat = tile[tilenum].format;
    uint32_t shbytes = 0, shbytes1 = 0, shbytes2 = 0, shbytes3 = 0;
    int32_t delta = 0;
    uint32_t sortidx[8];


    if (tsize == PIXEL_SIZE_8BIT || tformat == FORMAT_YUV)
    {
        shbytes = s << 1;
        shbytes1 = s1 << 1;
        shbytes2 = s2 << 1;
        shbytes3 = s3 << 1;
    }
    else if (tsize >= PIXEL_SIZE_16BIT)
    {
        shbytes = s << 2;
        shbytes1 = s1 << 2;
        shbytes2 = s2 << 2;
        shbytes3 = s3 << 2;
    }
    else
    {
        shbytes = s;
        shbytes1 = s1;
        shbytes2 = s2;
        shbytes3 = s3;
    }

    shbytes &= 0x1fff;
    shbytes1 &= 0x1fff;
    shbytes2 &= 0x1fff;
    shbytes3 &= 0x1fff;

    int tidx_a, tidx_blow, tidx_bhi, tidx_c, tidx_dlow, tidx_dhi;

    tbase <<= 4;
    tidx_a = (tbase + shbytes) & 0x1fff;
    tidx_bhi = (tbase + shbytes1) & 0x1fff;
    tidx_c = (tbase + shbytes2) & 0x1fff;
    tidx_dhi = (tbase + shbytes3) & 0x1fff;

    if (tformat == FORMAT_YUV)
    {
        delta = shbytes1 - shbytes;
        tidx_blow = (tidx_a + (delta << 1)) & 0x1fff;
        tidx_dlow = (tidx_blow + shbytes3 - shbytes) & 0x1fff;
    }
    else
    {
        tidx_blow = tidx_bhi;
        tidx_dlow = tidx_dhi;
    }

    if (t & 1)
    {
        tidx_a ^= 8;
        tidx_blow ^= 8;
        tidx_bhi ^= 8;
        tidx_c ^= 8;
        tidx_dlow ^= 8;
        tidx_dhi ^= 8;
    }

    hibits[0] = (tidx_a & 0x1000) ? 1 : 0;
    hibits[1] = (tidx_blow & 0x1000) ? 1 : 0;
    hibits[2] = (tidx_bhi & 0x1000) ? 1 : 0;
    hibits[3] = (tidx_c & 0x1000) ? 1 : 0;
    hibits[4] = (tidx_dlow & 0x1000) ? 1 : 0;
    hibits[5] = (tidx_dhi & 0x1000) ? 1 : 0;
    lowbits[0] = tidx_a & 0xf;
    lowbits[1] = tidx_blow & 0xf;
    lowbits[2] = tidx_bhi & 0xf;
    lowbits[3] = tidx_c & 0xf;
    lowbits[4] = tidx_dlow & 0xf;
    lowbits[5] = tidx_dhi & 0xf;

    uint16_t* tmem16 = (uint16_t*)tmem;
    uint32_t short0, short1, short2, short3;


    tidx_a >>= 2;
    tidx_blow >>= 2;
    tidx_bhi >>= 2;
    tidx_c >>= 2;
    tidx_dlow >>= 2;
    tidx_dhi >>= 2;


    sort_tmem_idx(&sortidx[0], tidx_a, tidx_blow, tidx_c, tidx_dlow, 0);
    sort_tmem_idx(&sortidx[1], tidx_a, tidx_blow, tidx_c, tidx_dlow, 1);
    sort_tmem_idx(&sortidx[2], tidx_a, tidx_blow, tidx_c, tidx_dlow, 2);
    sort_tmem_idx(&sortidx[3], tidx_a, tidx_blow, tidx_c, tidx_dlow, 3);

    short0 = tmem16[sortidx[0] ^ WORD_ADDR_XOR];
    short1 = tmem16[sortidx[1] ^ WORD_ADDR_XOR];
    short2 = tmem16[sortidx[2] ^ WORD_ADDR_XOR];
    short3 = tmem16[sortidx[3] ^ WORD_ADDR_XOR];


    sort_tmem_shorts_lowhalf(&sortshort[0], short0, short1, short2, short3, lowbits[0] >> 2);
    sort_tmem_shorts_lowhalf(&sortshort[1], short0, short1, short2, short3, lowbits[1] >> 2);
    sort_tmem_shorts_lowhalf(&sortshort[2], short0, short1, short2, short3, lowbits[3] >> 2);
    sort_tmem_shorts_lowhalf(&sortshort[3], short0, short1, short2, short3, lowbits[4] >> 2);

    if (other_modes.en_tlut)
    {

        compute_color_index(&short0, sortshort[0], lowbits[0] & 3, tilenum);
        compute_color_index(&short1, sortshort[1], lowbits[1] & 3, tilenum);
        compute_color_index(&short2, sortshort[2], lowbits[3] & 3, tilenum);
        compute_color_index(&short3, sortshort[3], lowbits[4] & 3, tilenum);


        sortidx[4] = (short0 << 2);
        sortidx[5] = (short1 << 2) | 1;
        sortidx[6] = (short2 << 2) | 2;
        sortidx[7] = (short3 << 2) | 3;
    }
    else
    {
        sort_tmem_idx(&sortidx[4], tidx_a, tidx_bhi, tidx_c, tidx_dhi, 0);
        sort_tmem_idx(&sortidx[5], tidx_a, tidx_bhi, tidx_c, tidx_dhi, 1);
        sort_tmem_idx(&sortidx[6], tidx_a, tidx_bhi, tidx_c, tidx_dhi, 2);
        sort_tmem_idx(&sortidx[7], tidx_a, tidx_bhi, tidx_c, tidx_dhi, 3);
    }

    short0 = tmem16[(sortidx[4] | 0x400) ^ WORD_ADDR_XOR];
    short1 = tmem16[(sortidx[5] | 0x400) ^ WORD_ADDR_XOR];
    short2 = tmem16[(sortidx[6] | 0x400) ^ WORD_ADDR_XOR];
    short3 = tmem16[(sortidx[7] | 0x400) ^ WORD_ADDR_XOR];



    if (other_modes.en_tlut)
    {
        sort_tmem_shorts_lowhalf(&sortshort[4], short0, short1, short2, short3, 0);
        sort_tmem_shorts_lowhalf(&sortshort[5], short0, short1, short2, short3, 1);
        sort_tmem_shorts_lowhalf(&sortshort[6], short0, short1, short2, short3, 2);
        sort_tmem_shorts_lowhalf(&sortshort[7], short0, short1, short2, short3, 3);
    }
    else
    {
        sort_tmem_shorts_lowhalf(&sortshort[4], short0, short1, short2, short3, lowbits[0] >> 2);
        sort_tmem_shorts_lowhalf(&sortshort[5], short0, short1, short2, short3, lowbits[2] >> 2);
        sort_tmem_shorts_lowhalf(&sortshort[6], short0, short1, short2, short3, lowbits[3] >> 2);
        sort_tmem_shorts_lowhalf(&sortshort[7], short0, short1, short2, short3, lowbits[5] >> 2);
    }
}




static void sort_tmem_idx(uint32_t *idx, uint32_t idxa, uint32_t idxb, uint32_t idxc, uint32_t idxd, uint32_t bankno)
{
    if ((idxa & 3) == bankno)
        *idx = idxa & 0x3ff;
    else if ((idxb & 3) == bankno)
        *idx = idxb & 0x3ff;
    else if ((idxc & 3) == bankno)
        *idx = idxc & 0x3ff;
    else if ((idxd & 3) == bankno)
        *idx = idxd & 0x3ff;
    else
        *idx = 0;
}


static void sort_tmem_shorts_lowhalf(uint32_t* bindshort, uint32_t short0, uint32_t short1, uint32_t short2, uint32_t short3, uint32_t bankno)
{
    switch(bankno)
    {
    case 0:
        *bindshort = short0;
        break;
    case 1:
        *bindshort = short1;
        break;
    case 2:
        *bindshort = short2;
        break;
    case 3:
        *bindshort = short3;
        break;
    }
}



static void compute_color_index(uint32_t* cidx, uint32_t readshort, uint32_t nybbleoffset, uint32_t tilenum)
{
    uint32_t lownib, hinib;
    if (tile[tilenum].size == PIXEL_SIZE_4BIT)
    {
        lownib = (nybbleoffset ^ 3) << 2;
        hinib = tile[tilenum].palette;
    }
    else
    {
        lownib = ((nybbleoffset & 2) ^ 2) << 2;
        hinib = lownib ? ((readshort >> 12) & 0xf) : ((readshort >> 4) & 0xf);
    }
    lownib = (readshort >> lownib) & 0xf;
    *cidx = (hinib << 4) | lownib;
}

static STRICTINLINE void texture_pipeline_cycle(struct color* TEX, struct color* prev, int32_t SSS, int32_t SST, uint32_t tilenum, uint32_t cycle)
{
#define TRELATIVE(x, y)     ((x) - ((y) << 3))



    int32_t maxs, maxt, invt3r, invt3g, invt3b, invt3a;
    int32_t sfrac, tfrac, invsf, invtf, sfracrg, invsfrg;
    int upper, upperrg, center, centerrg;


    int bilerp = cycle ? other_modes.bi_lerp1 : other_modes.bi_lerp0;
    int convert = other_modes.convert_one && cycle;
    struct color t0, t1, t2, t3;
    int sss1, sst1, sdiff, tdiff;

    sss1 = SSS;
    sst1 = SST;

    tcshift_cycle(&sss1, &sst1, &maxs, &maxt, tilenum);

    sss1 = TRELATIVE(sss1, tile[tilenum].sl);
    sst1 = TRELATIVE(sst1, tile[tilenum].tl);

    if (other_modes.sample_type || other_modes.en_tlut)
    {
        sfrac = sss1 & 0x1f;
        tfrac = sst1 & 0x1f;




        tcclamp_cycle(&sss1, &sst1, &sfrac, &tfrac, maxs, maxt, tilenum);






        tcmask_coupled(&sss1, &sdiff, &sst1, &tdiff, tilenum);







        upper = (sfrac + tfrac) & 0x20;




        if (tile[tilenum].format == FORMAT_YUV)
        {
            sfracrg = (sfrac >> 1) | ((sss1 & 1) << 4);



            upperrg = (sfracrg + tfrac) & 0x20;
        }
        else
        {
            upperrg = upper;
            sfracrg = sfrac;
        }





        if (!other_modes.sample_type)
            fetch_texel_entlut_quadro_nearest(&t0, &t1, &t2, &t3, sss1, sst1, tilenum, upper, upperrg);
        else if (other_modes.en_tlut)
            fetch_texel_entlut_quadro(&t0, &t1, &t2, &t3, sss1, sdiff, sst1, tdiff, tilenum, upper, upperrg);
        else
            fetch_texel_quadro(&t0, &t1, &t2, &t3, sss1, sdiff, sst1, tdiff, tilenum, upper - upperrg);
















        if (bilerp)
        {
            if (!other_modes.mid_texel)
                center = centerrg = 0;
            else
            {

                center = (sfrac == 0x10 && tfrac == 0x10);
                centerrg = (sfracrg == 0x10 && tfrac == 0x10);
            }

            invtf = 0x20 - tfrac;

            if (!centerrg)
            {
                if (!convert)
                {


                    if (upperrg)
                    {

                        invsfrg = 0x20 - sfracrg;

                        TEX->r = t3.r + ((invsfrg * (t2.r - t3.r) + invtf * (t1.r - t3.r) + 0x10) >> 5);
                        TEX->g = t3.g + ((invsfrg * (t2.g - t3.g) + invtf * (t1.g - t3.g) + 0x10) >> 5);
                    }
                    else
                    {
                        TEX->r = t0.r + ((sfracrg * (t1.r - t0.r) + tfrac * (t2.r - t0.r) + 0x10) >> 5);
                        TEX->g = t0.g + ((sfracrg * (t1.g - t0.g) + tfrac * (t2.g - t0.g) + 0x10) >> 5);
                    }
                }
                else
                {
                    if (upperrg)
                    {
                        TEX->r = prev->b + ((prev->r * (t2.r - t3.r) + prev->g * (t1.r - t3.r) + 0x80) >> 8);
                        TEX->g = prev->b + ((prev->r * (t2.g - t3.g) + prev->g * (t1.g - t3.g) + 0x80) >> 8);
                    }
                    else
                    {
                        TEX->r = prev->b + ((prev->r * (t1.r - t0.r) + prev->g * (t2.r - t0.r) + 0x80) >> 8);
                        TEX->g = prev->b + ((prev->r * (t1.g - t0.g) + prev->g * (t2.g - t0.g) + 0x80) >> 8);
                    }
                }
            }
            else
            {
                invt3r  = ~t3.r;
                invt3g = ~t3.g;

                if (!convert)
                {

                    TEX->r = t3.r + ((((t1.r + t2.r) << 6) - (t3.r << 7) + ((invt3r + t0.r) << 6) + 0xc0) >> 8);
                    TEX->g = t3.g + ((((t1.g + t2.g) << 6) - (t3.g << 7) + ((invt3g + t0.g) << 6) + 0xc0) >> 8);
                }
                else
                {
                    TEX->r = prev->b + ((prev->r * (t2.r - t3.r) + prev->g * (t1.r - t3.r) + ((invt3r + t0.r) << 6) + 0xc0) >> 8);
                    TEX->g = prev->b + ((prev->r * (t2.g - t3.g) + prev->g * (t1.g - t3.g) + ((invt3g + t0.g) << 6) + 0xc0) >> 8);
                }
            }

            if (!center)
            {
                if (!convert)
                {
                    if (upper)
                    {
                        invsf = 0x20 - sfrac;

                        TEX->b = t3.b + ((invsf * (t2.b - t3.b) + invtf * (t1.b - t3.b) + 0x10) >> 5);
                        TEX->a = t3.a + ((invsf * (t2.a - t3.a) + invtf * (t1.a - t3.a) + 0x10) >> 5);
                    }
                    else
                    {
                        TEX->b = t0.b + ((sfrac * (t1.b - t0.b) + tfrac * (t2.b - t0.b) + 0x10) >> 5);
                        TEX->a = t0.a + ((sfrac * (t1.a - t0.a) + tfrac * (t2.a - t0.a) + 0x10) >> 5);
                    }
                }
                else
                {
                    if (upper)
                    {
                        TEX->b = prev->b + ((prev->r * (t2.b - t3.b) + prev->g * (t1.b - t3.b) + 0x80) >> 8);
                        TEX->a = prev->b + ((prev->r * (t2.a - t3.a) + prev->g * (t1.a - t3.a) + 0x80) >> 8);
                    }
                    else
                    {
                        TEX->b = prev->b + ((prev->r * (t1.b - t0.b) + prev->g * (t2.b - t0.b) + 0x80) >> 8);
                        TEX->a = prev->b + ((prev->r * (t1.a - t0.a) + prev->g * (t2.a - t0.a) + 0x80) >> 8);
                    }
                }
            }
            else
            {
                invt3b = ~t3.b;
                invt3a = ~t3.a;

                if (!convert)
                {
                    TEX->b = t3.b + ((((t1.b + t2.b) << 6) - (t3.b << 7) + ((invt3b + t0.b) << 6) + 0xc0) >> 8);
                    TEX->a = t3.a + ((((t1.a + t2.a) << 6) - (t3.a << 7) + ((invt3a + t0.a) << 6) + 0xc0) >> 8);
                }
                else
                {
                    TEX->b = prev->b + ((prev->r * (t2.b - t3.b) + prev->g * (t1.b - t3.b) + ((invt3b + t0.b) << 6) + 0xc0) >> 8);
                    TEX->a = prev->b + ((prev->r * (t2.a - t3.a) + prev->g * (t1.a - t3.a) + ((invt3a + t0.a) << 6) + 0xc0) >> 8);
                }
            }
        }
        else
        {

            if (convert)
                t0 = t3 = *prev;


            if (upperrg)
            {
                if (upper)
                {
                    TEX->r = t3.b + ((k0_tf * t3.g + 0x80) >> 8);
                    TEX->g = t3.b + ((k1_tf * t3.r + k2_tf * t3.g + 0x80) >> 8);
                    TEX->b = t3.b + ((k3_tf * t3.r + 0x80) >> 8);
                    TEX->a = t3.b;
                }
                else
                {
                    TEX->r = t0.b + ((k0_tf * t3.g + 0x80) >> 8);
                    TEX->g = t0.b + ((k1_tf * t3.r + k2_tf * t3.g + 0x80) >> 8);
                    TEX->b = t0.b + ((k3_tf * t3.r + 0x80) >> 8);
                    TEX->a = t0.b;
                }
            }
            else
            {
                if (upper)
                {
                    TEX->r = t3.b + ((k0_tf * t0.g + 0x80) >> 8);
                    TEX->g = t3.b + ((k1_tf * t0.r + k2_tf * t0.g + 0x80) >> 8);
                    TEX->b = t3.b + ((k3_tf * t0.r + 0x80) >> 8);
                    TEX->a = t3.b;
                }
                else
                {
                    TEX->r = t0.b + ((k0_tf * t0.g + 0x80) >> 8);
                    TEX->g = t0.b + ((k1_tf * t0.r + k2_tf * t0.g + 0x80) >> 8);
                    TEX->b = t0.b + ((k3_tf * t0.r + 0x80) >> 8);
                    TEX->a = t0.b;
                }
            }
        }

        TEX->r &= 0x1ff;
        TEX->g &= 0x1ff;
        TEX->b &= 0x1ff;
        TEX->a &= 0x1ff;


    }
    else
    {




        tcclamp_cycle_light(&sss1, &sst1, maxs, maxt, tilenum);

        tcmask(&sss1, &sst1, tilenum);


        fetch_texel(&t0, sss1, sst1, tilenum);

        if (bilerp)
        {
            if (!convert)
            {
                TEX->r = t0.r & 0x1ff;
                TEX->g = t0.g & 0x1ff;
                TEX->b = t0.b;
                TEX->a = t0.a;
            }
            else
                TEX->r = TEX->g = TEX->b = TEX->a = prev->b;
        }
        else
        {
            if (convert)
                t0 = *prev;

            TEX->r = t0.b + ((k0_tf * t0.g + 0x80) >> 8);
            TEX->g = t0.b + ((k1_tf * t0.r + k2_tf * t0.g + 0x80) >> 8);
            TEX->b = t0.b + ((k3_tf * t0.r + 0x80) >> 8);
            TEX->a = t0.b;
            TEX->r &= 0x1ff;
            TEX->g &= 0x1ff;
            TEX->b &= 0x1ff;
        }
    }

}

static STRICTINLINE void tc_pipeline_copy(int32_t* sss0, int32_t* sss1, int32_t* sss2, int32_t* sss3, int32_t* sst, int tilenum)
{
    int ss0 = *sss0, ss1 = 0, ss2 = 0, ss3 = 0, st = *sst;

    tcshift_copy(&ss0, &st, tilenum);



    ss0 = TRELATIVE(ss0, tile[tilenum].sl);
    st = TRELATIVE(st, tile[tilenum].tl);
    ss0 = (ss0 >> 5);
    st = (st >> 5);

    ss1 = ss0 + 1;
    ss2 = ss0 + 2;
    ss3 = ss0 + 3;

    tcmask_copy(&ss0, &ss1, &ss2, &ss3, &st, tilenum);

    *sss0 = ss0;
    *sss1 = ss1;
    *sss2 = ss2;
    *sss3 = ss3;
    *sst = st;
}

static STRICTINLINE void tc_pipeline_load(int32_t* sss, int32_t* sst, int tilenum, int coord_quad)
{
    int sss1 = *sss, sst1 = *sst;
    sss1 = SIGN16(sss1);
    sst1 = SIGN16(sst1);


    sss1 = TRELATIVE(sss1, tile[tilenum].sl);
    sst1 = TRELATIVE(sst1, tile[tilenum].tl);



    if (!coord_quad)
    {
        sss1 = (sss1 >> 5);
        sst1 = (sst1 >> 5);
    }
    else
    {
        sss1 = (sss1 >> 3);
        sst1 = (sst1 >> 3);
    }

    *sss = sss1;
    *sst = sst1;
}

static void loading_pipeline(int start, int end, int tilenum, int coord_quad, int ltlut)
{


    int localdebugmode = 0, cnt = 0;
    int i, j;

    int dsinc, dtinc;
    dsinc = spans.ds;
    dtinc = spans.dt;

    int s, t;
    int ss, st;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int ti_index, length;

    uint32_t tmemidx0 = 0, tmemidx1 = 0, tmemidx2 = 0, tmemidx3 = 0;
    int dswap = 0;
    uint16_t* tmem16 = (uint16_t*)tmem;
    uint32_t readval0, readval1, readval2, readval3;
    uint32_t readidx32;
    uint64_t loadqword;
    uint16_t tempshort;
    int tmem_formatting = 0;
    uint32_t bit3fl = 0, hibit = 0;

    if (end > start && ltlut)
    {
        rdp_pipeline_crashed = 1;
        return;
    }

    if (tile[tilenum].format == FORMAT_YUV)
        tmem_formatting = 0;
    else if (tile[tilenum].format == FORMAT_RGBA && tile[tilenum].size == PIXEL_SIZE_32BIT)
        tmem_formatting = 1;
    else
        tmem_formatting = 2;

    int tiadvance = 0, spanadvance = 0;
    int tiptr = 0;
    switch (ti_size)
    {
    case PIXEL_SIZE_4BIT:
        rdp_pipeline_crashed = 1;
        return;
        break;
    case PIXEL_SIZE_8BIT:
        tiadvance = 8;
        spanadvance = 8;
        break;
    case PIXEL_SIZE_16BIT:
        if (!ltlut)
        {
            tiadvance = 8;
            spanadvance = 4;
        }
        else
        {
            tiadvance = 2;
            spanadvance = 1;
        }
        break;
    case PIXEL_SIZE_32BIT:
        tiadvance = 8;
        spanadvance = 2;
        break;
    }

    for (i = start; i <= end; i++)
    {
        xstart = span[i].lx;
        xend = span[i].unscrx;
        xendsc = span[i].rx;
        s = span[i].s;
        t = span[i].t;

        ti_index = ti_width * i + xend;
        tiptr = ti_address + PIXELS_TO_BYTES(ti_index, ti_size);

        length = (xstart - xend + 1) & 0xfff;

        if (trace_write_is_open()) {
            trace_write_rdram((tiptr >> 2), length);
        }

        for (j = 0; j < length; j+= spanadvance)
        {
            ss = s >> 16;
            st = t >> 16;







            sss = ss & 0xffff;
            sst = st & 0xffff;

            tc_pipeline_load(&sss, &sst, tilenum, coord_quad);

            dswap = sst & 1;


            get_tmem_idx(sss, sst, tilenum, &tmemidx0, &tmemidx1, &tmemidx2, &tmemidx3, &bit3fl, &hibit);

            readidx32 = (tiptr >> 2) & ~1;
            RREADIDX32(readval0, readidx32);
            readidx32++;
            RREADIDX32(readval1, readidx32);
            readidx32++;
            RREADIDX32(readval2, readidx32);
            readidx32++;
            RREADIDX32(readval3, readidx32);


            switch(tiptr & 7)
            {
            case 0:
                if (!ltlut)
                    loadqword = ((uint64_t)readval0 << 32) | readval1;
                else
                {
                    tempshort = readval0 >> 16;
                    loadqword = ((uint64_t)tempshort << 48) | ((uint64_t) tempshort << 32) | ((uint64_t) tempshort << 16) | tempshort;
                }
                break;
            case 1:
                loadqword = ((uint64_t)readval0 << 40) | ((uint64_t)readval1 << 8) | (readval2 >> 24);
                break;
            case 2:
                if (!ltlut)
                    loadqword = ((uint64_t)readval0 << 48) | ((uint64_t)readval1 << 16) | (readval2 >> 16);
                else
                {
                    tempshort = readval0 & 0xffff;
                    loadqword = ((uint64_t)tempshort << 48) | ((uint64_t) tempshort << 32) | ((uint64_t) tempshort << 16) | tempshort;
                }
                break;
            case 3:
                loadqword = ((uint64_t)readval0 << 56) | ((uint64_t)readval1 << 24) | (readval2 >> 8);
                break;
            case 4:
                if (!ltlut)
                    loadqword = ((uint64_t)readval1 << 32) | readval2;
                else
                {
                    tempshort = readval1 >> 16;
                    loadqword = ((uint64_t)tempshort << 48) | ((uint64_t) tempshort << 32) | ((uint64_t) tempshort << 16) | tempshort;
                }
                break;
            case 5:
                loadqword = ((uint64_t)readval1 << 40) | ((uint64_t)readval2 << 8) | (readval3 >> 24);
                break;
            case 6:
                if (!ltlut)
                    loadqword = ((uint64_t)readval1 << 48) | ((uint64_t)readval2 << 16) | (readval3 >> 16);
                else
                {
                    tempshort = readval1 & 0xffff;
                    loadqword = ((uint64_t)tempshort << 48) | ((uint64_t) tempshort << 32) | ((uint64_t) tempshort << 16) | tempshort;
                }
                break;
            case 7:
                loadqword = ((uint64_t)readval1 << 56) | ((uint64_t)readval2 << 24) | (readval3 >> 8);
                break;
            }


            switch(tmem_formatting)
            {
            case 0:
                readval0 = (uint32_t)((((loadqword >> 56) & 0xff) << 24) | (((loadqword >> 40) & 0xff) << 16) | (((loadqword >> 24) & 0xff) << 8) | (((loadqword >> 8) & 0xff) << 0));
                readval1 = (uint32_t)((((loadqword >> 48) & 0xff) << 24) | (((loadqword >> 32) & 0xff) << 16) | (((loadqword >> 16) & 0xff) << 8) | (((loadqword >> 0) & 0xff) << 0));
                if (bit3fl)
                {
                    tmem16[tmemidx2 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 >> 16);
                    tmem16[tmemidx3 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 & 0xffff);
                    tmem16[(tmemidx2 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 >> 16);
                    tmem16[(tmemidx3 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 & 0xffff);
                }
                else
                {
                    tmem16[tmemidx0 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 >> 16);
                    tmem16[tmemidx1 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 & 0xffff);
                    tmem16[(tmemidx0 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 >> 16);
                    tmem16[(tmemidx1 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 & 0xffff);
                }
                break;
            case 1:
                readval0 = (uint32_t)(((loadqword >> 48) << 16) | ((loadqword >> 16) & 0xffff));
                readval1 = (uint32_t)((((loadqword >> 32) & 0xffff) << 16) | (loadqword & 0xffff));

                if (bit3fl)
                {
                    tmem16[tmemidx2 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 >> 16);
                    tmem16[tmemidx3 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 & 0xffff);
                    tmem16[(tmemidx2 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 >> 16);
                    tmem16[(tmemidx3 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 & 0xffff);
                }
                else
                {
                    tmem16[tmemidx0 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 >> 16);
                    tmem16[tmemidx1 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 & 0xffff);
                    tmem16[(tmemidx0 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 >> 16);
                    tmem16[(tmemidx1 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 & 0xffff);
                }
                break;
            case 2:
                if (!dswap)
                {
                    if (!hibit)
                    {
                        tmem16[tmemidx0 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 48);
                        tmem16[tmemidx1 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 32);
                        tmem16[tmemidx2 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 16);
                        tmem16[tmemidx3 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword & 0xffff);
                    }
                    else
                    {
                        tmem16[(tmemidx0 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 48);
                        tmem16[(tmemidx1 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 32);
                        tmem16[(tmemidx2 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 16);
                        tmem16[(tmemidx3 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword & 0xffff);
                    }
                }
                else
                {
                    if (!hibit)
                    {
                        tmem16[tmemidx0 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 16);
                        tmem16[tmemidx1 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword & 0xffff);
                        tmem16[tmemidx2 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 48);
                        tmem16[tmemidx3 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 32);
                    }
                    else
                    {
                        tmem16[(tmemidx0 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 16);
                        tmem16[(tmemidx1 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword & 0xffff);
                        tmem16[(tmemidx2 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 48);
                        tmem16[(tmemidx3 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 32);
                    }
                }
            break;
            }


            s = (s + dsinc) & ~0x1f;
            t = (t + dtinc) & ~0x1f;
            tiptr += tiadvance;
        }
    }
}

static void edgewalker_for_loads(int32_t* lewdata)
{
    int j = 0;
    int xleft = 0, xright = 0;
    int xstart = 0, xend = 0;
    int s = 0, t = 0, w = 0;
    int dsdx = 0, dtdx = 0;
    int dsdy = 0, dtdy = 0;
    int dsde = 0, dtde = 0;
    int tilenum = 0, flip = 0;
    int32_t yl = 0, ym = 0, yh = 0;
    int32_t xl = 0, xm = 0, xh = 0;
    int32_t dxldy = 0, dxhdy = 0, dxmdy = 0;

    int cmd_id = CMD_ID(lewdata);
    int ltlut = (cmd_id == CMD_ID_LOAD_TLUT);
    int coord_quad = ltlut || (cmd_id == CMD_ID_LOAD_BLOCK);
    flip = 1;
    max_level = 0;
    tilenum = (lewdata[0] >> 16) & 7;


    yl = SIGN(lewdata[0], 14);
    ym = lewdata[1] >> 16;
    ym = SIGN(ym, 14);
    yh = SIGN(lewdata[1], 14);

    xl = SIGN(lewdata[2], 28);
    xh = SIGN(lewdata[3], 28);
    xm = SIGN(lewdata[4], 28);

    dxldy = 0;
    dxhdy = 0;
    dxmdy = 0;


    s    = lewdata[5] & 0xffff0000;
    t    = (lewdata[5] & 0xffff) << 16;
    w    = 0;
    dsdx = (lewdata[7] & 0xffff0000) | ((lewdata[6] >> 16) & 0xffff);
    dtdx = ((lewdata[7] << 16) & 0xffff0000)    | (lewdata[6] & 0xffff);
    dsde = 0;
    dtde = (lewdata[9] & 0xffff) << 16;
    dsdy = 0;
    dtdy = (lewdata[8] & 0xffff) << 16;

    spans.ds = dsdx & ~0x1f;
    spans.dt = dtdx & ~0x1f;
    spans.dw = 0;






    xright = xh & ~0x1;
    xleft = xm & ~0x1;

    int k = 0;

    int sign_dxhdy = 0;

    int do_offset = 0;

    int xfrac = 0;






#define ADJUST_ATTR_LOAD()                                      \
{                                                               \
    span[j].s = s & ~0x3ff;                                     \
    span[j].t = t & ~0x3ff;                                     \
}


#define ADDVALUES_LOAD() {  \
            t += dtde;      \
}

    int32_t maxxmx, minxhx;

    int spix = 0;
    int ycur =  yh & ~3;
    int ylfar = yl | 3;

    int valid_y = 1;
    int length = 0;
    int32_t xrsc = 0, xlsc = 0, stickybit = 0;
    int32_t yllimit = yl;
    int32_t yhlimit = yh;

    xfrac = 0;
    xend = xright >> 16;


    for (k = ycur; k <= ylfar; k++)
    {
        if (k == ym)
            xleft = xl & ~1;

        spix = k & 3;

        if (!(k & ~0xfff))
        {
            j = k >> 2;
            valid_y = !(k < yhlimit || k >= yllimit);

            if (spix == 0)
            {
                maxxmx = 0;
                minxhx = 0xfff;
            }

            xrsc = (xright >> 13) & 0x7ffe;



            xlsc = (xleft >> 13) & 0x7ffe;

            if (valid_y)
            {
                maxxmx = (((xlsc >> 3) & 0xfff) > maxxmx) ? (xlsc >> 3) & 0xfff : maxxmx;
                minxhx = (((xrsc >> 3) & 0xfff) < minxhx) ? (xrsc >> 3) & 0xfff : minxhx;
            }

            if (spix == 0)
            {
                span[j].unscrx = xend;
                ADJUST_ATTR_LOAD();
            }

            if (spix == 3)
            {
                span[j].lx = maxxmx;
                span[j].rx = minxhx;


            }


        }

        if (spix == 3)
        {
            ADDVALUES_LOAD();
        }



    }

    loading_pipeline(yhlimit >> 2, yllimit >> 2, tilenum, coord_quad, ltlut);
}


static const char *const image_format[] = { "RGBA", "YUV", "CI", "IA", "I", "???", "???", "???" };
static const char *const image_size[] = { "4-bit", "8-bit", "16-bit", "32-bit" };


static void rdp_invalid(const uint32_t* args)
{
}

static void rdp_noop(const uint32_t* args)
{
}


static void rdp_sync_load(const uint32_t* args)
{

}

static void rdp_sync_pipe(const uint32_t* args)
{


}

static void rdp_sync_tile(const uint32_t* args)
{

}

static void rdp_sync_full(const uint32_t* args)
{
    core_sync_dp();
}

static void rdp_set_convert(const uint32_t* args)
{
    int32_t k0 = (args[0] >> 13) & 0x1ff;
    int32_t k1 = (args[0] >> 4) & 0x1ff;
    int32_t k2 = ((args[0] & 0xf) << 5) | ((args[1] >> 27) & 0x1f);
    int32_t k3 = (args[1] >> 18) & 0x1ff;
    k0_tf = (SIGN(k0, 9) << 1) + 1;
    k1_tf = (SIGN(k1, 9) << 1) + 1;
    k2_tf = (SIGN(k2, 9) << 1) + 1;
    k3_tf = (SIGN(k3, 9) << 1) + 1;
    k4 = (args[1] >> 9) & 0x1ff;
    k5 = args[1] & 0x1ff;
}

static void rdp_set_prim_depth(const uint32_t* args)
{
    primitive_z = args[1] & (0x7fff << 16);


    primitive_delta_z = (uint16_t)(args[1]);
}

static void rdp_set_other_modes(const uint32_t* args)
{
    other_modes.cycle_type          = (args[0] >> 20) & 0x3;
    other_modes.persp_tex_en        = (args[0] & 0x80000) ? 1 : 0;
    other_modes.detail_tex_en       = (args[0] & 0x40000) ? 1 : 0;
    other_modes.sharpen_tex_en      = (args[0] & 0x20000) ? 1 : 0;
    other_modes.tex_lod_en          = (args[0] & 0x10000) ? 1 : 0;
    other_modes.en_tlut             = (args[0] & 0x08000) ? 1 : 0;
    other_modes.tlut_type           = (args[0] & 0x04000) ? 1 : 0;
    other_modes.sample_type         = (args[0] & 0x02000) ? 1 : 0;
    other_modes.mid_texel           = (args[0] & 0x01000) ? 1 : 0;
    other_modes.bi_lerp0            = (args[0] & 0x00800) ? 1 : 0;
    other_modes.bi_lerp1            = (args[0] & 0x00400) ? 1 : 0;
    other_modes.convert_one         = (args[0] & 0x00200) ? 1 : 0;
    other_modes.key_en              = (args[0] & 0x00100) ? 1 : 0;
    other_modes.rgb_dither_sel      = (args[0] >> 6) & 0x3;
    other_modes.alpha_dither_sel    = (args[0] >> 4) & 0x3;
    other_modes.blend_m1a_0         = (args[1] >> 30) & 0x3;
    other_modes.blend_m1a_1         = (args[1] >> 28) & 0x3;
    other_modes.blend_m1b_0         = (args[1] >> 26) & 0x3;
    other_modes.blend_m1b_1         = (args[1] >> 24) & 0x3;
    other_modes.blend_m2a_0         = (args[1] >> 22) & 0x3;
    other_modes.blend_m2a_1         = (args[1] >> 20) & 0x3;
    other_modes.blend_m2b_0         = (args[1] >> 18) & 0x3;
    other_modes.blend_m2b_1         = (args[1] >> 16) & 0x3;
    other_modes.force_blend         = (args[1] >> 14) & 1;
    other_modes.alpha_cvg_select    = (args[1] >> 13) & 1;
    other_modes.cvg_times_alpha     = (args[1] >> 12) & 1;
    other_modes.z_mode              = (args[1] >> 10) & 0x3;
    other_modes.cvg_dest            = (args[1] >> 8) & 0x3;
    other_modes.color_on_cvg        = (args[1] >> 7) & 1;
    other_modes.image_read_en       = (args[1] >> 6) & 1;
    other_modes.z_update_en         = (args[1] >> 5) & 1;
    other_modes.z_compare_en        = (args[1] >> 4) & 1;
    other_modes.antialias_en        = (args[1] >> 3) & 1;
    other_modes.z_source_sel        = (args[1] >> 2) & 1;
    other_modes.dither_alpha_en     = (args[1] >> 1) & 1;
    other_modes.alpha_compare_en    = (args[1]) & 1;

    set_blender_input(0, 0, &blender.i1a_r[0], &blender.i1a_g[0], &blender.i1a_b[0], &blender.i1b_a[0],
                      other_modes.blend_m1a_0, other_modes.blend_m1b_0);
    set_blender_input(0, 1, &blender.i2a_r[0], &blender.i2a_g[0], &blender.i2a_b[0], &blender.i2b_a[0],
                      other_modes.blend_m2a_0, other_modes.blend_m2b_0);
    set_blender_input(1, 0, &blender.i1a_r[1], &blender.i1a_g[1], &blender.i1a_b[1], &blender.i1b_a[1],
                      other_modes.blend_m1a_1, other_modes.blend_m1b_1);
    set_blender_input(1, 1, &blender.i2a_r[1], &blender.i2a_g[1], &blender.i2a_b[1], &blender.i2b_a[1],
                      other_modes.blend_m2a_1, other_modes.blend_m2b_1);

    other_modes.f.stalederivs = 1;
}

static void deduce_derivatives()
{

    other_modes.f.partialreject_1cycle = (blender.i2b_a[0] == &inv_pixel_color.a && blender.i1b_a[0] == &pixel_color.a);
    other_modes.f.partialreject_2cycle = (blender.i2b_a[1] == &inv_pixel_color.a && blender.i1b_a[1] == &pixel_color.a);


    other_modes.f.special_bsel0 = (blender.i2b_a[0] == &memory_color.a);
    other_modes.f.special_bsel1 = (blender.i2b_a[1] == &memory_color.a);


    other_modes.f.realblendershiftersneeded = (other_modes.f.special_bsel0 && other_modes.cycle_type == CYCLE_TYPE_1) || (other_modes.f.special_bsel1 && other_modes.cycle_type == CYCLE_TYPE_2);
    other_modes.f.interpixelblendershiftersneeded = (other_modes.f.special_bsel0 && other_modes.cycle_type == CYCLE_TYPE_2);

    other_modes.f.rgb_alpha_dither = (other_modes.rgb_dither_sel << 2) | other_modes.alpha_dither_sel;

    if (other_modes.rgb_dither_sel == 3)
        rgb_dither_ptr = rgb_dither_func[1];
    else
        rgb_dither_ptr = rgb_dither_func[0];

    tcdiv_ptr = tcdiv_func[other_modes.persp_tex_en];


    int texel1_used_in_cc1 = 0, texel0_used_in_cc1 = 0, texel0_used_in_cc0 = 0, texel1_used_in_cc0 = 0;
    int texels_in_cc0 = 0, texels_in_cc1 = 0;
    int lod_frac_used_in_cc1 = 0, lod_frac_used_in_cc0 = 0;

    if ((combiner.rgbmul_r[1] == &lod_frac) || (combiner.alphamul[1] == &lod_frac))
        lod_frac_used_in_cc1 = 1;
    if ((combiner.rgbmul_r[0] == &lod_frac) || (combiner.alphamul[0] == &lod_frac))
        lod_frac_used_in_cc0 = 1;

    if (combiner.rgbmul_r[1] == &texel1_color.r || combiner.rgbsub_a_r[1] == &texel1_color.r || combiner.rgbsub_b_r[1] == &texel1_color.r || combiner.rgbadd_r[1] == &texel1_color.r || \
        combiner.alphamul[1] == &texel1_color.a || combiner.alphasub_a[1] == &texel1_color.a || combiner.alphasub_b[1] == &texel1_color.a || combiner.alphaadd[1] == &texel1_color.a || \
        combiner.rgbmul_r[1] == &texel1_color.a)
        texel1_used_in_cc1 = 1;
    if (combiner.rgbmul_r[1] == &texel0_color.r || combiner.rgbsub_a_r[1] == &texel0_color.r || combiner.rgbsub_b_r[1] == &texel0_color.r || combiner.rgbadd_r[1] == &texel0_color.r || \
        combiner.alphamul[1] == &texel0_color.a || combiner.alphasub_a[1] == &texel0_color.a || combiner.alphasub_b[1] == &texel0_color.a || combiner.alphaadd[1] == &texel0_color.a || \
        combiner.rgbmul_r[1] == &texel0_color.a)
        texel0_used_in_cc1 = 1;
    if (combiner.rgbmul_r[0] == &texel1_color.r || combiner.rgbsub_a_r[0] == &texel1_color.r || combiner.rgbsub_b_r[0] == &texel1_color.r || combiner.rgbadd_r[0] == &texel1_color.r || \
        combiner.alphamul[0] == &texel1_color.a || combiner.alphasub_a[0] == &texel1_color.a || combiner.alphasub_b[0] == &texel1_color.a || combiner.alphaadd[0] == &texel1_color.a || \
        combiner.rgbmul_r[0] == &texel1_color.a)
        texel1_used_in_cc0 = 1;
    if (combiner.rgbmul_r[0] == &texel0_color.r || combiner.rgbsub_a_r[0] == &texel0_color.r || combiner.rgbsub_b_r[0] == &texel0_color.r || combiner.rgbadd_r[0] == &texel0_color.r || \
        combiner.alphamul[0] == &texel0_color.a || combiner.alphasub_a[0] == &texel0_color.a || combiner.alphasub_b[0] == &texel0_color.a || combiner.alphaadd[0] == &texel0_color.a || \
        combiner.rgbmul_r[0] == &texel0_color.a)
        texel0_used_in_cc0 = 1;
    texels_in_cc0 = texel0_used_in_cc0 || texel1_used_in_cc0;
    texels_in_cc1 = texel0_used_in_cc1 || texel1_used_in_cc1;


    if (texel1_used_in_cc1)
        render_spans_1cycle_ptr = render_spans_1cycle_func[2];
    else if (texel0_used_in_cc1 || lod_frac_used_in_cc1)
        render_spans_1cycle_ptr = render_spans_1cycle_func[1];
    else
        render_spans_1cycle_ptr = render_spans_1cycle_func[0];

    if (texel1_used_in_cc1)
        render_spans_2cycle_ptr = render_spans_2cycle_func[3];
    else if (texel1_used_in_cc0 || texel0_used_in_cc1)
        render_spans_2cycle_ptr = render_spans_2cycle_func[2];
    else if (texel0_used_in_cc0 || lod_frac_used_in_cc0 || lod_frac_used_in_cc1)
        render_spans_2cycle_ptr = render_spans_2cycle_func[1];
    else
        render_spans_2cycle_ptr = render_spans_2cycle_func[0];


    int lodfracused = 0;

    if ((other_modes.cycle_type == CYCLE_TYPE_2 && (lod_frac_used_in_cc0 || lod_frac_used_in_cc1)) || \
        (other_modes.cycle_type == CYCLE_TYPE_1 && lod_frac_used_in_cc1))
        lodfracused = 1;

    if ((other_modes.cycle_type == CYCLE_TYPE_1 && combiner.rgbsub_a_r[1] == &noise) || \
        (other_modes.cycle_type == CYCLE_TYPE_2 && (combiner.rgbsub_a_r[0] == &noise || combiner.rgbsub_a_r[1] == &noise)) || \
        other_modes.alpha_dither_sel == 2)
        get_dither_noise_ptr = get_dither_noise_func[0];
    else if (other_modes.f.rgb_alpha_dither != 0xf)
        get_dither_noise_ptr = get_dither_noise_func[1];
    else
        get_dither_noise_ptr = get_dither_noise_func[2];

    other_modes.f.dolod = other_modes.tex_lod_en || lodfracused;
}

static void rdp_set_tile_size(const uint32_t* args)
{
    int tilenum = (args[1] >> 24) & 0x7;
    tile[tilenum].sl = (args[0] >> 12) & 0xfff;
    tile[tilenum].tl = (args[0] >>  0) & 0xfff;
    tile[tilenum].sh = (args[1] >> 12) & 0xfff;
    tile[tilenum].th = (args[1] >>  0) & 0xfff;

    calculate_clamp_diffs(tilenum);
}

static void rdp_load_block(const uint32_t* args)
{
    int tilenum = (args[1] >> 24) & 0x7;
    int sl, sh, tl, dxt;


    tile[tilenum].sl = sl = ((args[0] >> 12) & 0xfff);
    tile[tilenum].tl = tl = ((args[0] >>  0) & 0xfff);
    tile[tilenum].sh = sh = ((args[1] >> 12) & 0xfff);
    tile[tilenum].th = dxt  = ((args[1] >>  0) & 0xfff);

    calculate_clamp_diffs(tilenum);

    int tlclamped = tl & 0x3ff;

    int32_t lewdata[10];

    lewdata[0] = (args[0] & 0xff000000) | (0x10 << 19) | (tilenum << 16) | ((tlclamped << 2) | 3);
    lewdata[1] = (((tlclamped << 2) | 3) << 16) | (tlclamped << 2);
    lewdata[2] = sh << 16;
    lewdata[3] = sl << 16;
    lewdata[4] = sh << 16;
    lewdata[5] = ((sl << 3) << 16) | (tl << 3);
    lewdata[6] = (dxt & 0xff) << 8;
    lewdata[7] = ((0x80 >> ti_size) << 16) | (dxt >> 8);
    lewdata[8] = 0x20;
    lewdata[9] = 0x20;

    edgewalker_for_loads(lewdata);

}

static void rdp_load_tlut(const uint32_t* args)
{


    tile_tlut_common_cs_decoder(args);
}

static void rdp_load_tile(const uint32_t* args)
{
    tile_tlut_common_cs_decoder(args);
}

static void tile_tlut_common_cs_decoder(const uint32_t* args)
{
    int tilenum = (args[1] >> 24) & 0x7;
    int sl, tl, sh, th;


    tile[tilenum].sl = sl = ((args[0] >> 12) & 0xfff);
    tile[tilenum].tl = tl = ((args[0] >>  0) & 0xfff);
    tile[tilenum].sh = sh = ((args[1] >> 12) & 0xfff);
    tile[tilenum].th = th = ((args[1] >>  0) & 0xfff);

    calculate_clamp_diffs(tilenum);


    int32_t lewdata[10];

    lewdata[0] = (args[0] & 0xff000000) | (0x10 << 19) | (tilenum << 16) | (th | 3);
    lewdata[1] = ((th | 3) << 16) | (tl);
    lewdata[2] = ((sh >> 2) << 16) | ((sh & 3) << 14);
    lewdata[3] = ((sl >> 2) << 16) | ((sl & 3) << 14);
    lewdata[4] = ((sh >> 2) << 16) | ((sh & 3) << 14);
    lewdata[5] = ((sl << 3) << 16) | (tl << 3);
    lewdata[6] = 0;
    lewdata[7] = (0x200 >> ti_size) << 16;
    lewdata[8] = 0x20;
    lewdata[9] = 0x20;

    edgewalker_for_loads(lewdata);
}

static void rdp_set_tile(const uint32_t* args)
{
    int tilenum = (args[1] >> 24) & 0x7;

    tile[tilenum].format    = (args[0] >> 21) & 0x7;
    tile[tilenum].size      = (args[0] >> 19) & 0x3;
    tile[tilenum].line      = (args[0] >>  9) & 0x1ff;
    tile[tilenum].tmem      = (args[0] >>  0) & 0x1ff;
    tile[tilenum].palette   = (args[1] >> 20) & 0xf;
    tile[tilenum].ct        = (args[1] >> 19) & 0x1;
    tile[tilenum].mt        = (args[1] >> 18) & 0x1;
    tile[tilenum].mask_t    = (args[1] >> 14) & 0xf;
    tile[tilenum].shift_t   = (args[1] >> 10) & 0xf;
    tile[tilenum].cs        = (args[1] >>  9) & 0x1;
    tile[tilenum].ms        = (args[1] >>  8) & 0x1;
    tile[tilenum].mask_s    = (args[1] >>  4) & 0xf;
    tile[tilenum].shift_s   = (args[1] >>  0) & 0xf;

    calculate_tile_derivs(tilenum);
}

static void rdp_fill_rect(const uint32_t* args)
{
    uint32_t xl = (args[0] >> 12) & 0xfff;
    uint32_t yl = (args[0] >>  0) & 0xfff;
    uint32_t xh = (args[1] >> 12) & 0xfff;
    uint32_t yh = (args[1] >>  0) & 0xfff;

    if (other_modes.cycle_type == CYCLE_TYPE_FILL || other_modes.cycle_type == CYCLE_TYPE_COPY)
        yl |= 3;

    uint32_t xlint = (xl >> 2) & 0x3ff;
    uint32_t xhint = (xh >> 2) & 0x3ff;

    int32_t ewdata[CMD_MAX_INTS];
    ewdata[0] = (0x3680 << 16) | yl;
    ewdata[1] = (yl << 16) | yh;
    ewdata[2] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[3] = 0;
    ewdata[4] = (xhint << 16) | ((xh & 3) << 14);
    ewdata[5] = 0;
    ewdata[6] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[7] = 0;
    memset(&ewdata[8], 0, 36 * sizeof(int32_t));

    edgewalker_for_prims(ewdata);
}

static void rdp_set_fill_color(const uint32_t* args)
{
    fill_color = args[1];
}

static void rdp_set_texture_image(const uint32_t* args)
{
    ti_format   = (args[0] >> 21) & 0x7;
    ti_size     = (args[0] >> 19) & 0x3;
    ti_width    = (args[0] & 0x3ff) + 1;
    ti_address  = args[1] & 0x0ffffff;



}

static STRICTINLINE int32_t normalize_dzpix(int32_t sum)
{
    if (sum & 0xc000)
        return 0x8000;
    if (!(sum & 0xffff))
        return 1;

    if (sum == 1)
        return 3;

    for(int count = 0x2000; count > 0; count >>= 1)
    {
        if (sum & count)
            return(count << 1);
    }
    msg_error("normalize_dzpix: invalid codepath taken");
    return 0;
}

static STRICTINLINE int32_t clamp(int32_t value,int32_t min,int32_t max)
{
    if (value < min)
        return min;
    else if (value > max)
        return max;
    else
        return value;
}

static INLINE void calculate_clamp_diffs(uint32_t i)
{
    tile[i].f.clampdiffs = ((tile[i].sh >> 2) - (tile[i].sl >> 2)) & 0x3ff;
    tile[i].f.clampdifft = ((tile[i].th >> 2) - (tile[i].tl >> 2)) & 0x3ff;
}


static INLINE void calculate_tile_derivs(uint32_t i)
{
    tile[i].f.clampens = tile[i].cs || !tile[i].mask_s;
    tile[i].f.clampent = tile[i].ct || !tile[i].mask_t;
    tile[i].f.masksclamped = tile[i].mask_s <= 10 ? tile[i].mask_s : 10;
    tile[i].f.masktclamped = tile[i].mask_t <= 10 ? tile[i].mask_t : 10;
    tile[i].f.notlutswitch = (tile[i].format << 2) | tile[i].size;
    tile[i].f.tlutswitch = (tile[i].size << 2) | ((tile[i].format + 2) & 3);

    if (tile[i].format < 5)
    {
        tile[i].f.notlutswitch = (tile[i].format << 2) | tile[i].size;
        tile[i].f.tlutswitch = (tile[i].size << 2) | ((tile[i].format + 2) & 3);
    }
    else
    {
        tile[i].f.notlutswitch = 0x10 | tile[i].size;
        tile[i].f.tlutswitch = (tile[i].size << 2) | 2;
    }
}

static void rgb_dither_complete(int* r, int* g, int* b, int dith)
{

    int32_t newr = *r, newg = *g, newb = *b;
    int32_t rcomp, gcomp, bcomp;


    if (newr > 247)
        newr = 255;
    else
        newr = (newr & 0xf8) + 8;
    if (newg > 247)
        newg = 255;
    else
        newg = (newg & 0xf8) + 8;
    if (newb > 247)
        newb = 255;
    else
        newb = (newb & 0xf8) + 8;

    if (other_modes.rgb_dither_sel != 2)
        rcomp = gcomp = bcomp = dith;
    else
    {
        rcomp = dith & 7;
        gcomp = (dith >> 3) & 7;
        bcomp = (dith >> 6) & 7;
    }





    int32_t replacesign = (rcomp - (*r & 7)) >> 31;

    int32_t ditherdiff = newr - *r;
    *r = *r + (ditherdiff & replacesign);

    replacesign = (gcomp - (*g & 7)) >> 31;
    ditherdiff = newg - *g;
    *g = *g + (ditherdiff & replacesign);

    replacesign = (bcomp - (*b & 7)) >> 31;
    ditherdiff = newb - *b;
    *b = *b + (ditherdiff & replacesign);

}

static void rgb_dither_nothing(int* r, int* g, int* b, int dith)
{
}


static void get_dither_noise_complete(int x, int y, int* cdith, int* adith)
{


    noise = ((irand() & 7) << 6) | 0x20;


    int dithindex;
    switch(other_modes.f.rgb_alpha_dither)
    {
    case 0:
        dithindex = ((y & 3) << 2) | (x & 3);
        *adith = *cdith = magic_matrix[dithindex];
        break;
    case 1:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = magic_matrix[dithindex];
        *adith = (~(*cdith)) & 7;
        break;
    case 2:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = magic_matrix[dithindex];
        *adith = (noise >> 6) & 7;
        break;
    case 3:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = magic_matrix[dithindex];
        *adith = 0;
        break;
    case 4:
        dithindex = ((y & 3) << 2) | (x & 3);
        *adith = *cdith = bayer_matrix[dithindex];
        break;
    case 5:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = bayer_matrix[dithindex];
        *adith = (~(*cdith)) & 7;
        break;
    case 6:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = bayer_matrix[dithindex];
        *adith = (noise >> 6) & 7;
        break;
    case 7:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = bayer_matrix[dithindex];
        *adith = 0;
        break;
    case 8:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = irand();
        *adith = magic_matrix[dithindex];
        break;
    case 9:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = irand();
        *adith = (~magic_matrix[dithindex]) & 7;
        break;
    case 10:
        *cdith = irand();
        *adith = (noise >> 6) & 7;
        break;
    case 11:
        *cdith = irand();
        *adith = 0;
        break;
    case 12:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = 7;
        *adith = bayer_matrix[dithindex];
        break;
    case 13:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = 7;
        *adith = (~bayer_matrix[dithindex]) & 7;
        break;
    case 14:
        *cdith = 7;
        *adith = (noise >> 6) & 7;
        break;
    case 15:
        *cdith = 7;
        *adith = 0;
        break;
    }
}


static void get_dither_only(int x, int y, int* cdith, int* adith)
{
    int dithindex;
    switch(other_modes.f.rgb_alpha_dither)
    {
    case 0:
        dithindex = ((y & 3) << 2) | (x & 3);
        *adith = *cdith = magic_matrix[dithindex];
        break;
    case 1:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = magic_matrix[dithindex];
        *adith = (~(*cdith)) & 7;
        break;
    case 2:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = magic_matrix[dithindex];
        *adith = (noise >> 6) & 7;
        break;
    case 3:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = magic_matrix[dithindex];
        *adith = 0;
        break;
    case 4:
        dithindex = ((y & 3) << 2) | (x & 3);
        *adith = *cdith = bayer_matrix[dithindex];
        break;
    case 5:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = bayer_matrix[dithindex];
        *adith = (~(*cdith)) & 7;
        break;
    case 6:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = bayer_matrix[dithindex];
        *adith = (noise >> 6) & 7;
        break;
    case 7:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = bayer_matrix[dithindex];
        *adith = 0;
        break;
    case 8:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = irand();
        *adith = magic_matrix[dithindex];
        break;
    case 9:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = irand();
        *adith = (~magic_matrix[dithindex]) & 7;
        break;
    case 10:
        *cdith = irand();
        *adith = (noise >> 6) & 7;
        break;
    case 11:
        *cdith = irand();
        *adith = 0;
        break;
    case 12:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = 7;
        *adith = bayer_matrix[dithindex];
        break;
    case 13:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = 7;
        *adith = (~bayer_matrix[dithindex]) & 7;
        break;
    case 14:
        *cdith = 7;
        *adith = (noise >> 6) & 7;
        break;
    case 15:
        *cdith = 7;
        *adith = 0;
        break;
    }
}

static void get_dither_nothing(int x, int y, int* cdith, int* adith)
{
}









static void tcdiv_nopersp(int32_t ss, int32_t st, int32_t sw, int32_t* sss, int32_t* sst)
{



    *sss = (SIGN16(ss)) & 0x1ffff;
    *sst = (SIGN16(st)) & 0x1ffff;
}

static void tcdiv_persp(int32_t ss, int32_t st, int32_t sw, int32_t* sss, int32_t* sst)
{


    int w_carry = 0;
    int shift;
    int tlu_rcp;
    int sprod, tprod;
    int outofbounds_s, outofbounds_t;
    int tempmask;
    int shift_value;
    int32_t temps, tempt;



    int overunder_s = 0, overunder_t = 0;


    if (SIGN16(sw) <= 0)
        w_carry = 1;

    sw &= 0x7fff;



    shift = tcdiv_table[sw];
    tlu_rcp = shift >> 4;
    shift &= 0xf;

    sprod = SIGN16(ss) * tlu_rcp;
    tprod = SIGN16(st) * tlu_rcp;




    tempmask = ((1 << 30) - 1) & -((1 << 29) >> shift);

    outofbounds_s = sprod & tempmask;
    outofbounds_t = tprod & tempmask;

    if (shift != 0xe)
    {
        shift_value = 13 - shift;
        temps = sprod = (sprod >> shift_value);
        tempt = tprod = (tprod >> shift_value);
    }
    else
    {
        temps = sprod << 1;
        tempt = tprod << 1;
    }

    if (outofbounds_s != tempmask && outofbounds_s != 0)
    {
        if (!(sprod & (1 << 29)))
            overunder_s = 2 << 17;
        else
            overunder_s = 1 << 17;
    }

    if (outofbounds_t != tempmask && outofbounds_t != 0)
    {
        if (!(tprod & (1 << 29)))
            overunder_t = 2 << 17;
        else
            overunder_t = 1 << 17;
    }

    if (w_carry)
    {
        overunder_s |= (2 << 17);
        overunder_t |= (2 << 17);
    }

    *sss = (temps & 0x1ffff) | overunder_s;
    *sst = (tempt & 0x1ffff) | overunder_t;
}

static STRICTINLINE void tclod_2cycle_current(int32_t* sss, int32_t* sst, int32_t nexts, int32_t nextt, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2)
{








    int nextys, nextyt, nextysw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile;
    uint32_t magnify = 0;
    uint32_t distant = 0;
    int inits = *sss, initt = *sst;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {






        nextys = (s + spans.dsdy) >> 16;
        nextyt = (t + spans.dtdy) >> 16;
        nextysw = (w + spans.dwdy) >> 16;

        tcdiv_ptr(nextys, nextyt, nextysw, &nextys, &nextyt);

        lodclamp = (initt & 0x60000) || (nextt & 0x60000) || (inits & 0x60000) || (nexts & 0x60000) || (nextys & 0x60000) || (nextyt & 0x60000);




        if (!lodclamp)
        {
            tclod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
            tclod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
        }

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &lod_frac);


        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;
            if (!other_modes.detail_tex_en)
            {
                *t1 = (prim_tile + l_tile) & 7;
                if (!(distant || (!other_modes.sharpen_tex_en && magnify)))
                    *t2 = (*t1 + 1) & 7;
                else
                    *t2 = *t1;
            }
            else
            {
                if (!magnify)
                    *t1 = (prim_tile + l_tile + 1);
                else
                    *t1 = (prim_tile + l_tile);
                *t1 &= 7;
                if (!distant && !magnify)
                    *t2 = (prim_tile + l_tile + 2) & 7;
                else
                    *t2 = (prim_tile + l_tile + 1) & 7;
            }
        }
    }
}


static STRICTINLINE void tclod_2cycle_current_simple(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2)
{
    int nextys, nextyt, nextysw, nexts, nextt, nextsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile;
    uint32_t magnify = 0;
    uint32_t distant = 0;
    int inits = *sss, initt = *sst;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {
        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        nextys = (s + spans.dsdy) >> 16;
        nextyt = (t + spans.dtdy) >> 16;
        nextysw = (w + spans.dwdy) >> 16;

        tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        tcdiv_ptr(nextys, nextyt, nextysw, &nextys, &nextyt);

        lodclamp = (initt & 0x60000) || (nextt & 0x60000) || (inits & 0x60000) || (nexts & 0x60000) || (nextys & 0x60000) || (nextyt & 0x60000);

        if (!lodclamp)
        {
            tclod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
            tclod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
        }

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &lod_frac);

        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;
            if (!other_modes.detail_tex_en)
            {
                *t1 = (prim_tile + l_tile) & 7;
                if (!(distant || (!other_modes.sharpen_tex_en && magnify)))
                    *t2 = (*t1 + 1) & 7;
                else
                    *t2 = *t1;
            }
            else
            {
                if (!magnify)
                    *t1 = (prim_tile + l_tile + 1);
                else
                    *t1 = (prim_tile + l_tile);
                *t1 &= 7;
                if (!distant && !magnify)
                    *t2 = (prim_tile + l_tile + 2) & 7;
                else
                    *t2 = (prim_tile + l_tile + 1) & 7;
            }
        }
    }
}


static STRICTINLINE void tclod_2cycle_current_notexel1(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1)
{
    int nextys, nextyt, nextysw, nexts, nextt, nextsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile;
    uint32_t magnify = 0;
    uint32_t distant = 0;
    int inits = *sss, initt = *sst;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {
        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        nextys = (s + spans.dsdy) >> 16;
        nextyt = (t + spans.dtdy) >> 16;
        nextysw = (w + spans.dwdy) >> 16;

        tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        tcdiv_ptr(nextys, nextyt, nextysw, &nextys, &nextyt);

        lodclamp = (initt & 0x60000) || (nextt & 0x60000) || (inits & 0x60000) || (nexts & 0x60000) || (nextys & 0x60000) || (nextyt & 0x60000);

        if (!lodclamp)
        {
            tclod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
            tclod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
        }

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &lod_frac);

        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;
            if (!other_modes.detail_tex_en || magnify)
                *t1 = (prim_tile + l_tile) & 7;
            else
                *t1 = (prim_tile + l_tile + 1) & 7;
        }

    }
}

static STRICTINLINE void tclod_2cycle_next(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2, int32_t* prelodfrac)
{
    int nexts, nextt, nextsw, nextys, nextyt, nextysw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile;
    uint32_t magnify = 0;
    uint32_t distant = 0;
    int inits = *sss, initt = *sst;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {
        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        nextys = (s + spans.dsdy) >> 16;
        nextyt = (t + spans.dtdy) >> 16;
        nextysw = (w + spans.dwdy) >> 16;

        tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        tcdiv_ptr(nextys, nextyt, nextysw, &nextys, &nextyt);

        lodclamp = (initt & 0x60000) || (nextt & 0x60000) || (inits & 0x60000) || (nexts & 0x60000) || (nextys & 0x60000) || (nextyt & 0x60000);

        if (!lodclamp)
        {
            tclod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
            tclod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
        }

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, prelodfrac);

        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;
            if (!other_modes.detail_tex_en)
            {
                *t1 = (prim_tile + l_tile) & 7;
                if (!(distant || (!other_modes.sharpen_tex_en && magnify)))
                    *t2 = (*t1 + 1) & 7;
                else
                    *t2 = *t1;
            }
            else
            {
                if (!magnify)
                    *t1 = (prim_tile + l_tile + 1);
                else
                    *t1 = (prim_tile + l_tile);
                *t1 &= 7;
                if (!distant && !magnify)
                    *t2 = (prim_tile + l_tile + 2) & 7;
                else
                    *t2 = (prim_tile + l_tile + 1) & 7;
            }
        }
    }
}

static STRICTINLINE void tclod_1cycle_current(int32_t* sss, int32_t* sst, int32_t nexts, int32_t nextt, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs)
{









    int fars, fart, farsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile = 0, magnify = 0, distant = 0;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {
        int nextscan = scanline + 1;


        if (span[nextscan].validline)
        {
            if (!sigs->endspan || !sigs->longspan)
            {
                if (!(sigs->preendspan && sigs->longspan) && !(sigs->endspan && sigs->midspan))
                {
                    farsw = (w + (dwinc << 1)) >> 16;
                    fars = (s + (dsinc << 1)) >> 16;
                    fart = (t + (dtinc << 1)) >> 16;
                }
                else
                {
                    farsw = (w - dwinc) >> 16;
                    fars = (s - dsinc) >> 16;
                    fart = (t - dtinc) >> 16;
                }
            }
            else
            {
                fart = (span[nextscan].t + dtinc) >> 16;
                fars = (span[nextscan].s + dsinc) >> 16;
                farsw = (span[nextscan].w + dwinc) >> 16;
            }
        }
        else
        {
            farsw = (w + (dwinc << 1)) >> 16;
            fars = (s + (dsinc << 1)) >> 16;
            fart = (t + (dtinc << 1)) >> 16;
        }

        tcdiv_ptr(fars, fart, farsw, &fars, &fart);

        lodclamp = (fart & 0x60000) || (nextt & 0x60000) || (fars & 0x60000) || (nexts & 0x60000);




        if (!lodclamp)
            tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &lod_frac);

        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;



            if (!other_modes.detail_tex_en || magnify)
                *t1 = (prim_tile + l_tile) & 7;
            else
                *t1 = (prim_tile + l_tile + 1) & 7;
        }
    }
}



static STRICTINLINE void tclod_1cycle_current_simple(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs)
{
    int fars, fart, farsw, nexts, nextt, nextsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile = 0, magnify = 0, distant = 0;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {

        int nextscan = scanline + 1;
        if (span[nextscan].validline)
        {
            if (!sigs->endspan || !sigs->longspan)
            {
                nextsw = (w + dwinc) >> 16;
                nexts = (s + dsinc) >> 16;
                nextt = (t + dtinc) >> 16;

                if (!(sigs->preendspan && sigs->longspan) && !(sigs->endspan && sigs->midspan))
                {
                    farsw = (w + (dwinc << 1)) >> 16;
                    fars = (s + (dsinc << 1)) >> 16;
                    fart = (t + (dtinc << 1)) >> 16;
                }
                else
                {
                    farsw = (w - dwinc) >> 16;
                    fars = (s - dsinc) >> 16;
                    fart = (t - dtinc) >> 16;
                }
            }
            else
            {
                nextt = span[nextscan].t >> 16;
                nexts = span[nextscan].s >> 16;
                nextsw = span[nextscan].w >> 16;
                fart = (span[nextscan].t + dtinc) >> 16;
                fars = (span[nextscan].s + dsinc) >> 16;
                farsw = (span[nextscan].w + dwinc) >> 16;
            }
        }
        else
        {
            nextsw = (w + dwinc) >> 16;
            nexts = (s + dsinc) >> 16;
            nextt = (t + dtinc) >> 16;
            farsw = (w + (dwinc << 1)) >> 16;
            fars = (s + (dsinc << 1)) >> 16;
            fart = (t + (dtinc << 1)) >> 16;
        }

        tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        tcdiv_ptr(fars, fart, farsw, &fars, &fart);

        lodclamp = (fart & 0x60000) || (nextt & 0x60000) || (fars & 0x60000) || (nexts & 0x60000);

        if (!lodclamp)
            tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &lod_frac);

        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;
            if (!other_modes.detail_tex_en || magnify)
                *t1 = (prim_tile + l_tile) & 7;
            else
                *t1 = (prim_tile + l_tile + 1) & 7;
        }
    }
}

static STRICTINLINE void tclod_1cycle_next(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs, int32_t* prelodfrac)
{
    int nexts, nextt, nextsw, fars, fart, farsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile = 0, magnify = 0, distant = 0;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {

        int nextscan = scanline + 1;

        if (span[nextscan].validline)
        {

            if (!sigs->nextspan)
            {
                if (!sigs->endspan || !sigs->longspan)
                {
                    nextsw = (w + dwinc) >> 16;
                    nexts = (s + dsinc) >> 16;
                    nextt = (t + dtinc) >> 16;

                    if (!(sigs->preendspan && sigs->longspan) && !(sigs->endspan && sigs->midspan))
                    {
                        farsw = (w + (dwinc << 1)) >> 16;
                        fars = (s + (dsinc << 1)) >> 16;
                        fart = (t + (dtinc << 1)) >> 16;
                    }
                    else
                    {
                        farsw = (w - dwinc) >> 16;
                        fars = (s - dsinc) >> 16;
                        fart = (t - dtinc) >> 16;
                    }
                }
                else
                {
                    nextt = span[nextscan].t;
                    nexts = span[nextscan].s;
                    nextsw = span[nextscan].w;
                    fart = (nextt + dtinc) >> 16;
                    fars = (nexts + dsinc) >> 16;
                    farsw = (nextsw + dwinc) >> 16;
                    nextt >>= 16;
                    nexts >>= 16;
                    nextsw >>= 16;
                }
            }
            else
            {









                if (sigs->longspan)
                {
                    nextt = (span[nextscan].t + dtinc) >> 16;
                    nexts = (span[nextscan].s + dsinc) >> 16;
                    nextsw = (span[nextscan].w + dwinc) >> 16;
                    fart = (span[nextscan].t + (dtinc << 1)) >> 16;
                    fars = (span[nextscan].s + (dsinc << 1)) >> 16;
                    farsw = (span[nextscan].w  + (dwinc << 1)) >> 16;
                }
                else if (sigs->midspan)
                {
                    nextt = span[nextscan].t >> 16;
                    nexts = span[nextscan].s >> 16;
                    nextsw = span[nextscan].w >> 16;
                    fart = (span[nextscan].t + dtinc) >> 16;
                    fars = (span[nextscan].s + dsinc) >> 16;
                    farsw = (span[nextscan].w  + dwinc) >> 16;
                }
                else if (sigs->onelessthanmid)
                {
                    nextsw = (w + dwinc) >> 16;
                    nexts = (s + dsinc) >> 16;
                    nextt = (t + dtinc) >> 16;
                    farsw = (w - dwinc) >> 16;
                    fars = (s - dsinc) >> 16;
                    fart = (t - dtinc) >> 16;
                }
                else
                {
                    nextt = (t + dtinc) >> 16;
                    nexts = (s + dsinc) >> 16;
                    nextsw = (w + dwinc) >> 16;
                    fart = (t + (dtinc << 1)) >> 16;
                    fars = (s + (dsinc << 1)) >> 16;
                    farsw = (w + (dwinc << 1)) >> 16;
                }
            }
        }
        else
        {
            nextsw = (w + dwinc) >> 16;
            nexts = (s + dsinc) >> 16;
            nextt = (t + dtinc) >> 16;
            farsw = (w + (dwinc << 1)) >> 16;
            fars = (s + (dsinc << 1)) >> 16;
            fart = (t + (dtinc << 1)) >> 16;
        }

        tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        tcdiv_ptr(fars, fart, farsw, &fars, &fart);

        lodclamp = (fart & 0x60000) || (nextt & 0x60000) || (fars & 0x60000) || (nexts & 0x60000);



        if (!lodclamp)
            tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, prelodfrac);

        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;
            if (!other_modes.detail_tex_en || magnify)
                *t1 = (prim_tile + l_tile) & 7;
            else
                *t1 = (prim_tile + l_tile + 1) & 7;
        }
    }
}

static STRICTINLINE void tclod_copy(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1)
{




    int nexts, nextt, nextsw, fars, fart, farsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile = 0, magnify = 0, distant = 0;

    tclod_tcclamp(sss, sst);

    if (other_modes.tex_lod_en)
    {



        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        farsw = (w + (dwinc << 1)) >> 16;
        fars = (s + (dsinc << 1)) >> 16;
        fart = (t + (dtinc << 1)) >> 16;

        tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        tcdiv_ptr(fars, fart, farsw, &fars, &fart);

        lodclamp = (fart & 0x60000) || (nextt & 0x60000) || (fars & 0x60000) || (nexts & 0x60000);

        if (!lodclamp)
            tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

        if ((lod & 0x4000) || lodclamp)
        {


            magnify = 0;
            l_tile = max_level;
        }
        else if (lod < 32)
        {
            magnify = 1;
            l_tile = 0;
        }
        else
        {
            magnify = 0;
            l_tile =  log2table[(lod >> 5) & 0xff];

            if (max_level)
                distant = ((lod & 0x6000) || (l_tile >= max_level)) ? 1 : 0;
            else
                distant = 1;

            if (distant)
                l_tile = max_level;
        }

        if (!other_modes.detail_tex_en || magnify)
            *t1 = (prim_tile + l_tile) & 7;
        else
            *t1 = (prim_tile + l_tile + 1) & 7;
    }

}

static STRICTINLINE void get_texel1_1cycle(int32_t* s1, int32_t* t1, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, struct spansigs* sigs)
{
    int32_t nexts, nextt, nextsw;

    if (!sigs->endspan || !sigs->longspan || !span[scanline + 1].validline)
    {


        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
    }
    else
    {







        int32_t nextscan = scanline + 1;
        nextt = span[nextscan].t >> 16;
        nexts = span[nextscan].s >> 16;
        nextsw = span[nextscan].w >> 16;
    }

    tcdiv_ptr(nexts, nextt, nextsw, s1, t1);
}

static STRICTINLINE void get_nexttexel0_2cycle(int32_t* s1, int32_t* t1, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc)
{


    int32_t nexts, nextt, nextsw;
    nextsw = (w + dwinc) >> 16;
    nexts = (s + dsinc) >> 16;
    nextt = (t + dtinc) >> 16;

    tcdiv_ptr(nexts, nextt, nextsw, s1, t1);
}



static STRICTINLINE void tclod_4x17_to_15(int32_t scurr, int32_t snext, int32_t tcurr, int32_t tnext, int32_t previous, int32_t* lod)
{



    int dels = SIGN(snext, 17) - SIGN(scurr, 17);
    if (dels & 0x20000)
        dels = ~dels & 0x1ffff;
    int delt = SIGN(tnext, 17) - SIGN(tcurr, 17);
    if(delt & 0x20000)
        delt = ~delt & 0x1ffff;


    dels = (dels > delt) ? dels : delt;
    dels = (previous > dels) ? previous : dels;
    *lod = dels & 0x7fff;
    if (dels & 0x1c000)
        *lod |= 0x4000;
}

static STRICTINLINE void tclod_tcclamp(int32_t* sss, int32_t* sst)
{
    int32_t tempanded, temps = *sss, tempt = *sst;





    if (!(temps & 0x40000))
    {
        if (!(temps & 0x20000))
        {
            tempanded = temps & 0x18000;
            if (tempanded != 0x8000)
            {
                if (tempanded != 0x10000)
                    *sss &= 0xffff;
                else
                    *sss = 0x8000;
            }
            else
                *sss = 0x7fff;
        }
        else
            *sss = 0x8000;
    }
    else
        *sss = 0x7fff;

    if (!(tempt & 0x40000))
    {
        if (!(tempt & 0x20000))
        {
            tempanded = tempt & 0x18000;
            if (tempanded != 0x8000)
            {
                if (tempanded != 0x10000)
                    *sst &= 0xffff;
                else
                    *sst = 0x8000;
            }
            else
                *sst = 0x7fff;
        }
        else
            *sst = 0x8000;
    }
    else
        *sst = 0x7fff;

}


static STRICTINLINE void lodfrac_lodtile_signals(int lodclamp, int32_t lod, uint32_t* l_tile, uint32_t* magnify, uint32_t* distant, int32_t* lfdst)
{
    uint32_t ltil, dis, mag;
    int32_t lf;


    if ((lod & 0x4000) || lodclamp)
    {


        mag = 0;
        ltil = 7;
        dis = 1;
        lf = 0xff;
    }
    else if (lod < min_level)
    {


        mag = 1;
        ltil = 0;
        dis = max_level ? 0 : 1;

        if(!other_modes.sharpen_tex_en && !other_modes.detail_tex_en)
        {
            if (dis)
                lf = 0xff;
            else
                lf = 0;
        }
        else
        {
            lf = min_level << 3;
            if (other_modes.sharpen_tex_en)
                lf |= 0x100;
        }
    }
    else if (lod < 32)
    {
        mag = 1;
        ltil = 0;
        dis = max_level ? 0 : 1;

        if(!other_modes.sharpen_tex_en && !other_modes.detail_tex_en)
        {
            if (dis)
                lf = 0xff;
            else
                lf = 0;
        }
        else
        {
            lf = lod << 3;
            if (other_modes.sharpen_tex_en)
                lf |= 0x100;
        }
    }
    else
    {
        mag = 0;
        ltil =  log2table[(lod >> 5) & 0xff];

        if (max_level)
            dis = ((lod & 0x6000) || (ltil >= max_level)) ? 1 : 0;
        else
            dis = 1;


        if(!other_modes.sharpen_tex_en && !other_modes.detail_tex_en && dis)
            lf = 0xff;
        else
            lf = ((lod << 3) >> ltil) & 0xff;






    }

    *distant = dis;
    *l_tile = ltil;
    *magnify = mag;
    *lfdst = lf;
}
