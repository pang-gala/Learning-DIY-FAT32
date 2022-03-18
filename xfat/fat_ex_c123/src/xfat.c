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
    err = xdisk_read_sector(xdisk, (u8_t *)xfat->fat_buffer, xfat->fat_start_sector, xfat->fat_tbl_sectors); //
    if (err < 0) {
        return err;
    }

    return FS_ERR_OK;
}

/**
 * ��ȡ����������һ�صĿ�ʼ����
 * @param xfat xfat�ṹ
 * @param cluster_no  �غ�
 * @return ������
 */
u32_t cluster_fist_sector(xfat_t *xfat, u32_t cluster_no) {
    u32_t data_start_sector = xfat->fat_start_sector + xfat->fat_tbl_sectors * xfat->fat_tbl_nr;// ��������ʼλ��
    return data_start_sector + (cluster_no - 2) * xfat->sec_per_cluster;    // ��ǰ�ص������� = ��������ʼλ��  + ���غ�-2�� *  ÿ�ص�������
}

/**
 * ����һ���غţ��������غŴ���Ĵ��Ƿ�����Ч�ģ���ռ�û򻵴أ�
 * @param cluster �����Ĵغ�
 * @return
 */
int is_cluster_valid(u32_t cluster) {
    cluster &= 0x0FFFFFFF; // ��ǰ28λ��ʲô�ģ��صģ����Ǽ�¼����ģ��������֣�
    // ��Ϊ0xFFFFFFF0��ʼ��0xFFFFFFFF��8���ֱ��� ϵͳ���� ���ر�־ �ļ�������־ 
    // ��������fat�����ϣ�����������fat����󲻶���תʱ��������10��������ֵ���Ͳ�Ҫ����������ˣ�
    return (cluster < 0x0FFFFFF0) && (cluster >= 0x00000002);     
    // ��Щֵ��ʾ��ǰ�����Ӧ�Ĵ�����Ч�ģ�����0x00000000~0x0FFFFFFF�����Ƕ�һ������Ч�ı�ʶ��������0x00000002~0x0FFFFFF0������������Ĵص�ֵ��Χ
}

/**
 * ��fat���ȡָ���ص���һ���صĴغţ�ԭ���ǲ�ѯfat�����ȡ�����ݣ���ת��
 * @param xfat xfat�ṹ
 * @param curr_cluster_no ��ǰ�غ�
 * @param next_cluster ָ����һ�غŵ�ָ��
 * @return
 */
xfat_err_t get_next_cluster(xfat_t * xfat, u32_t curr_cluster_no, u32_t * next_cluster) {
    if (is_cluster_valid(curr_cluster_no)) {
        cluster32_t * cluster32_buf = (cluster32_t *)xfat->fat_buffer; // cluster32_buf��fat32�������ͣ�ָ������fat�������ʼ��ַ
        *next_cluster = cluster32_buf[curr_cluster_no].s.next;
    } else {
        *next_cluster = CLUSTER_INVALID;
    }

    return FS_ERR_OK;
}

/**
 * ��ȡָ��fatϵͳ�д�ָ���ؿ�ʼ�����������ݵ�ָ��������
 * @param xfat xfat�ṹ 
 * @param buffer ���ݴ洢�Ļ����� 
 * @param cluster ��ȡ����ʼ�غ� 
 * @param count ��ȡ�Ĵ����� 
 * @return 
 */
xfat_err_t read_cluster(xfat_t *xfat, u8_t *buffer, u32_t cluster, u32_t count) {
    xfat_err_t err = 0;
    u32_t i = 0;
    u8_t * curr_buffer = buffer; // curr_bufferָ��ǰ��bufferд�뵽��λ�� 
    u32_t curr_sector = cluster_fist_sector(xfat, cluster); // �˱�����ʼʱָ������������һ�صĿ�ʼ������������������ţ�
     
    // ��ȡcount���ص����� 
    for (i = 0; i < count; i++) {
        err = xdisk_read_sector(xfat_get_disk(xfat), curr_buffer, curr_sector, xfat->sec_per_cluster);
        if (err < 0) { 
            return err;
        }

        curr_buffer += xfat->cluster_byte_size;// curr_bufferǰ������һ��λ��
        curr_sector += xfat->sec_per_cluster;// ��һ�δ����￪ʼ��ȡ
    }

    return FS_ERR_OK;
}
