
#define DEBUG
#include "rw.h"
/*
这个函数用来初始化conf
复制r5conf中有用的信息，同时初始化一些额外需要的数据结构
*/
struct rwconf * Init_RWConf(struct r5conf *r5conf)
{
    printk("-----------------enter into init function---------------\n");
    struct rwconf *rwconf;
    rwconf = kzalloc(sizeof(struct rwconf), GFP_KERNEL);
    rwconf->first_table = kzalloc(sizeof(struct U2FTransTable),GFP_KERNEL);
    rwconf->first_table->ftbale = kcalloc(UserLogicalSpace,sizeof(struct Laddr2Paddr), GFP_KERNEL);
    rwconf->blocks = kcalloc(FusionLogicalSpace, sizeof(struct phyblock), GFP_KERNEL);

    int i = 0;
    for(; i < FusionLogicalSpace; i++)
    {
        rwconf->blocks[i].block = kcalloc(num_block, sizeof(struct phyaddr), GFP_KERNEL);
        spin_lock_init(&rwconf->blocks[i].phy_lock);
    }
    printk("----------------------Finish Init block and some lock-----------------------\n");
    rwconf->chunk_sectors = 8;
    rwconf->mddev = r5conf->mddev;
    rwconf->rw_disks = r5conf->raid_disks;
    rwconf->num_rw = 0;
    spin_lock_init(&rwconf->count_lock);

    rwconf->num_trans = 0;
    spin_lock_init(&rwconf->trans_lock);

    rwconf->disks = r5conf->disks;
    rwconf->first_table->order = 0;
    
    if(!rwconf->disks || !rwconf->mddev)
        printk("------init rwconf error------\n");
    return rwconf;
};

/**
 * 单线程版本
 * bio来了给一个order实现顺序写
 * 
*/


long Translatel2f(struct bio *bio,struct rwconf *conf)
{
    //sector_t bio_sector = bio->bi_iter.bi_sector;
    //conf->first_table.ftbale[bio_sector].order = conf->first_table.order;
    long order = conf->first_table->order;
    conf->first_table->order++;


    return order;
}
//根据order计算在第二层区域的大块和小块
/**
 * 有了order，可以算出在哪过大块(条带)中的哪个block
 * 
*/
int compute_block(long order,struct rwconf *conf, long *nblock)
{
    if(order >= FusionLogicalSpace * num_block)
        order = 0;

    if(order%num_block == 0)
        order += num_block;

    *nblock = order / num_block;
    
    return order % num_block;
}
/**
 * 这里的数据结构是大块含有若干小块，这个转换时只需要处理大块
*/

sector_t rw_compute_sector(struct rwconf *conf, long off_big_block, int off_in_block, int *disk_idx)
{
    int data_disks =  conf->rw_disks;
    int sectors_per_chunk = conf->chunk_sectors;
    sector_t chunk_number;
    unsigned int chunk_offset;
    long order = off_big_block * num_block + off_in_block;
    chunk_offset = order % sectors_per_chunk;
    chunk_number = order / sectors_per_chunk;
    *disk_idx = order % data_disks;
    return chunk_number * sectors_per_chunk + chunk_offset;
}
/*
保存信息，在读的时候需要
*/
void Translatef2p(long order, struct bio *bi, struct rwconf *conf)
{
    int off_in_block = 0;
    long off_big_block = 0;
    int first_dd_idx = 0;
    sector_t first_off_disk = 0;
    sector_t off = bi->bi_iter.bi_sector;
    //第一个写
    off_in_block = compute_block(order, conf, &off_big_block);
    first_off_disk = rw_compute_sector(conf,off_big_block,off_in_block, &first_dd_idx);
    int size = bi->bi_io_vec[0].bv_len;
    //加锁
    spinlock_t phy_lock = conf->blocks[off_big_block].phy_lock;
    spin_lock(&phy_lock);
   conf->blocks[off_big_block].block[off_in_block].d_idx = first_dd_idx;
    conf->blocks[off_big_block].block[off_in_block].off = first_off_disk;
    conf->blocks[off_big_block].block[off_in_block].size = size;
    spin_unlock(&phy_lock);
    
    
    int second_dd_idx = 0;
    sector_t second_off_disk = 0;
    second_off_disk = rw_compute_sector(conf, off_big_block+1, off_in_block, &second_dd_idx);
    //复制写区域
    spinlock_t second_lock = conf->blocks[off_big_block + 1].phy_lock;
    spin_lock(&second_lock);
    conf->blocks[off_big_block+1].block[off_in_block].d_idx = second_dd_idx;
    conf->blocks[off_big_block+1].block[off_in_block].off = second_off_disk;
    conf->blocks[off_big_block+1].block[off_in_block].size = size;
    spin_unlock(&second_lock);

    spinlock_t count_lock = conf->count_lock;
    spin_lock(&count_lock);
    conf->num_rw += 2;
    spin_unlock(&count_lock);
    printk("-------after 2 layer mapping----first--%d--%d--",first_dd_idx,first_off_disk);
    printk("-------after 2 layer mapping----first--%d--%d--",second_dd_idx,second_off_disk);
}

/**
 * 读取bio在第二层记录的偏移和盘号
 * 
*/
sector_t ReadPhytable(long order, struct rwconf *conf,struct bio *bio, int *first_d_idx, int *second_d_idx, sector_t *second_off)
{
    int off_in_block = 0;
    long off_big_block = 0;

    off_in_block = compute_block(order,conf, &off_big_block);
    *first_d_idx = conf->blocks[off_big_block].block[off_in_block].d_idx;
    *second_d_idx = conf->blocks[off_big_block+1].block[off_in_block].d_idx;
    *second_off = conf->blocks[off_big_block + 1].block[off_in_block].off;
    return conf->blocks[off_big_block].block[off_in_block].off;
}

void copy_frombio(struct page *page, struct bio *bi, int len)
{
    enum async_tx_flags flags = 0;
    struct async_submit_ctl submit;
    struct dma_async_tx_descriptor *tx = NULL;

    struct bio_vec bvl;
    struct bvec_iter iter;

    flags |= ASYNC_TX_FENCE;
	init_async_submit(&submit, flags, tx, NULL, NULL, NULL);

    bio_for_each_segment(bvl, bi, iter) {
        //int len = bvl.bv_len;
        struct page *bio_page = bvl.bv_page;
            tx = async_memcpy(page, bio_page, 0,
						  0, len, &submit);
    };
}

static void rw_end_write_request(struct bio *bio)
{
	struct r5conf *conf = bio->bi_private;
    printk("-----------enter into rw_end_write_request-------------\n");
    BUG();
}



void init_rwbio(struct r1bio *r1_bio, struct mddev *mddev, struct bio *bio)
{
	r1_bio->master_bio = bio;
	r1_bio->sectors = bio_sectors(bio);
	r1_bio->state = 0;
	r1_bio->mddev = mddev;
	r1_bio->sector = bio->bi_iter.bi_sector;
}

struct r1bio *
alloc_rwbio(struct mddev *mddev, struct bio *bio)
{
	struct r1conf *conf = mddev->private;
	struct r1bio *r1_bio;

	r1_bio = kzalloc(sizeof(struct r1bio), GFP_NOIO);
	/* Ensure no bio records IO_BLOCKED */
	memset(r1_bio->bios, 0, sizeof(r1_bio->bios[0]));
	init_rwbio(r1_bio, mddev, bio);
	return r1_bio;
}


/**
 * 复制写函数，思路是复制bio，修改mbio的参数进行提交
*/

void do_replicated_write(struct rwconf *conf,struct bio *bio, int dd_idx, sector_t off)
{
    struct page * place;
    struct page *page, *page1, *page2;
    struct buffer_head *bh;
    struct md_rdev *rdev = NULL;
    unsigned long block_len = 64 * 1024;//表示64K
    struct bio_list pending_bios = BIO_EMPTY_LIST;

    struct mddev *mddev = conf->mddev;

    struct r1bio *r1_bio;
    r1_bio = alloc_rwbio(mddev, bio);
    if(!r1_bio)
    printk("---------------alloc rw bio error-------------------\n");
    r1_bio->sectors = 65535;

    rdev = rcu_dereference(conf->disks[dd_idx].rdev);
    //r1_bio->bios[0] = bio;

    struct bio *mbio = NULL;

    mbio = bio_clone_fast(bio, GFP_NOIO, bio->bi_pool);

    if(mbio == NULL)
        printk("--------mbio error------\n");


	//r1_bio->bios[0] = mbio;

	// mbio->bi_iter.bi_sector	= bio->bi_iter.bi_sector ;
	// 	bio_set_dev(mbio, rdev->bdev);
	// 	mbio->bi_end_io	= rw_end_write_request;
	// 	mbio->bi_opf = bio_op(bio) | (bio->bi_opf & (REQ_SYNC | REQ_FUA));
	// 	mbio->bi_private = r1_bio;s

	// 	atomic_inc(&r1_bio->remaining);
	// 	if (mddev->gendisk)
	// 		trace_block_bio_remap(mbio, disk_devt(mddev->gendisk),
	// 				      r1_bio->sector);
	// 	/* flush_pending_writes() needs access to the rdev so...*/
	// 	mbio->bi_disk = (void *)rdev;
        printk("--------start to submit bio--------\n");
        mbio->bi_end_io = rw_end_write_request;
        mbio->bi_opf = bio_op(bio) | (bio->bi_opf & (REQ_SYNC | REQ_FUA));
        int num_bio_sub = submit_bio_noacct(mbio);
        printk("------------%d--------------\n",num_bio_sub);
        printk("--------------finish bio------------\n");
}

/**
 * 复制写之前的操作，确定两次写入的盘号，偏移，进行两次复制写
 * 
*/
void ReadBiopage(long order, struct bio *bi,struct rwconf *conf)
{

    sector_t first_off = 0;
    int first_dd_idx = 0;

    sector_t second_off = 0;
    int second_dd_idx = 0;

    first_off = ReadPhytable(order, conf, bi, &first_dd_idx, &second_dd_idx, &second_off);
    do_replicated_write(conf,bi, first_dd_idx, first_off);
    //do_replicated_write(conf,bi, second_dd_idx, second_off);
}

//用来判断什么时候进行转换
int Judge_Condition(struct rwconf *conf)
{
    if(conf->num_rw > FusionLogicalSpace * num_block / 2)//超过一半
        return 1;
    else
        return 0;
}

//bio读取完数据后直接递交raid中的make request来进行raid处理,直接修改偏移然后提交raid
void rw_after_bioread(struct bio *bi)
{
    struct rwconf *conf = bi->bi_private;
    struct mddev *mddev = conf->mddev;
    
    //修改bio的偏移
    bi->bi_iter.bi_sector -= RW_RAID;
    md_handle_request(mddev,bi);
    

}

//转换 简单思路，将要转换的数据读进来调度给raid
//应该转化一对block
void Transfer2Raid(struct phyblock *phyblock, struct rwconf *conf)
{
   // phyblock->is_trans = 1;
    int i = 0;
   //循环处理四个数据block中的bio
    for(; i < num_block; ++i)
    {
      //  int dd_idx = (*phyblock.block[i]).d_idx;
      //  sector_t offset = phyblock->block[i].off;
      //  int size = phyblock->block[i].size;
        int dd_idx = 0;
        sector_t offset = 0;
        int size = 0;
        struct md_rdev *rdev = rcu_dereference(conf->disks[dd_idx].rdev);

        struct bio *bi = bio_alloc(GFP_KERNEL, 1);

        bi->bi_iter.bi_sector = offset;
        bi->bi_iter.bi_size = size / 512;
        bi->bi_vcnt = 1;
        // bi->bi_rw = 0;
        bi->bi_end_io = rw_after_bioread;
        bi->bi_private = conf;
    }

    //然后修改元数据
    spin_lock(&conf->count_lock);
    conf->num_rw -= 2;
    spin_unlock(&conf->count_lock);

}


/**
 * 读取数据时先改变原来bio的盘号和偏移
 * 
*/

void Change_Bio_Read(struct bio *bi, struct rwconf *conf)
{
    int d_idx = 0;
    sector_t off_disk = 0;
    struct md_rdev *rdev = NULL;

    sector_t secotr = bi->bi_iter.bi_sector;
    
    long big_block = 0;
    int off_block = 0;
    off_block = compute_block(bi,conf, &big_block);

    off_disk = conf->blocks[big_block].block[off_block].off;
    d_idx = conf->blocks[big_block].block[off_block].d_idx;

    rdev = rcu_dereference(conf->disks[d_idx].rdev);
   // bi->bi_bdev = rdev;
    bi->bi_iter.bi_sector = off_disk;
    trace_block_bio_remap(bi,
						disk_devt(conf->mddev->gendisk),
						bi->bi_iter.bi_sector);
    //generic_make_request(bi);
        submit_bio_noacct(bi);
}



//这里是调用复制写的入口函数
void Do_RW(struct bio *bi, struct rwconf *conf)
{
    const int rw = bio_data_dir(bi);
    //首先判断是读还是写
    //如果是复制读，只需要修改一下映射就可以    
    if(rw == 1) 
    {
        printk("-----enter into read-------\n");
        Change_Bio_Read(bi, conf);
    }
    else{
    	if (!md_write_start(conf->mddev,bi))
		{
            printk("-----------error--------\n");
            return;}
        printk("---------enter into write----------\n");
        long order = Translatel2f(bi,conf);//第一次映射
        printk("------------after first mapping---the order is:%ld----------\n",order);
        Translatef2p(order, bi, conf);
        ReadBiopage(order, bi, conf);//直接写入
    }
    // if(Judge_Condition(conf) == 1)
    // {
    //     spin_lock(&conf->trans_lock);
    //     Transfer2Raid(&conf->blocks[conf->num_trans],conf);
    //     conf->num_trans += 2;
    //     if(conf->num_trans >= FusionLogicalSpace * num_block)
    //         conf->num_trans = 0;
    //     spin_unlock(&conf->trans_lock);
    // }
}