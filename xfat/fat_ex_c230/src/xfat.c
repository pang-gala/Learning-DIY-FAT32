/**
 * ��Դ�����׵Ŀγ�Ϊ - ��0��1����дFAT32�ļ�ϵͳ��ÿ�����̶�Ӧһ����ʱ��������ע�͡�
 * ���ߣ�����ͭ
 * �γ���ַ��http://01ketang.cc
 * ��Ȩ��������Դ��ǿ�Դ�����ο���������������ǰ����ϵ���ߡ�
 */
#include <stdlib.h>
#include "xfat.h"
#include "xdisk.h"

extern u8_t temp_buffer[512];      // todo: �����Ż�

#define xfat_get_disk(xfat)     ((xfat)->disk_part->disk)               // ��ȡdisk�ṹ

/**
 * ��dbr�н�����fat������ò���
 * @param dbr ��ȡ���豸dbr
 * @return
 */
static xfat_err_t parse_fat_header (xfat_t * xfat, dbr_t * dbr) {
    xdisk_part_t * xdisk_part = xfat->disk_part;

    // ����DBR���������������õĲ���
    xfat->root_cluster = dbr->fat32.BPB_RootClus;
    xfat->fat_tbl_sectors = dbr->fat32.BPB_FATSz32;

    // �����ֹFAT����ֻˢ��һ��FAT��
    // disk_part->start_blockΪ�÷����ľ������������ţ����Բ���Ҫ�ټ���Hidden_sector
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
 * ��ʼ��FAT��
 * @param xfat xfat�ṹ
 * @param disk_part �����ṹ
 * @return
 */
xfat_err_t xfat_open(xfat_t * xfat, xdisk_part_t * xdisk_part) {
    dbr_t * dbr = (dbr_t *)temp_buffer;
    xdisk_t * xdisk = xdisk_part->disk;
    xfat_err_t err;

    xfat->disk_part = xdisk_part;

    // ��ȡdbr������
    err = xdisk_read_sector(xdisk, (u8_t *) dbr, xdisk_part->start_sector, 1);
    if (err < 0) {
        return err;
    }

    // ����dbr�����е�fat�����Ϣ
    err = parse_fat_header(xfat, dbr);
    if (err < 0) {
        return err;
    }

    // ��һ����ȫ����ȡFAT��: todo: �Ż�
    xfat->fat_buffer = (u8_t *)malloc(xfat->fat_tbl_sectors * xdisk->sector_size);
    err = xdisk_read_sector(xdisk, (u8_t *)xfat->fat_buffer, xfat->fat_start_sector, xfat->fat_tbl_sectors);
    if (err < 0) {
        return err;
    }

    return FS_ERR_OK;
}

/**
 * ��ȡָ���غŵĵ�һ���������
 * @param xfat xfat�ṹ
 * @param cluster_no  �غ�
 * @return ������
 */
u32_t cluster_fist_sector(xfat_t *xfat, u32_t cluster_no) {
    u32_t data_start_sector = xfat->fat_start_sector + xfat->fat_tbl_sectors * xfat->fat_tbl_nr;
    return data_start_sector + (cluster_no - 2) * xfat->sec_per_cluster;    // ǰ�����غű���
}

/**
 * ���ָ�����Ƿ���ã���ռ�û򻵴�
 * @param cluster �����Ĵ�
 * @return
 */
int is_cluster_valid(u32_t cluster) {
    cluster &= 0x0FFFFFFF;
    return (cluster < 0x0FFFFFF0) && (cluster >= 0x2);     // ֵ�Ƿ���ȷ
}

/**
 * ��ȡָ���ص���һ����
 * @param xfat xfat�ṹ
 * @param curr_cluster_no
 * @param next_cluster
 * @return
 */
xfat_err_t get_next_cluster(xfat_t * xfat, u32_t curr_cluster_no, u32_t * next_cluster) {
    if (is_cluster_valid(curr_cluster_no)) {
        cluster32_t * cluster32_buf = (cluster32_t *)xfat->fat_buffer;
        *next_cluster = cluster32_buf[curr_cluster_no].s.next;
    } else {
        *next_cluster = CLUSTER_INVALID;
    }

    return FS_ERR_OK;
}

/**
 * ��ȡһ���ص����ݵ�ָ��������
 * @param xfat xfat�ṹ
 * @param buffer ���ݴ洢�Ļ�����
 * @param cluster ��ȡ����ʼ�غ�
 * @param count ��ȡ�Ĵ�����
 * @return
 */
xfat_err_t read_cluster(xfat_t *xfat, u8_t *buffer, u32_t cluster, u32_t count) {
    xfat_err_t err = 0;
    u32_t i = 0;
    u8_t * curr_buffer = buffer;
    u32_t curr_sector = cluster_fist_sector(xfat, cluster);

    for (i = 0; i < count; i++) {
        err = xdisk_read_sector(xfat_get_disk(xfat), curr_buffer, curr_sector, xfat->sec_per_cluster);
        if (err < 0) {
            return err;
        }

        curr_buffer += xfat->cluster_byte_size;
        curr_sector += xfat->sec_per_cluster;
    }

    return FS_ERR_OK;
}

/**
 * ��ָ��dir_cluster��ʼ�Ĵ����а��������ļ���
 * ���pathΪ�գ�����dir_cluster����һ���򿪵�Ŀ¼����
 * @param xfat xfat�ṹ
 * @param dir_cluster ���ҵĶ���Ŀ¼����ʼ����
 * @param file �򿪵��ļ�file�ṹ
 * @param path ��dir_cluster����Ӧ��Ŀ¼Ϊ��������·��
 * @return
 */
static xfat_err_t open_sub_file (xfat_t * xfat, u32_t dir_cluster, xfile_t * file, const char * path) {
    file->size = 0;
    file->type = FAT_DIR;
    file->start_cluster = dir_cluster; // ��Ŀ¼����ʼ�أ����Ǵ���Ķ���Ŀ¼����ʼ����
    file->curr_cluster = dir_cluster;

    file->xfat = xfat;
    file->pos = 0;
    file->err = FS_ERR_OK;
    file->attr = 0; // ��Ŀ¼��attr����Ȼ��0x00000000��������
    return FS_ERR_OK;
}

/**
 * ��ָ�����ļ���Ŀ¼
 * @param xfat xfat�ṹ
 * @param file �򿪵��ļ���Ŀ¼
 * @param path �ļ���Ŀ¼���ڵ�����·��
 * @return
 */
xfat_err_t xfile_open(xfat_t * xfat, xfile_t * file, const char * path) {
    return open_sub_file(xfat, xfat->root_cluster, file, path);
}

/**
 * �ر��Ѿ��򿪵��ļ�
 * @param file ���رյ��ļ�
 * @return
 */
xfat_err_t xfile_close(xfile_t *file) {
    return FS_ERR_OK;
}
