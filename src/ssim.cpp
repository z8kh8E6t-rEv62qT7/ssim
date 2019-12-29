/*
 * Copyright (c) 2019, Romain Bailly
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include <rmgr/ssim.h>
#include "ssim_internal.h"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>


#ifndef RMGR_SSIM_REPORT_ERROR
    #ifdef _DEBUG
        #define RMGR_SSIM_REPORT_ERROR(...)  fprintf(stderr, __VA_ARGS__)
    #else
        #define RMGR_SSIM_REPORT_ERROR(...)
    #endif
#endif


#ifndef RMGR_SSIM_TILE_ALIGNMENT
    #define RMGR_SSIM_TILE_ALIGNMENT  64
#endif

#define ALIGN_UP(size, alignment)  (((size) + (alignment)-1) & ~size_t((alignment)-1))


//=================================================================================================
// Allocator stuff

#if defined(_MSC_VER)
    #include <malloc.h>
    static void* default_alloc(size_t size, size_t alignment) RMGR_NOEXCEPT
    {
        return ::_aligned_malloc(size, alignment);
    }
    static void default_dealloc(void* address) RMGR_NOEXCEPT
    {
        ::_aligned_free(address);
    }
#elif _POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600
    #include <stdlib.h>
    static void* default_alloc(size_t size, size_t alignment) RMGR_NOEXCEPT
    {
        void* address = NULL;
        int   err = ::posix_memalign(&address, alignment, size);
        return (err==0) ? address : NULL;
    }
    static void default_dealloc(void* address) RMGR_NOEXCEPT
    {
        ::free(address);
    }
#elif _ISOC11_SOURCE
    #include <stdlib.h>
    static void* default_alloc(size_t size, size_t alignment) RMGR_NOEXCEPT
    {
        size = (size + alignment-1) & ~(alignment - 1);
        return ::aligned_alloc(alignment, size);
    }
    static void default_dealloc(void* address) RMGR_NOEXCEPT
    {
        ::free(address);
    }
#else
    #include <stdlib.h>

    static void* default_alloc(size_t size, size_t alignment) RMGR_NOEXCEPT
    {
        assert(alignment >= sizeof(char*));

        const size_t extraSize = sizeof(char*) + alignment-1;
        char* buf = static_cast<char*>(::malloc(size + extraSize));
        if (buf == NULL)
            return NULL;

        char* alignedBuf = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(buf) + extraSize) & ~(alignment-1));
        assert(size_t(alignedBuf-buf) <= extraSize);
        assert(reinterpret_cast<uintptr_t>(alignedBuf) % sizeof(char*) == 0);
        reinterpret_cast<char**>(alignedBuf)[-1] = buf;
        return alignedBuf;
    }

    void default_dealloc(void* address) RMGR_NOEXCEPT
    {
        if (address == NULL)
            return;
        void* originalPtr = reinterpret_cast<char**>(address)[-1];
        ::free(originalPtr);
    }
#endif


namespace rmgr { namespace ssim
{


static AllocFct   g_alloc   = default_alloc;
static DeallocFct g_dealloc = default_dealloc;

static inline void* alloc(size_t size, size_t alignment=0) RMGR_NOEXCEPT
{
    assert(g_alloc != NULL);
    assert((alignment & (alignment-1)) == 0); // 0 or a power of 2
    const size_t minAlignment = sizeof(void*);
    return g_alloc(size, (alignment>=minAlignment)?alignment:minAlignment);
}

static inline void dealloc(void* address) RMGR_NOEXCEPT
{
    assert(g_dealloc != NULL);
    return g_dealloc(address);
}


static const unsigned CACHE_ALIGNMENT = 64;


static inline size_t align_up(size_t size, size_t alignment) RMGR_NOEXCEPT
{
    assert(alignment!=0 && (alignment & (alignment-1))==0); // Power of 2
    return (size + (alignment-1)) & ~(alignment-1);
}


/**
 * @brief Multiplies a tile by another tile
 */
static void multiply(Float* product, const Float* a, const Float* b, uint32_t width, uint32_t height, size_t stride, uint32_t margin) RMGR_NOEXCEPT
{
    a       -= margin * stride + margin,
    b       -= margin * stride + margin,
    product -= margin * stride + margin,
    width   += 2 * margin;
    height  += 2 * margin;
    stride  -= width;
    for (uint32_t y=0; y<height; ++y)
    {
        for (uint32_t x=0; x<width; ++x)
            *product++ = *a++ * *b++;
        a       += stride;
        b       += stride;
        product += stride;
    }
}


static inline Float gaussian_kernel(int x, int y, Float sigma) RMGR_NOEXCEPT
{
    Float sigma2      = sigma * sigma;
    Float numerator   = std::exp(-Float(x*x + y*y) / (2 * sigma2));
    Float denominator = Float(2*M_PI) * sigma2;
    return numerator / denominator;
}


static void precompute_gaussian_kernel(Float kernel[], int radius, Float sigma) RMGR_NOEXCEPT
{
    assert(radius != 0);

    const int size = 2*radius + 1;

    double sum = 0;

    // Compute kernel, exploiting (some) symmetries
    #define k(x,y)  kernel[(y)*size + (x)]
    for (int y=0; y<=radius; ++y)
    {
        for (int x=0; x<=radius; ++x)
            sum += k(x,y) = gaussian_kernel(x-radius, y-radius, sigma);
        for (int x=radius+1; x<size; ++x)
            sum += k(x,y) = k(size-1-x, y);
    }
    for (int y=radius+1; y<size; ++y)
        for (int x=0; x<size; ++x)
            sum += k(x,y) = k(x, size-1-y);
    #undef k

    // Normalize kernel
    for (int i = 0; i < size * size; i++)
        kernel[i] /= Float(sum);

#if 0
    printf("{\n");
    for (int y = 0; y < size; y++)
    {
        printf("  ");
        for (int x = 0; x < size; x++)
            printf("%19.17f, ", kernel[x + y * size]);
        printf("\n");
    }
    printf("}\n\n");
#endif
}


void gaussian_blur(Float* dest, ptrdiff_t destStride, const Float* srce, ptrdiff_t srceStride, int32_t width, int32_t height, const Float kernel[], int radius) RMGR_NOEXCEPT
{
    assert(width  > 0);
    assert(height > 0);
    assert(srceStride >= width + 2*radius);

#if 0

    // Naive but generic implementation, kept for reference
    const int32_t kernelStride = 2*radius + 1;
    for (int32_t yd=0; yd<height; ++yd)
    {
        memset(dest, 0, width*sizeof(Float));
        for (int32_t yk=-radius; yk<=radius; ++yk)
        {
            const Float* kernelRow = kernel + yk * kernelStride;
            for (int32_t xk=-radius; xk<=radius; ++xk)
            {
                const Float  k = kernelRow[xk];
                const Float* s = srce + (yd+yk) * srceStride + xk;
                Float*       d = dest;

                for (int32_t xd=0; xd<width; ++xd)
                    *d++ += *s++ * k;
            }
        }

        dest += destStride;
    }

#else

    #define MUL_ADD(a, b, c)  ((a) + (b) * (c))
    #define REUSE_S           (RMGR_ARCH_IS_ARM_64) // 64-bit ARM has 32 registers, enough to keep everything in registers

    // Faster, but tailored for the radius==5 case
    assert(radius == 5);
    const int32_t kernelStride = 11;

    const Float* s  = srce -  5*srceStride;
    Float*       d  = dest - 10*destStride;

    #define k(x,y)  kernel[(y) * kernelStride + (x)]

    int32_t yd = height + 2*radius;
    do
    {
        memset(d+10*destStride, 0, width*sizeof(Float));
        int32_t xd = width;

#if REUSE_S
        Float s_5 = s[-5];
        Float s_4 = s[-4];
        Float s_3 = s[-3];
        Float s_2 = s[-2];
        Float s_1 = s[-1];
        Float s0  = s[ 0];
        Float s1  = s[ 1];
        Float s2  = s[ 2];
        Float s3  = s[ 3];
        Float s4  = s[ 4];
#endif
        do
        {
            // Unrolled code that exploits all the symmetries in the kernel.
            // Vertical and horizontal symmetries allow us to save a lot of computation thanks to factorization.
            // The diagonal symmetries say that k(x,y) == k(y,x), we cannot exploit that for factorizing,
            // but we can load fewer coefficients by keeping x <= y (21 coefficients instead of 36).

#if !REUSE_S
            const Float s0  = s[ 0];
            const Float s_1 = s[-1];
            const Float s1  = s[ 1];
            const Float s_2 = s[-2];
            const Float s2  = s[ 2];
            const Float s_3 = s[-3];
            const Float s3  = s[ 3];
            const Float s_4 = s[-4];
            const Float s4  = s[ 4];
            const Float s_5 = s[-5];
#endif
            const Float s5  = s[5];

            Float sum0 = s0 * k(0,0);
            Float sum1 = s0 * k(0,1);
            Float sum2 = s0 * k(0,2);
            Float sum3 = s0 * k(0,3);
            Float sum4 = s0 * k(0,4);
            Float sum5 = s0 * k(0,5);

            const Float s11 = s1 + s_1;
            sum0 = MUL_ADD(sum0, s11, k(0,1));
            sum1 = MUL_ADD(sum1, s11, k(1,1));
            sum2 = MUL_ADD(sum2, s11, k(1,2));
            sum3 = MUL_ADD(sum3, s11, k(1,3));
            sum4 = MUL_ADD(sum4, s11, k(1,4));
            sum5 = MUL_ADD(sum5, s11, k(1,5));

            const Float s22 = s2 + s_2;
            sum0 = MUL_ADD(sum0, s22, k(0,2));
            sum1 = MUL_ADD(sum1, s22, k(1,2));
            sum2 = MUL_ADD(sum2, s22, k(2,2));
            sum3 = MUL_ADD(sum3, s22, k(2,3));
            sum4 = MUL_ADD(sum4, s22, k(2,4));
            sum5 = MUL_ADD(sum5, s22, k(2,5));

            const Float s33 = s3 + s_3;
            sum0 = MUL_ADD(sum0, s33, k(0,3));
            sum1 = MUL_ADD(sum1, s33, k(1,3));
            sum2 = MUL_ADD(sum2, s33, k(2,3));
            sum3 = MUL_ADD(sum3, s33, k(3,3));
            sum4 = MUL_ADD(sum4, s33, k(3,4));
            sum5 = MUL_ADD(sum5, s33, k(3,5));

            const Float s44 = s4 + s_4;
            sum0 = MUL_ADD(sum0, s44, k(0,4));
            sum1 = MUL_ADD(sum1, s44, k(1,4));
            sum2 = MUL_ADD(sum2, s44, k(2,4));
            sum3 = MUL_ADD(sum3, s44, k(3,4));
            sum4 = MUL_ADD(sum4, s44, k(4,4));
            sum5 = MUL_ADD(sum5, s44, k(4,5));

            const Float s55 = s5 + s_5;
            sum0 = MUL_ADD(sum0, s55, k(0,5));
            sum1 = MUL_ADD(sum1, s55, k(1,5));
            sum2 = MUL_ADD(sum2, s55, k(2,5));
            sum3 = MUL_ADD(sum3, s55, k(3,5));
            sum4 = MUL_ADD(sum4, s55, k(4,5));
            sum5 = MUL_ADD(sum5, s55, k(5,5));

            Float* pd = d;
            *pd += sum5;  pd+=destStride;
            *pd += sum4;  pd+=destStride;
            *pd += sum3;  pd+=destStride;
            *pd += sum2;  pd+=destStride;
            *pd += sum1;  pd+=destStride;
            *pd += sum0;  pd+=destStride;
            *pd += sum1;  pd+=destStride;
            *pd += sum2;  pd+=destStride;
            *pd += sum3;  pd+=destStride;
            *pd += sum4;  pd+=destStride;
            *pd += sum5;

#if REUSE_S
            s_5 = s_4;
            s_4 = s_3;
            s_3 = s_2;
            s_2 = s_1;
            s_1 = s0; 
            s0  = s1; 
            s1  = s2; 
            s2  = s3; 
            s3  = s4; 
            s4  = s5;
#endif

            ++s;
            ++d;
        }
        while (--xd);

        s += srceStride - width;
        d += destStride - width;
    }
    while (--yd);

    #undef k
#endif
}


/**
 * @brief Retrieves a tile of an image, converting it to `Float` on the fly
 *
 * The tile is retrieved with margins, i.e. image data located around the tile. When the tile
 * is located on a border of the image, data is simply duplicated to fill the tile as needed.
 *
 * @param [out] tile        Pointer to the tile's top-left pixel (i.e. exclusive of margins)
 * @param [in]  tileWidth   The tile's width,  in pixels
 * @param [in]  tileHeight  The tile's height, in pixels
 * @param [in]  tileStride  Distance between a tile row and the row below it, in number of `Float`s
 * @param [in]  margin      The size of the margin around the tile, in pixels
 * @param [in]  x           Horizontal coordinate of the tile's top-left corner within the image
 * @param [in]  y           Vertical   coordinate of the tile's top-left corner within the image
 * @param [in]  imgData     Pointer to the top-left pixel of the image
 * @param [in]  imgWidth    Image's width,  in pixels
 * @param [in]  imgHeight   Image's height, in pixels
 * @param [in]  imgStep     Distance between a pixel of the image and the one immediatelt to its right.
 * @param [in]  imgStride   Distance between a pixel of the image and the one immediately below it.
 */
static void retrieve_tile(Float* tile, uint32_t tileWidth, uint32_t tileHeight, size_t tileStride, uint32_t margin, uint32_t x, uint32_t y,
                          const uint8_t* imgData, uint32_t imgWidth, uint32_t imgHeight, ptrdiff_t imgStep, ptrdiff_t imgStride) RMGR_NOEXCEPT
{
    assert(tileStride >= tileWidth + 2*margin);
    assert(0<=x && x<imgWidth);
    assert(0<=y && y<imgHeight);

    // Point to start of buffer rather than top-left corner
    tile -= margin * tileStride + margin;

    const int32_t dx1 = x - margin;
    const int32_t dy1 = y - margin;
    const int32_t dx2 = x + tileWidth  + margin;
    const int32_t dy2 = y + tileHeight + margin;
    const int32_t sx1 = std::max(dx1, 0);
    const int32_t sy1 = std::max(dy1, 0);
    const int32_t sx2 = std::min(dx2, int32_t(imgWidth));
    const int32_t sy2 = std::min(dy2, int32_t(imgHeight));

    const uint8_t* s       = imgData + sx1 * imgStep + sy1 * imgStride;
    Float*         d       = tile + tileStride * (sy1 - dy1);
    const size_t   sStride = imgStride  - imgStep * (sx2 - sx1);
    const size_t   dStride = tileStride - (tileWidth + 2*margin);
    for (int32_t dy=sy1; dy<sy2; ++dy)
    {
        // Left margin
        for (int32_t dx=dx1; dx<sx1; ++dx)
            *d++ = Float(*s);

        // Actual data
        for (int32_t dx=sx1; dx<sx2; ++dx)
        {
            *d++ = Float(*s);
            s += imgStep;
        }

        // Right margin
        const Float right = d[-1];
        for (int32_t dx=sx2; dx<dx2; ++dx)
            *d++ = right;

        s += sStride;
        d += dStride;
    }

    // Bottom margin
    const size_t tileRowSize = (tileWidth + 2*margin) * sizeof(Float);
    if (dy2 > sy2)
    {
        Float* lastRow = d - tileStride;
        for (int32_t dy=sy2; dy<dy2; ++dy)
        {
            memcpy(d, lastRow, tileRowSize);
            d += tileStride;
        }
    }

    // Top margin
    if (dy1 < sy1)
    {
        Float* firstRow = tile + tileStride * (sy1 - dy1);
        d = tile;
        for (int32_t dy=dy1; dy<sy1; ++dy)
        {
            memcpy(d , firstRow, tileRowSize);
            d += tileStride;
        }
    }
}


double sum_tile(uint32_t tileWidth, uint32_t tileHeight, uint32_t tileStride, double c1, double c2,
                const Float* muATile, const Float* muBTile, const Float* sigmaA2Tile, const Float* sigmaB2Tile, const Float* sigmaABTile,
                float* ssimTile, ptrdiff_t ssimStep, ptrdiff_t ssimStride) RMGR_NOEXCEPT
{
    double tileSum = 0.0f;
    for (uint32_t y=0; y<tileHeight; ++y)
    {
        const Float* muARow     = muATile     + y * tileStride;
        const Float* muBRow     = muBTile     + y * tileStride;
        const Float* sigmaA2Row = sigmaA2Tile + y * tileStride;
        const Float* sigmaB2Row = sigmaB2Tile + y * tileStride;
        const Float* sigmaABRow = sigmaABTile + y * tileStride;
        float*       ssimPtr    = ssimTile    + y * ssimStride;
        for (uint32_t x=0; x<tileWidth; ++x)
        {
            const double muA     = muARow[x];
            const double muB     = muBRow[x];
            const double muA2    = muA * muA;
            const double muB2    = muB * muB;
            const double muAB    = muA * muB;
            const double sigmaA2 = sigmaA2Row[x] - muA2;
            const double sigmaB2 = sigmaB2Row[x] - muB2;
            const double sigmaAB = sigmaABRow[x] - muAB;

            const double numerator1   = 2 * muAB    + c1;
            const double numerator2   = 2 * sigmaAB + c2;
            const double denominator1 = muA2 + muB2 + c1;
            const double denominator2 = sigmaA2 + sigmaB2 + c2;

            const double numerator   = numerator1 * numerator2;
            const double denominator = denominator1 * denominator2;

            const double ssim = numerator / denominator;
            tileSum += ssim;

            if (ssimTile != NULL)
            {
                *ssimPtr = float(ssim);
                ssimPtr += ssimStep;
            }
        }
    }
    return tileSum;
}


//=================================================================================================
// Tile processing function


struct GlobalParams
{
    // User-specified parameters
    uint32_t        width;
    uint32_t        height;
    const uint8_t*  imgAData;
    ptrdiff_t       imgAStep;
    ptrdiff_t       imgAStride;
    const uint8_t*  imgBData;
    ptrdiff_t       imgBStep;
    ptrdiff_t       imgBStride;
    float*          ssimMap;
    ptrdiff_t       ssimStep;
    ptrdiff_t       ssimStride;

    // Other parameters
    uint32_t        tileMaxWidth;
    uint32_t        tileMaxHeight;
    uint32_t        horzMargin;
    uint32_t        vertMargin;
    uint32_t        rowAlignment;
    size_t          bufferCapacity;
    const Float*    gaussianKernel;
    uint32_t        gaussianRadius;
    double          c1;
    double          c2;
    GaussianBlurFct gaussianBlur;
    SumTileFct      sumTile;
};


struct TileParams
{
    Float*   buffers[6];
    uint32_t tileX;
    uint32_t tileY;
};


static double process_tile(const TileParams& tp, const GlobalParams& gp) RMGR_NOEXCEPT
{
    const uint32_t tileWidth  = std::min(gp.tileMaxWidth,  gp.width  - tp.tileX);
    const uint32_t tileHeight = std::min(gp.tileMaxHeight, gp.height - tp.tileY);
    const uint32_t tileStride = ALIGN_UP(tileWidth + 2*gp.horzMargin, gp.rowAlignment);

    // Compute offset between start of buffer and top-left pixel
    const size_t offsetToTopLeft = gp.vertMargin * tileStride + ALIGN_UP(gp.horzMargin, gp.rowAlignment);
    assert(offsetToTopLeft % gp.rowAlignment == 0);
    assert(offsetToTopLeft + (tileStride * (tileHeight + gp.vertMargin)) - gp.horzMargin <= gp.bufferCapacity);

    Float* a = tp.buffers[1] + offsetToTopLeft;
    Float* b = tp.buffers[2] + offsetToTopLeft;
    retrieve_tile(a, tileWidth, tileHeight, tileStride, gp.gaussianRadius, tp.tileX, tp.tileY, gp.imgAData, gp.width, gp.height, gp.imgAStep, gp.imgAStride);
    retrieve_tile(b, tileWidth, tileHeight, tileStride, gp.gaussianRadius, tp.tileX, tp.tileY, gp.imgBData, gp.width, gp.height, gp.imgBStep, gp.imgBStride);

    Float* a2 = tp.buffers[3] + offsetToTopLeft;
    Float* b2 = tp.buffers[4] + offsetToTopLeft;
    Float* ab = tp.buffers[5] + offsetToTopLeft;
    multiply(a2, a, a, tileWidth, tileHeight, tileStride, gp.gaussianRadius);
    multiply(b2, b, b, tileWidth, tileHeight, tileStride, gp.gaussianRadius);
    multiply(ab, a, b, tileWidth, tileHeight, tileStride, gp.gaussianRadius);

    Float* muA     = tp.buffers[0] + offsetToTopLeft;
    Float* muB     = tp.buffers[1] + offsetToTopLeft;
    Float* sigmaA2 = tp.buffers[2] + offsetToTopLeft;
    Float* sigmaB2 = tp.buffers[3] + offsetToTopLeft;
    Float* sigmaAB = tp.buffers[4] + offsetToTopLeft;
    gp.gaussianBlur(muA,     tileStride, a,  tileStride, tileWidth, tileHeight, gp.gaussianKernel, gp.gaussianRadius);
    gp.gaussianBlur(muB,     tileStride, b,  tileStride, tileWidth, tileHeight, gp.gaussianKernel, gp.gaussianRadius);
    gp.gaussianBlur(sigmaA2, tileStride, a2, tileStride, tileWidth, tileHeight, gp.gaussianKernel, gp.gaussianRadius);
    gp.gaussianBlur(sigmaB2, tileStride, b2, tileStride, tileWidth, tileHeight, gp.gaussianKernel, gp.gaussianRadius);
    gp.gaussianBlur(sigmaAB, tileStride, ab, tileStride, tileWidth, tileHeight, gp.gaussianKernel, gp.gaussianRadius);

    float* ssimTile = gp.ssimMap + (tp.tileX * gp.ssimStep) + (tp.tileY * gp.ssimStride);
    return gp.sumTile(tileWidth, tileHeight, tileStride, gp.c1, gp.c2, muA, muB, sigmaA2, sigmaB2, sigmaAB, ssimTile, gp.ssimStep, gp.ssimStride);
};


volatile GaussianBlurFct g_gaussianBlurFct = NULL;
volatile SumTileFct      g_sumTileFct      = NULL;


}} // namespace rmgr::ssim


//=================================================================================================
// x86 init

#if RMGR_ARCH_IS_X86_ANY

#ifdef _MSC_VER
    #include <intrin.h>
    static inline void cpu_id(int leaf, int regs[4]) RMGR_NOEXCEPT
    {
        __cpuid(regs, leaf);
    }
#else
    #include <cpuid.h>
    static inline void cpu_id(int leaf, int regs[4]) RMGR_NOEXCEPT
    {
        __get_cpuid(leaf, reinterpret_cast<unsigned*>(regs), reinterpret_cast<unsigned*>(regs+1), reinterpret_cast<unsigned*>(regs+2), reinterpret_cast<unsigned*>(regs+3));
    }
#endif


static void init_x86() RMGR_NOEXCEPT
{
    using namespace rmgr::ssim;

#if RMGR_SSIM_USE_DOUBLE
    const unsigned sseShift = 26; // SSE2 (required for doubles)
#else
    const unsigned sseShift = 25; // SSE
#endif

    // Detect machine's features
    int regs[4] = {};
    cpu_id(0, regs);
    const uint32_t maxLeaf = regs[0];
    if (maxLeaf >= 1)
    {
        cpu_id(1, regs);
        const uint32_t ecx = regs[2];
        const uint32_t edx = regs[3];

        if (((ecx >> 28) & 1)!=0 && avx::g_gaussianBlurFct!=NULL)
        {
            g_gaussianBlurFct = avx::g_gaussianBlurFct;
            g_sumTileFct      = sum_tile;
        }
        else if (((edx >> sseShift) & 1)!=0 && sse::g_gaussianBlurFct!=NULL)
        {
            g_gaussianBlurFct = sse::g_gaussianBlurFct;
            g_sumTileFct      = sum_tile;
        }
        else
        {
            g_gaussianBlurFct = gaussian_blur;
            g_sumTileFct      = sum_tile;
        }
    }
}
   

#endif // RMGR_ARCH_IS_X86_ANY


//=================================================================================================
// Main function

float rmgr::ssim::compute_ssim(uint32_t width, uint32_t height,
                               const uint8_t* imgAData, ptrdiff_t imgAStep, ptrdiff_t imgAStride,
                               const uint8_t* imgBData, ptrdiff_t imgBStep, ptrdiff_t imgBStride,
                               float* ssimMap, ptrdiff_t ssimStep, ptrdiff_t ssimStride, unsigned flags) RMGR_NOEXCEPT
{
    GaussianBlurFct gaussianBlur = g_gaussianBlurFct;
    SumTileFct      sumTile      = g_sumTileFct;
    if (gaussianBlur==NULL || sumTile==NULL)
    {
#if RMGR_ARCH_IS_X86_ANY
        init_x86();
#else
        g_gaussianBlurFct = gaussian_blur;
        g_sumTileFct      = sum_tile;
#endif
        gaussianBlur = g_gaussianBlurFct;
        sumTile      = g_sumTileFct;
    }

    const double k1 = 0.01;
    const double k2 = 0.03;
    const double L  = UINT8_MAX;
    const double c1 = (k1 * L) * (k1 * L);
    const double c2 = (k2 * L) * (k2 * L);

    if (imgAData==NULL || imgBData==NULL)
    {
        RMGR_SSIM_REPORT_ERROR("Invalid parameter: imgAData or imgBData is NULL\n");
        return -EINVAL;
    }

    if (ssimMap == NULL)
    {
        ssimStep   = 0;
        ssimStride = 0;
    }

    // Parameters of the gaussian blur
    const unsigned gaussianRadius = 5;
    const Float    gaussianSigma  = 1.5;
    Float kernelBuffer[(2*gaussianRadius+1) * (2*gaussianRadius+1)];
    precompute_gaussian_kernel(kernelBuffer, gaussianRadius, gaussianSigma);

    const Float* gaussianKernel = kernelBuffer + gaussianRadius * (2*gaussianRadius+1) + gaussianRadius; // Center of the kernel

    const uint32_t tileMaxWidth    = 256 / (sizeof(Float) / sizeof(float));
    const uint32_t tileMaxHeight   =  64;
    const uint32_t horzMargin      = gaussianRadius;
    const uint32_t vertMargin      = 2 * gaussianRadius; // The blur routines require an extra write margin vertically (but the read margin is equal to the radius)
    const size_t   bufferAlignment = RMGR_SSIM_TILE_ALIGNMENT / sizeof(Float);
    const size_t   rowAlignment    = bufferAlignment;
    const uint32_t bufferWidth     = tileMaxWidth  + 2*horzMargin;
    const uint32_t bufferHeight    = tileMaxHeight + 2*vertMargin;
    const size_t   bufferStride    = ALIGN_UP(bufferWidth, rowAlignment);
    const size_t   bufferCapacity  = ALIGN_UP(bufferStride*bufferHeight + rowAlignment-1, bufferAlignment);

    // Prepare global parameters
    const GlobalParams gp =
    {
        // User-specified parameters
        width,
        height,
        imgAData,
        imgAStep,
        imgAStride,
        imgBData,
        imgBStep,
        imgBStride,
        ssimMap,
        ssimStep,
        ssimStride,

        // Other parameters
        tileMaxWidth,
        tileMaxHeight,
        horzMargin,
        vertMargin,
        rowAlignment,
        bufferCapacity,
        gaussianKernel,
        gaussianRadius,
        c1,
        c2,
        gaussianBlur,
        sumTile
    };

    double sum = 0.0;
    if (flags & FLAG_HEAP_BUFFERS)
    {
        Float* heapBuffer = static_cast<Float*>(alloc(6 * bufferCapacity * sizeof(Float), bufferAlignment));
        if (heapBuffer == NULL)
            return float(-ENOMEM);
        for (uint32_t tileY=0; tileY<height; tileY+=tileMaxHeight)
        {
            for (uint32_t tileX=0; tileX<width; tileX+=tileMaxWidth)
            {
                const TileParams tp =
                {
                    {
                        heapBuffer,
                        heapBuffer + bufferCapacity,
                        heapBuffer + bufferCapacity * 2,
                        heapBuffer + bufferCapacity * 3,
                        heapBuffer + bufferCapacity * 4,
                        heapBuffer + bufferCapacity * 5,
                    },
                    tileX,
                    tileY
                };
                sum += process_tile(tp, gp);
            }
        }
        dealloc(heapBuffer);
    }
    else
    {
        RMGR_ALIGNED_VAR(RMGR_SSIM_TILE_ALIGNMENT, Float, buffers[6][bufferCapacity]);
        for (uint32_t tileY=0; tileY<height; tileY+=tileMaxHeight)
        {
            for (uint32_t tileX=0; tileX<width; tileX+=tileMaxWidth)
            {
                const TileParams tp = {{buffers[0],buffers[1],buffers[2],buffers[3],buffers[4],buffers[5]}, tileX, tileY};
                sum += process_tile(tp, gp);
            }
        }
    }

    return float(sum / double(width * height));
}
