#include <linux/module.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/err.h>
#include <linux/param.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/f2fs_fs.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/crc32.h>

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

static u32 ms_digest_exact(const void *buf, size_t len)
{
    /* Fast, hardware-accelerated on many platforms */
    return crc32_le(0, buf, len);
}

void memshare_set_bitmap(struct f2fs_sb_info *sbi, int type, struct curseg_info *curseg, unsigned int segno)
{
    u32 s, start_seq;
    int start_segno = GET_SEGNO(sbi, FDEV(1).start_blk);
    unsigned int secno, first_seg, next_sec_seg;
    unsigned int segs_per_sec, i;
    size_t per_seg_bytes = SIT_VBLOCK_MAP_SIZE; /* ckpt_valid_map size per segment (bytes) */
    size_t total_bytes;
    unsigned int max_segs; /* max segments that fit into payload */
    unsigned int nr_segs_used; /* segments we actually copy */
    size_t bytes_used; /* total bytes written into shared buffer */

    /* digest for host-side verification */
    u32 h = 2166136261u; /* FNV-1a 32-bit offset basis */

    int total = 0;

    for (i = 0; i < 8192; i++) {
        if (f2fs_test_bit(i, curseg->cursec->valid_map)) 
            total++;
    }


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
    /* 안전 가드: 해당 segno가 dev1 범위에 속하지 않으면 스킵 */
    {
        block_t fs_blk = START_BLOCK(sbi, segno);
        if (fs_blk < FDEV(1).start_blk ||
            (FDEV(1).end_blk && fs_blk >= FDEV(1).end_blk)) {
            /* dev1 영역이 아니면 공유메모리 업데이트 불필요 */
            return;
        }
    }

    /* dev1 로컬 섹션 번호 = (글로벌 섹션) - (dev1 시작 글로벌 섹션) */
    do {
        unsigned int global_sec = GET_SEC_FROM_SEG(sbi, segno);
        unsigned int dev1_base_sec = GET_SEC_FROM_SEG(sbi, start_segno);
        if (global_sec < dev1_base_sec)
            return; /* 방어: 이론상 위의 블럭 범위 가드로 오지 않음 */
        secno = global_sec - dev1_base_sec;
    } while (0);
    segs_per_sec = SEGS_PER_SEC(sbi);
    first_seg = GET_SEG_FROM_SEC(sbi, secno);
    next_sec_seg = GET_SEG_FROM_SEC(sbi, secno + 1);


    // printk("msb segno %u (%u).  secno %u\n",
	// 		segno-start_segno, GET_SEC_FROM_SEG(sbi, segno-start_segno),
    //         secno);


    /* Bound check to avoid overrunning shared payload */
    total_bytes = (size_t)segs_per_sec * per_seg_bytes;
    max_segs = F2FS_SSR_PAYLOAD / per_seg_bytes;
    if (unlikely(total_bytes > F2FS_SSR_PAYLOAD)) {
        /* Warn once and clamp copy to max that fits */
        WARN_ON_ONCE(1);
        next_sec_seg = first_seg + max_segs;
    }
    nr_segs_used = next_sec_seg - first_seg;
    bytes_used = (size_t)nr_segs_used * per_seg_bytes;

    /* 1) '쓰기 중(홀수)'로 전이 */
    s = READ_ONCE(ps->seq[type]);
    start_seq = (s & ~1u) + 1u;            /* 짝수→홀수 */
    WRITE_ONCE(ps->seq[type], start_seq);
    smp_wmb();  

    /* 공유 버퍼 전체를 채움: valid_map 크기와 동일(1KB) */
    memcpy(ps->data[type], curseg->cursec->valid_map, F2FS_SSR_PAYLOAD);

//     /* 2) 섹션 내 각 세그먼트의 (ckpt_valid_map OR cur_valid_map)만 수집 */
//     for (i = first_seg; i < next_sec_seg; i++) {
//         struct seg_entry *se = get_seg_entry(sbi, i + start_segno);
//         const unsigned long *ck = (const unsigned long *)se->ckpt_valid_map;
//         const unsigned long *cu = (const unsigned long *)se->cur_valid_map;
//         unsigned long *out = (unsigned long *)&ps->data[type][(size_t)(i - first_seg) * per_seg_bytes];
//         size_t nlong = per_seg_bytes / sizeof(unsigned long);
//         size_t rbytes = per_seg_bytes % sizeof(unsigned long);
//         size_t w;
//         /* bit-position modulo-4 masks for fast popcount */
// #if BITS_PER_LONG == 64
//         const unsigned long m0 = 0x1111111111111111ULL;
//         const unsigned long m1 = 0x2222222222222222ULL;
//         const unsigned long m2 = 0x4444444444444444ULL;
//         const unsigned long m3 = 0x8888888888888888ULL;
// #else /* 32-bit */
//         const unsigned long m0 = 0x11111111UL;
//         const unsigned long m1 = 0x22222222UL;
//         const unsigned long m2 = 0x44444444UL;
//         const unsigned long m3 = 0x88888888UL;
// #endif

//         for (w = 0; w < nlong; w++) {
//             unsigned long v = ck[w] | cu[w];
//             out[w] = v;
//             /* part_total[i] update: count bits where (bit_index % 4) == i */
//             part_total[0] += hweight_long(v & m0);
//             part_total[1] += hweight_long(v & m1);
//             part_total[2] += hweight_long(v & m2);
//             part_total[3] += hweight_long(v & m3);
//         }

//         if (rbytes) {
//             unsigned long v = 0, ckv = 0, cuv = 0;
//             /* Load tail bytes safely */
//             memcpy(&ckv, (const u8 *)se->ckpt_valid_map + (nlong * sizeof(unsigned long)), rbytes);
//             memcpy(&cuv, (const u8 *)se->cur_valid_map + (nlong * sizeof(unsigned long)), rbytes);
//             v = ckv | cuv;
//             memcpy((u8 *)out + (nlong * sizeof(unsigned long)), &v, rbytes);
//             /* Count only valid tail bits */
//             {
//                 unsigned long maskbits = (1UL << (rbytes * 8)) - 1UL;
//                 unsigned long rv = v & maskbits;
//                 part_total[0] += hweight_long(rv & m0);
//                 part_total[1] += hweight_long(rv & m1);
//                 part_total[2] += hweight_long(rv & m2);
//                 part_total[3] += hweight_long(rv & m3);
//             }
//         }
//     }

    /* Compute digest over the contiguous payload we just populated */
    h = ms_digest_exact(&ps->data[type][0], bytes_used);

    /*
     * 섹션 식별자 저장 (host order -> MMIO)
     * - 멀티 디바이스 고려: 2번째 디바이스(index 1)의 디바이스-로컬 sec로 기록
     * - 섹션 데이터 적재 및 복사는 글로벌 sec 기준을 그대로 사용
     */

    WRITE_ONCE(ps->block_id[type], secno);

    // Host digest print for comparison with device side */
    // printk("memshare_set_bitmap: digest=%u, bits=%zu,  segno=%u. start_segno=%u  global_sec=%u, dev1_local_sec=%u, type=%d\n",
    //        h,
    //        bytes_used * 8,
    //        segno, start_segno,
    //        GET_SEC_FROM_SEG(sbi, segno), 
    //        secno,
    //        type);

    
    smp_wmb();
    WRITE_ONCE(ps->seq[type], start_seq + 1u);

    // printk("     valid moved 0 = %d\n", total);
}
