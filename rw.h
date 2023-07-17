#ifndef _RW_H
#define _RW_H


#include <linux/blkdev.h>
#include <linux/kthread.h>
#include <linux/raid/pq.h>
#include <linux/async_tx.h>
#include <linux/module.h>
#include <linux/async.h>
#include <linux/seq_file.h>
#include <linux/cpu.h>
#include <linux/slab.h>
#include <linux/ratelimit.h>
#include <linux/nodemask.h>

#include <trace/events/block.h>
#include <linux/list_sort.h>

#include<linux/bio.h>
#include<linux/blk_types.h>
 #include <linux/printk.h>
#include "md.h"
#include "raid5.h"
#include "raid0.h"
#include "md-bitmap.h"
#include "raid5-log.h"
#include "raid1.h"

#define  Elemtype long
#define UserLogicalSpace 4096      //以sector为单位，可调
#define FusionLogicalSpace 1024
#define num_block 4

#define RW_SECTORS 0x80000
#define RW_RAID  0x800000000000


struct  Laddr2Paddr{
    long order;
};

//这个表从用户逻辑空间映射到fusion逻辑空间
struct U2FTransTable{
    struct Laddr2Paddr *ftbale;//[UserLogicalSpace];
    long order;
};

struct phyaddr{
    int d_idx;
    sector_t off;
    int size;
};

//每5个block组成一个大块
struct phyblock{
    struct phyaddr *block;//[num_block];小的block，记录下bio盘号，偏移等
    spinlock_t      phy_lock;
};

//需要一些元数据知道那些块分配了，哪些没有
struct rwconf{
   // HashTable       rw_hash; //复制写 通过低块号索引
    struct U2FTransTable *first_table; //第一层bio下来给予order然后根据地址保存order
    struct phyblock *blocks;//[FusionLogicalSpace]; 相当于一个条带，条带中有许多小block
    struct mddev		*mddev;
    int rw_disks;

    long num_rw;
    spinlock_t count_lock;
    long num_trans;
    spinlock_t trans_lock;

    int chunk_sectors;/*一个chunk中sector的数目，    默认值为8，即一个chunk的大小为512KB*/

    struct disk_info    *disks;
};


extern void Do_RW(struct bio *bi, struct rwconf *conf);
sector_t rw_compute_sector(struct rwconf *conf, long off_big_block, int off_in_block, int *disk_idx);
void Change_Bio_Read(struct bio *bi, struct rwconf *conf);
struct rwconf * Init_RWConf(struct r5conf *r5conf);
#endif