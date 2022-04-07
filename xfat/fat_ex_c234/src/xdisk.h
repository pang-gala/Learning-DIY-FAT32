/**
 * ��Դ�����׵Ŀγ�Ϊ - ��0��1����дFAT32�ļ�ϵͳ��ÿ�����̶�Ӧһ����ʱ��������ע�͡�
 * ���ߣ�����ͭ
 * �γ���ַ��http://01ketang.cc
 * ��Ȩ��������Դ��ǿ�Դ�����ο���������������ǰ����ϵ���ߡ�
 */
#ifndef XDISK_H
#define	XDISK_H

#include "xtypes.h"

/**
 * �ļ�ϵͳ����
 */
typedef enum {
	FS_NOT_VALID = 0x00,            // ��Ч����
	FS_FAT32 = 0x01,                // FAT32
    FS_EXTEND = 0x05,               // ��չ����
    FS_WIN95_FAT32_0 = 0xB,         // FAT32
    FS_WIN95_FAT32_1 = 0xC,         // FAT32
}xfs_type_t;

#pragma pack(1)

/**
 * MBR�ķ�����������
 */
typedef struct _mbr_part_t {
    u8_t boot_active;               // �����Ƿ�
	u8_t start_header;              // ��ʼheader
	u16_t start_sector : 6;         // ��ʼ����
	u16_t start_cylinder : 10;	    // ��ʼ�ŵ�
	u8_t system_id;	                // �ļ�ϵͳ����
	u8_t end_header;                // ����header
	u16_t end_sector : 6;           // ��������
	u16_t end_cylinder : 10;        // �����ŵ�
	u32_t relative_sectors;	        // ����ڸ���������ʼ�����������
	u32_t total_sectors;            // �ܵ�������
}mbr_part_t;

#define MBR_PRIMARY_PART_NR	    4   // 4��������

/**
 * MBR���������ṹ
 */
typedef  struct _mbr_t {
	u8_t code[446];                 // ����������
    mbr_part_t part_info[MBR_PRIMARY_PART_NR];
	u8_t boot_sig[2];               // ������־
}mbr_t;

#pragma pack()

// ���ǰ������
struct _xdisk_t;

/**
 * ���������ӿ�
 */
typedef struct _xdisk_driver_t {
    xfat_err_t (*open) (struct _xdisk_t * disk, void * init_data);
    xfat_err_t (*close) (struct _xdisk_t * disk);
    xfat_err_t (*read_sector) (struct _xdisk_t *disk, u8_t *buffer, u32_t start_sector, u32_t count);
    xfat_err_t (*write_sector) (struct _xdisk_t *disk, u8_t *buffer, u32_t start_sector, u32_t count);
}xdisk_driver_t;

/**
 * �洢�豸����
 */
typedef struct _xdisk_t {
    const char * name;              // �豸����
    u32_t sector_size;              // ���С
	u32_t total_sector;             // �ܵĿ�����
    xdisk_driver_t * driver;        // �����ӿ�
    void * data;                    // �豸�Զ������
}xdisk_t;
	
/**
 * �洢�豸��������
 */
typedef struct _xdisk_part_t {
	u32_t start_sector;             // �������������洢����ʼ�Ŀ����
	u32_t total_sector;             // �ܵĿ�����
	xfs_type_t type;                // �ļ�ϵͳ����
	xdisk_t * disk;                 // ��Ӧ�Ĵ洢�豸
}xdisk_part_t;

xfat_err_t xdisk_open(xdisk_t *disk, const char * name, xdisk_driver_t * driver, void * init_data);
xfat_err_t xdisk_close(xdisk_t * disk);
xfat_err_t xdisk_get_part_count(xdisk_t *disk, u32_t *count);
xfat_err_t xdisk_get_part(xdisk_t *disk, xdisk_part_t *xdisk_part, int part_no);
xfat_err_t xdisk_read_sector(xdisk_t *disk, u8_t *buffer, u32_t start_sector, u32_t count);
xfat_err_t xdisk_write_sector(xdisk_t *disk, u8_t *buffer, u32_t start_sector, u32_t count);

#endif

