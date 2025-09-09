#ifndef __MEM_SHARE_H_
#define __MEM_SHARE_H_

#include <linux/types.h>

#define F2FS_SSR_PAYLOAD 1024  /* 1KB: SEGS_PER_SEC * SIT_VBLOCK_MAP_SIZE (typ. 16 * 64B) */
#define F2FS_SSR_PAYLOAD_LONGS (F2FS_SSR_PAYLOAD / sizeof(__le32)) 

#define SHARING_LOGS 6

struct f2fs_ssr_direct {
	u32 seq[SHARING_LOGS];       /* 홀수=쓰기중, 짝수=안정 */
    u64 block_id[SHARING_LOGS];
	u8  data[SHARING_LOGS][F2FS_SSR_PAYLOAD];
};

struct f2fs_sb_info;
struct curseg_info;

int f2fs_memshare_get(void);
void f2fs_memshare_put(void);
void memshare_set_bitmap(struct f2fs_sb_info *sbi, int type, struct curseg_info *curseg, unsigned int segno);
void memshare_set_bitmap_test(struct f2fs_sb_info *sbi, int type, unsigned int segno);

#endif
