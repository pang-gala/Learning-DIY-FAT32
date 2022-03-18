/**
 * 本源码配套的课程为 - 从0到1动手写FAT32文件系统。每个例程对应一个课时，尽可能注释。
 * 作者：李述铜
 * 课程网址：http://01ketang.cc
 * 版权声明：本源码非开源，二次开发，或其它商用前请联系作者。
 */
#include <stdlib.h>
#include "xfat.h"
#include "xdisk.h"

extern u8_t temp_buffer[512];      // todo: 缓存优化

#define xfat_get_disk(xfat)     ((xfat)->disk_part->disk)               // 获取disk结构

/**
 * 从dbr中解析出fat相关配置参数
 * @param dbr 读取的设备dbr
 * @return
 */
static xfat_err_t parse_fat_header (xfat_t * xfat, dbr_t * dbr) {
    xdisk_part_t * xdisk_part = xfat->disk_part;

    // 解析DBR参数，解析出有用的参数
    xfat->root_cluster = dbr->fat32.BPB_RootClus;
    xfat->fat_tbl_sectors = dbr->fat32.BPB_FATSz32;

    // 如果禁止FAT镜像，只刷新一个FAT表
    // disk_part->start_block为该分区的绝对物理扇区号，所以不需要再加上Hidden_sector
    if (dbr->fat32.BPB_ExtFlags & (1 << 7)) {
        u32_t table = dbr->fat32.BPB_ExtFlags & 0xF;
        xfat->fat_start_sector = dbr->bpb.BPB_RsvdSecCnt + xdisk_part->start_sector + table * xfat->fat_tbl_sectors;
        xfat->fat_tbl_nr = 1;
    } else {
        xfat->fat_start_sector = dbr->bpb.BPB_RsvdSecCnt + xdisk_part->start_sector;
        xfat->fat_tbl_nr = dbr->bpb.BPB_NumFATs;
    }

    xfat->sec_per_cluster = dbr->bpb.BPB_SecPerClus;
    xfat->total_sectors = dbr->bpb.BPB_TotSec32;
    xfat->cluster_byte_size = xfat->sec_per_cluster * dbr->bpb.BPB_BytsPerSec;

    return FS_ERR_OK;
}

/**
 * 初始化FAT项
 * @param xfat xfat结构
 * @param disk_part 分区结构
 * @return
 */
xfat_err_t xfat_open(xfat_t * xfat, xdisk_part_t * xdisk_part) {
    dbr_t * dbr = (dbr_t *)temp_buffer;
    xdisk_t * xdisk = xdisk_part->disk;
    xfat_err_t err;

    xfat->disk_part = xdisk_part;

    // 读取dbr参数区
    err = xdisk_read_sector(xdisk, (u8_t *) dbr, xdisk_part->start_sector, 1);
    if (err < 0) {
        return err;
    }

    // 解析dbr参数中的fat相关信息
    err = parse_fat_header(xfat, dbr);
    if (err < 0) {
        return err;
    }

    // 先一次性全部读取FAT表: todo: 优化
    xfat->fat_buffer = (u8_t *)malloc(xfat->fat_tbl_sectors * xdisk->sector_size);
    err = xdisk_read_sector(xdisk, (u8_t *)xfat->fat_buffer, xfat->fat_start_sector, xfat->fat_tbl_sectors); //
    if (err < 0) {
        return err;
    }

    return FS_ERR_OK;
}

/**
 * 获取数据区中这一簇的开始扇区
 * @param xfat xfat结构
 * @param cluster_no  簇号
 * @return 扇区号
 */
u32_t cluster_fist_sector(xfat_t *xfat, u32_t cluster_no) {
    u32_t data_start_sector = xfat->fat_start_sector + xfat->fat_tbl_sectors * xfat->fat_tbl_nr;// 数据区起始位置
    return data_start_sector + (cluster_no - 2) * xfat->sec_per_cluster;    // 当前簇的扇区号 = 数据区起始位置  + （簇号-2） *  每簇的扇区数
}

/**
 * 输入一个簇号，检查这个簇号代表的簇是否是有效的，非占用或坏簇；
 * @param cluster 待检查的簇号
 * @return
 */
int is_cluster_valid(u32_t cluster) {
    cluster &= 0x0FFFFFFF; // 对前28位（什么的？簇的？还是记录表项的？）的数字；
    // 因为0xFFFFFFF0开始到0xFFFFFFFF这8个分别是 系统保留 坏簇标志 文件结束标志 
    // ————fat设计者希望我如果根据fat表向后不断跳转时遇到了这10个最大的数值，就不要继续往后读了！
    return (cluster < 0x0FFFFFF0) && (cluster >= 0x00000002);     
    // 这些值表示当前表项对应的簇是有效的，整个0x00000000~0x0FFFFFFF部分是对一个簇有效的标识，而其中0x00000002~0x0FFFFFF0又是正常分配的簇的值范围
}

/**
 * 向fat表获取指定簇的下一个簇的簇号：原理是查询fat表项，读取其内容，跳转。
 * @param xfat xfat结构
 * @param curr_cluster_no 当前簇号
 * @param next_cluster 指向下一簇号的指针
 * @return
 */
xfat_err_t get_next_cluster(xfat_t * xfat, u32_t curr_cluster_no, u32_t * next_cluster) {
    if (is_cluster_valid(curr_cluster_no)) {
        cluster32_t * cluster32_buf = (cluster32_t *)xfat->fat_buffer; // cluster32_buf是fat32表项类型，指向所有fat表项的起始地址
        *next_cluster = cluster32_buf[curr_cluster_no].s.next;
    } else {
        *next_cluster = CLUSTER_INVALID;
    }

    return FS_ERR_OK;
}

/**
 * 读取指定fat系统中从指定簇开始的数个的内容到指定缓冲区
 * @param xfat xfat结构 
 * @param buffer 数据存储的缓冲区 
 * @param cluster 读取的起始簇号 
 * @param count 读取的簇数量 
 * @return 
 */
xfat_err_t read_cluster(xfat_t *xfat, u8_t *buffer, u32_t cluster, u32_t count) {
    xfat_err_t err = 0;
    u32_t i = 0;
    u8_t * curr_buffer = buffer; // curr_buffer指向当前向buffer写入到的位置 
    u32_t curr_sector = cluster_fist_sector(xfat, cluster); // 此变量初始时指向数据区中这一簇的开始扇区；（绝对扇区编号）
     
    // 读取count个簇到扇区 
    for (i = 0; i < count; i++) {
        err = xdisk_read_sector(xfat_get_disk(xfat), curr_buffer, curr_sector, xfat->sec_per_cluster);
        if (err < 0) { 
            return err;
        }

        curr_buffer += xfat->cluster_byte_size;// curr_buffer前进到下一个位置
        curr_sector += xfat->sec_per_cluster;// 下一次从这里开始读取
    }

    return FS_ERR_OK;
}
