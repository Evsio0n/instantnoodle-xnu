/* One 4 KiB FastRPC argument page and three 4 KiB tables for a 39-bit VA,
 * 36-bit PA, stage-one translation context. Layout matches the apps SMMU.
 * Table walks use the untagged IOVA; the FastRPC CB tag is only on the wire. */
#ifndef SLPI_DMA_ARENA_H
#define SLPI_DMA_ARENA_H
#include "hk_fastrpc_wire.h"

#define SLPI_DMA_GRANULE 4096u
#define SLPI_DMA_TABLE_COUNT 3u
#define SLPI_DMA_BYTES (4u * SLPI_DMA_GRANULE)
#define SLPI_DMA_PA_MASK UINT64_C(0xffffff000)
/* MAIR index 1 (normal WB), RW, inner shareable, AF, nG, UXN and PXN. */
#define SLPI_DMA_LEAF (UINT64_C(3) | (1u<<2) | (1u<<6) | (3u<<8) | (1u<<10) | (1u<<11) | (UINT64_C(3)<<53))

struct slpi_dma_tables {
    uint64_t entries[SLPI_DMA_TABLE_COUNT][512];
};
_Static_assert(sizeof(struct slpi_dma_tables)==3 * SLPI_DMA_GRANULE, "one granule per table");

static inline unsigned
slpi_dma_index(uint64_t iova, unsigned level)
{
    return (iova >> (30 - 9 * level)) & 511;
}

/* Two active calls may share a context, never the same argument page.
 * Both mappings must fit in this leaf table and the captured IOVA pool. */
static inline bool
slpi_dma_shape(uint64_t iova, unsigned count)
{
    return count >= 1 && count <= 2 && !(iova & 4095) &&
        hk_fastrpc_iova_valid(iova) &&
        HK_FASTRPC_IOVA_END - iova >= count * SLPI_DMA_GRANULE &&
        slpi_dma_index(iova, 2) + count <= 512;
}
static inline bool
slpi_dma_prepare_pages(struct slpi_dma_tables *tables, const uint64_t *pa, uint64_t iova, unsigned count)
{
    if (!slpi_dma_shape(iova,count)) return false;
    for (unsigned i=0; i<3+count; i++) {
        if (!pa[i] || (pa[i] & ~SLPI_DMA_PA_MASK)) return false;
        for (unsigned j=0;j<i;j++) if(pa[j]==pa[i]) return false;
    }
    memset(tables,0,sizeof(*tables));
    for (unsigned i=0;i<2;i++) tables->entries[i][slpi_dma_index(iova,i)]=pa[i+1]|3;
    for (unsigned i=0;i<count;i++)
        tables->entries[2][slpi_dma_index(iova,2)+i]=pa[3+i]|SLPI_DMA_LEAF;
    return true;
}
static inline bool
slpi_dma_verify_pages(const struct slpi_dma_tables *tables,const uint64_t *pa,uint64_t iova,unsigned count)
{
    if (!slpi_dma_shape(iova,count)) return false;
    for (unsigned level=0;level<3;level++) {
        unsigned first=slpi_dma_index(iova,level);
        for(unsigned j=0;j<512;j++) {
            uint64_t expected=0;
            if(level<2 && j==first) expected=pa[level+1]|3;
            if(level==2 && j>=first && j-first<count) expected=pa[3+j-first]|SLPI_DMA_LEAF;
            if(tables->entries[level][j]!=expected)return false;
        }
    }
    return true;
}
static inline bool slpi_dma_prepare(struct slpi_dma_tables *t,const uint64_t p[4],uint64_t i)
{return slpi_dma_prepare_pages(t,p,i,1);}
static inline bool slpi_dma_verify(const struct slpi_dma_tables *t,const uint64_t p[4],uint64_t i)
{return slpi_dma_verify_pages(t,p,i,1);}
#endif
