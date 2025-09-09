#include <linux/module.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/err.h>
#include <linux/param.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/f2fs_fs.h>
#include <linux/bitmap.h>

#include "mem_share.h"
#include "f2fs.h" 
#include "segment.h"

static phys_addr_t phys_start = 0x280000000ULL;  /* memmap과 동일 */
static unsigned long region_len = PAGE_SIZE * 2;     /* 4K 이상이면 OK (6K면 8K 권장) */

static DEFINE_MUTEX(ps_lock);
static atomic_t ps_users = ATOMIC_INIT(0);

static void *kva;
static struct f2fs_ssr_direct *ps;

int f2fs_memshare_get(void)
{
    int i, ret = 0;

    /* Fast-path: if already initialized, just bump users and return */
    if (atomic_inc_return(&ps_users) > 1)
        return 0;

    /* First user: perform mapping + init under lock */
    mutex_lock(&ps_lock);

    if (!kva) {
        kva = memremap(phys_start, region_len, MEMREMAP_WB);
        if (!kva) {
            ret = -ENOMEM;
            goto err_dec;
        }
        ps = (struct f2fs_ssr_direct*)kva;

        /* Initialize all seq entries to even(0 = stable) */
        for (i = 0; i < SHARING_LOGS; i++)
            iowrite32(0, &ps->seq[i]);
    }

    mutex_unlock(&ps_lock);
    return 0;

err_dec:
    atomic_dec(&ps_users);
    mutex_unlock(&ps_lock);
    return ret;
}

void f2fs_memshare_put(void)
{
    if (atomic_dec_and_test(&ps_users)) {
        mutex_lock(&ps_lock);
        if (kva) {
            memunmap(kva);
            kva = NULL;
            ps = NULL;
        }
        mutex_unlock(&ps_lock);
    }
}


void memshare_set_bitmap_test(struct f2fs_sb_info *sbi, int type, unsigned int segno)
{
    u32 s, start_seq;
    unsigned int secno;
    unsigned int segs_per_sec;
    size_t total_bytes;
    u64 secno64;
    /* 고정 패턴(LCG)으로 테스트 데이터 생성 - 직접 MMIO에 기록하여 스택 사용 최소화 */
    u32 x;
    /* digest for host-side verification (fast fold, similar to set_bitmap) */
    u32 h = 2166136261u; /* FNV-1a 32-bit offset basis */
    size_t i, nwords, rem;
    void *dst;

    if (WARN_ON(!ps))
        return;

    if (type < 0 || type >= SHARING_LOGS)
        return;

    secno = GET_SEC_FROM_SEG(sbi, segno);
    segs_per_sec = SEGS_PER_SEC(sbi);
    total_bytes = (size_t)segs_per_sec * SIT_VBLOCK_MAP_SIZE;
    if (total_bytes > F2FS_SSR_PAYLOAD)
        total_bytes = F2FS_SSR_PAYLOAD;

    /* 1) 쓰기중(홀수) 시퀀스로 전이 */
    s = ioread32(&ps->seq[type]);
    start_seq = (s & ~1u) + 1u;
    iowrite32(start_seq, &ps->seq[type]);

    /* 2) 테스트 패턴 채우기: 간단한 LCG 기반 의사난수 */
    x = (secno << 16) ^ (type << 8) ^ 0x5Au;
    dst = (void __iomem *)ps->data[type];
    nwords = total_bytes >> 2; /* 4바이트 단위 */
    rem = total_bytes & 3;

    for (i = 0; i < nwords; i++) {
        x = x * 1664525u + 1013904223u; /* LCG */
        iowrite32(x, dst + (i << 2));
        h ^= x;
        h *= 16777619u;
    }
    if (rem) {
        u32 last;
        x = x * 1664525u + 1013904223u;
        last = x;
        memcpy_toio(dst + (nwords << 2), &last, rem);
        /* fold only the written bytes */
        {
            u32 mask = (rem == 4) ? 0xffffffffu : ((1u << (rem * 8)) - 1u);
            u32 v = last & mask;
            h ^= v;
            h *= 16777619u;
        }
    }

    /* 섹션 식별자 저장: dev1(두번째 디바이스) 로컬 sec로 기록 */
    {
        unsigned int stored_secno = secno; /* 기본: 글로벌 sec */
        if (sbi->s_ndevs > 1) {
            unsigned int dev0_total = FDEV(0).total_segments;
            unsigned int dev1_total = (sbi->s_ndevs > 1) ? FDEV(1).total_segments : 0;
            if (segno >= dev0_total && segno < dev0_total + dev1_total)
                stored_secno = (segno - dev0_total) / segs_per_sec;
        }
        secno64 = (u64)stored_secno;
    }
    memcpy_toio(&ps->block_id[type], &secno64, sizeof(secno64));

    /* Host digest print for comparison with device side */
    printk("memshare_set_bitmap_test: digest=%u, bits=%zu, global_sec=%u, dev1_local_sec=%llu, type=%d\n",
           h, total_bytes * 8, secno, secno64, type);

    /* 3) 배리어 후 안정(짝수) 시퀀스로 전이 */
    smp_wmb();
    iowrite32(start_seq + 1u, &ps->seq[type]);

}


void memshare_set_bitmap(struct f2fs_sb_info *sbi, int type, unsigned int segno)
{
    u32 s, start_seq;
    unsigned int secno, first_seg, next_sec_seg;
    unsigned int segs_per_sec, i;
    size_t per_seg_bytes = SIT_VBLOCK_MAP_SIZE; /* ckpt_valid_map size per segment (bytes) */
    size_t total_bytes;
    unsigned int max_segs; /* max segments that fit into payload */
    u64 secno64;
    /* digest for host-side verification */
    u32 h = 2166136261u; /* FNV-1a 32-bit offset basis */

    if (WARN_ON(!ps)) {
        printk("ps is null\n");
        return;
    }

    /* type 범위 방어: 0 <= type < SHARING_LOGS */
    if (type < 0 || type >= SHARING_LOGS) {
        printk("type is invalid\n");
        return;
    }

    /* section 계산 (글로벌 sec) */
    secno = GET_SEC_FROM_SEG(sbi, segno);
    segs_per_sec = SEGS_PER_SEC(sbi);
    first_seg = GET_SEG_FROM_SEC(sbi, secno);
    next_sec_seg = GET_SEG_FROM_SEC(sbi, secno + 1);

    /* Bound check to avoid overrunning shared payload */
    total_bytes = (size_t)segs_per_sec * per_seg_bytes;
    max_segs = F2FS_SSR_PAYLOAD / per_seg_bytes;
    if (unlikely(total_bytes > F2FS_SSR_PAYLOAD)) {
        /* Warn once and clamp copy to max that fits */
        WARN_ON_ONCE(1);
        next_sec_seg = first_seg + max_segs;
    }

    /* 1) '쓰기 중(홀수)'로 전이 */
    s = ioread32(&ps->seq[type]);
    start_seq = (s & ~1u) + 1u;
    iowrite32(start_seq, &ps->seq[type]);

    s = READ_ONCE(ps->seq[type]);
    start_seq = (s & ~1u) + 1u;            /* 짝수→홀수 */
    WRITE_ONCE(ps->seq[type], start_seq);
    smp_wmb();  

    /* 2) 섹션 내 각 세그먼트의 ckpt_valid_map을 1KB 버퍼에 순서대로 적재 */
    for (i = first_seg; i < next_sec_seg; i++) {
        struct seg_entry *se = get_seg_entry(sbi, i);
        const unsigned char *ckpt = se->ckpt_valid_map; /* size = SIT_VBLOCK_MAP_SIZE bytes */
        size_t off = (size_t)(i - first_seg) * per_seg_bytes;

        /* memcpy_toio expects __iomem pointer; take element address */
        memcpy_toio((void __iomem *)&ps->data[type][off], ckpt, per_seg_bytes);

        /* Accumulate digest over this segment's bitmap (as bits) */
        {
            const unsigned long *bp = (const unsigned long *)ckpt;
            const size_t nr_bits = per_seg_bytes * 8;
            const size_t nfull = nr_bits / BITS_PER_LONG;
            const size_t rem   = nr_bits % BITS_PER_LONG;
            size_t w;

            for (w = 0; w < nfull; w++) {
                unsigned long v = bp[w];
                h ^= (u32)(v & 0xffffffffu);
                h *= 16777619u;
#if BITS_PER_LONG > 32
                h ^= (u32)((v >> 32) & 0xffffffffu);
                h *= 16777619u;
#endif
            }

            if (rem) {
                unsigned long mask;
                if (rem == BITS_PER_LONG)
                    mask = ~0UL;
                else
                    mask = (1UL << rem) - 1UL;

                {
                    unsigned long v = bp[nfull] & mask;
                    h ^= (u32)(v & 0xffffffffu);
                    h *= 16777619u;
#if BITS_PER_LONG > 32
                    h ^= (u32)((v >> 32) & 0xffffffffu);
                    h *= 16777619u;
#endif
                }
            }
        }
    }

    /*
     * 섹션 식별자 저장 (host order -> MMIO)
     * - 멀티 디바이스 고려: 2번째 디바이스(index 1)의 디바이스-로컬 sec로 기록
     * - 섹션 데이터 적재 및 복사는 글로벌 sec 기준을 그대로 사용
     */
    {
        unsigned int stored_secno = secno; /* 기본: 글로벌 sec */
        if (sbi->s_ndevs > 1) {
            int start_segno = GET_SEGNO(sbi, FDEV(1).start_blk);

            secno64 = secno - start_segno;
        }
    }
    memcpy_toio((void __iomem *)&ps->block_id[type], &secno64, sizeof(secno64));

    // Host digest print for comparison with device side */
    printk("memshare_set_bitmap: digest=%u, bits=%zu, global_sec=%u, dev1_local_sec=%llu, type=%d\n",
           h,
           (size_t)(min(segs_per_sec, max_segs)) * per_seg_bytes * 8,
           secno, secno64, type);

    
    smp_wmb();
    iowrite32(start_seq + 1u, &ps->seq[type]);
}
