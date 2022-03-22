/**
 * ��Դ�����׵Ŀγ�Ϊ - ��0��1����дFAT32�ļ�ϵͳ��ÿ�����̶�Ӧһ����ʱ��������ע�͡�
 * ���ߣ�����ͭ
 * �γ���ַ��http://01ketang.cc
 * ��Ȩ��������Դ��ǿ�Դ�����ο���������������ǰ����ϵ���ߡ�
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "xfat.h"
#include "xdisk.h"

extern u8_t temp_buffer[512];      // todo: �����Ż�

// ���õ�.��..�ļ���             "12345678ext"
#define DOT_FILE                ".          "
#define DOT_DOT_FILE            "..         "

#define is_path_sep(ch)         (((ch) == '\\') || ((ch == '/')))       // �ж��Ƿ����ļ����ָ���
#define xfat_get_disk(xfat)     ((xfat)->disk_part->disk)               // ��ȡdisk�ṹ
#define to_sector(disk, offset)     ((offset) / (disk)->sector_size)    // ����ϡת��Ϊ������
#define to_sector_offset(disk, offset)   ((offset) % (disk)->sector_size)   // ��ȡ�����е����ƫ��
#define to_cluster_offset(xfat, pos)      ((pos) % ((xfat)->cluster_byte_size)) // ��ȡ���е����ƫ��

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
 * ��ָ����name��FAT 8+3����ת��
 * @param dest_name
 * @param my_name
 * @return
 */
static xfat_err_t to_sfn(char* dest_name, const char* my_name) {
    int i, name_len;
    char * dest = dest_name;
    const char * ext_dot;
    const char * p;
    int ext_existed;

    memset(dest, ' ', SFN_LEN);

    // ������ͷ�ķָ���
    while (is_path_sep(*my_name)) {
        my_name++;
    }

    // �ҵ���һ��б��֮ǰ���ַ�������ext_dot��λ������Ҽ�¼��Ч����
    ext_dot = my_name;
    p = my_name;
    name_len = 0;
    while ((*p != '\0') && !is_path_sep(*p)) {
        if (*p == '.') {
            ext_dot = p;
        }
        p++;
        name_len++;
    }

    // ����ļ�����.��β����˼����û����չ����
    // todo: ���ļ�������?
    ext_existed = (ext_dot > my_name) && (ext_dot < (my_name + name_len - 1));

    // �������ƣ���������ַ�, ����.�ָ������12�ֽڣ�������������ֻӦ��
    p = my_name;
    for (i = 0; (i < SFN_LEN) && (*p != '\0') && !is_path_sep(*p); i++) {
        if (ext_existed) {
            if (p == ext_dot) {
                dest = dest_name + 8;
                p++;
                i--;
                continue;
            }
            else if (p < ext_dot) {
                *dest++ = toupper(*p++);
            }
            else {
                *dest++ = toupper(*p++);
            }
        }
        else {
            *dest++ = toupper(*p++);
        }
    }
    return FS_ERR_OK;
}

/**
 * �ж������ļ����Ƿ�ƥ��
 * @param name_in_item fatdir�е��ļ�����ʽ
 * @param my_name Ӧ�ÿɶ����ļ�����ʽ
 * @return
 */
static u8_t is_filename_match(const char *name_in_dir, const char *to_find_name) {
    char temp_name[SFN_LEN];

    // FAT�ļ����ıȽϼ��ȣ�ȫ��ת���ɴ�д�Ƚ�
    // ����Ŀ¼�Ĵ�Сд���ã�����ת����8+3���ƣ��ٽ������ֽڱȽ�
    // ��ʵ����ʾʱ�������diritem->NTRes���д�Сдת��
    to_sfn(temp_name, to_find_name);
    return memcmp(temp_name, name_in_dir, SFN_LEN) == 0;
}

/**
 * ������ͷ�ķָ���
 * @param path Ŀ��·��
 * @return
 */
static const char * skip_first_path_sep (const char * path) {
    const char * c = path;

    // ������ͷ�ķָ���
    while (is_path_sep(*c)) {
        c++;
    }
    return c;
}

/**
 * ��ȡ��·��
 * @param dir_path ��һ��·��
 * @return
 */
const char * get_child_path(const char *dir_path) {
    const char * c = skip_first_path_sep(dir_path);

    // ������Ŀ¼
    while ((*c != '\0') && !is_path_sep(*c)) {
        c++;
    }

    return (*c == '\0') ? (const char *)0 : c + 1;
}

/**
 * ����diritem����ȡ�ļ�����
 * @param diritem �������diritem
 * @return
 */
static xfile_type_t get_file_type(const diritem_t *diritem) {
    xfile_type_t type;

    if (diritem->DIR_Attr & DIRITEM_ATTR_VOLUME_ID) {
        type = FAT_VOL;
    } else if (diritem->DIR_Attr & DIRITEM_ATTR_DIRECTORY) {
        type = FAT_DIR;
    } else {
        type = FAT_FILE;
    }

    return type;
}

/**
 * ��������diritem�е�Date��time��ʱ����Ϣ��xfile_time_t��
 * @param dest ָ���洢��ʱ����Ϣ�ṹ
 * @param date fat��ʽ������
 * @param time fat��ʽ��ʱ��
 * @param mil_sec fat��ʽ��10����
 */
static void copy_date_time(xfile_time_t *dest, const diritem_date_t *date,
                           const diritem_time_t *time, const u8_t mil_sec) {
    // date��time����ָ�붼�п����ǿ�
    if (date) {
        dest->year = (u16_t)(date->year_from_1980 + 1980);
        dest->month = (u8_t)date->month;
        dest->day = (u8_t)date->day;
    } else {
        dest->year = 0;
        dest->month = 0;
        dest->day = 0;
    }

    if (time) {
        dest->hour = (u8_t)time->hour;
        dest->minute = (u8_t)time->minute;
        dest->second = (u8_t)(time->second_2 * 2 + mil_sec / 100);
    } else {
        dest->hour = 0;
        dest->minute = 0;
        dest->second = 0;
    }
}

/**
 * ��fat_dir��ʽ���ļ����п������û��ɶ����ļ�����dest_name
 * @param dest_name ת������ļ����洢������
 * @param raw_name fat_dir��ʽ���ļ���
 */
static void sfn_to_myname(char *dest_name, const diritem_t * diritem) {
    int i;
    char * dest = dest_name;
    char * raw_name = (char *)diritem->DIR_Name;

    // �ж��Ƿ�����չ����
    u8_t ext_exist = raw_name[8] != 0x20; // diritem��DIR_Name��ʼ�ĵڰ˸��ַ�����Ӧ��DIR_ExtName[3]�ĵ�һ���ַ�����������ǿո�˵����չ����Ϊ��(0x20)��Ҳ��������չ����
    
    // ��Ž���ļ����������
    u8_t scan_len = ext_exist ? SFN_LEN + 1 : SFN_LEN; // ���������չ�����������Ϊ11+1(����һ��.��)

    memset(dest_name, 0, X_FILEINFO_NAME_SIZE);   // ��ս��������������֪��Ϊʲô��յĳ�����32.
    
    // Ҫ���Ǵ�Сд���⣬����NTRes����ת������Ӧ�Ĵ�Сд
    for (i = 0; i < scan_len; i++) {
        if (*raw_name == ' ') {// �������8+3�ļ��������˿ո�������������ԭ�ļ���������"123   ABC"
            raw_name++;
        } else if ((i == 8) && ext_exist) { // ɨ�赽�ļ�������,��������չ�������ڽ���ַ�������һ��λ��������һ��.��չ��
           *dest++ = '.';
        } else {
            // ����ͨ�ַ����������Ҫ�����Сд����
            u8_t lower = 0; // Ĭ�ϴ�д

            if (((i < 8) && (diritem->DIR_NTRes & DIRITEM_NTRES_BODY_LOWER)) // DIRITEM_NTRES_BODY_LOWER��ʾ�ļ���Сд��ֵΪ00001000
                || ((i > 8) && (diritem->DIR_NTRes & DIRITEM_NTRES_EXT_LOWER))) { // �����ʾ��չ��Сд��ֵΪ
                lower = 1;
            }

            *dest++ = lower ? tolower(*raw_name++) : toupper(*raw_name++); // �Դ�Сд��ʽ���
        }
    }

    *dest = '\0'; // ����һ���ַ�����ʽ�Ľ��ʱ����ü��Ͻ����������������ã�
}

/**
 * ��ȡdiritem���ļ���ʼ�غ�
 * @param item
 * @return
 */
static u32_t get_diritem_cluster (diritem_t * item) {
    return (item->DIR_FstClusHI << 16) | item->DIR_FstClusL0;
}

/**
 * ��dir_item����Ӧ���ļ���Ϣת������fs_fileinfo_t��
 * @param info ��Ϣ�洢��λ��
 * @param dir_item fat��diritem
 */
static void copy_file_info(xfileinfo_t *info, const diritem_t * dir_item) {
    sfn_to_myname(info->file_name, dir_item); // dir_item��ָ��Ŀ¼����ļ�����ת��ͨӦ�ò��ļ�����
    info->size = dir_item->DIR_FileSize;
    info->attr = dir_item->DIR_Attr;
    info->type = get_file_type(dir_item);

    // ������������ʡ��޸�ʱ��
    copy_date_time(&info->create_time, &dir_item->DIR_CrtDate, &dir_item->DIR_CrtTime, dir_item->DIR_CrtTimeTeenth); 
    copy_date_time(&info->last_acctime, &dir_item->DIR_LastAccDate, (diritem_time_t *) 0, 0) ; //�������ʱ�䣬fatֻ��¼��Date������������0
    copy_date_time(&info->modify_time, &dir_item->DIR_WrtDate, &dir_item->DIR_WrtTime, 0); // ����ʱ��ֻ��¼��date��ʱ����
}

/**
 * ����ļ����������Ƿ�ƥ��
 * @param dir_item
 * @param locate_type
 * @return
 */
static u8_t is_locate_type_match (diritem_t * dir_item, u8_t locate_type) {
    u8_t match = 1;

    if ((dir_item->DIR_Attr & DIRITEM_ATTR_SYSTEM) && !(locate_type & XFILE_LOCALE_SYSTEM)) {
        match = 0;  // ����ʾϵͳ�ļ�
    } else if ((dir_item->DIR_Attr & DIRITEM_ATTR_HIDDEN) && !(locate_type & XFILE_LOCATE_HIDDEN)) {
        match = 0;  // ����ʾ�����ļ�
    } else if ((dir_item->DIR_Attr & DIRITEM_ATTR_VOLUME_ID) && !(locate_type & XFILE_LOCATE_VOL)) {
        match = 0;  // ����ʾ���
    } else if ((memcmp(DOT_FILE, dir_item->DIR_Name, SFN_LEN) == 0)
                || (memcmp(DOT_DOT_FILE, dir_item->DIR_Name, SFN_LEN) == 0)) {
        if (!(locate_type & XFILE_LOCATE_DOT)) {
            match = 0;// ����ʾdot�ļ�
        }
    } else if (!(locate_type & XFILE_LOCATE_NORMAL)) {
        match = 0;
    }
    return match;
}

/**
 * ����ָ��dir_item����������Ӧ�Ľṹ
 * @param xfat xfat�ṹ
 * @param locate_type ��λ��item����
 * @param dir_cluster dir_item���ڵ�Ŀ¼���ݴغ�
 * @param cluster_offset ���е�ƫ��
 * @param move_bytes ���ҵ���Ӧ��item���������ʼ�����ƫ��ֵ���ƶ��˶��ٸ��ֽڲŶ�λ����item
 * @param path �ļ���Ŀ¼������·��
 * @param r_diritem ���ҵ���diritem��
 * @return
 */
static xfat_err_t locate_file_dir_item(xfat_t *xfat, u8_t locate_type, u32_t *dir_cluster, u32_t *cluster_offset,
                                    const char *path, u32_t *move_bytes, diritem_t **r_diritem) {
    u32_t curr_cluster = *dir_cluster;
    xdisk_t * xdisk = xfat_get_disk(xfat);
    u32_t initial_sector = to_sector(xdisk, *cluster_offset);
    u32_t initial_offset = to_sector_offset(xdisk, *cluster_offset);
    u32_t r_move_bytes = 0;

    // cluster
    do {
        u32_t i;
        xfat_err_t err;
        u32_t start_sector = cluster_fist_sector(xfat, curr_cluster);

        for (i = initial_sector; i < xfat->sec_per_cluster; i++) {
            u32_t j;

            err = xdisk_read_sector(xdisk, temp_buffer, start_sector + i, 1);
            if (err < 0) {
                return err;
            }

            for (j = initial_offset / sizeof(diritem_t); j < xdisk->sector_size / sizeof(diritem_t); j++) {
                diritem_t *dir_item = ((diritem_t *) temp_buffer) + j;

                if (dir_item->DIR_Name[0] == DIRITEM_NAME_END) {
                    return FS_ERR_EOF;
                } else if (dir_item->DIR_Name[0] == DIRITEM_NAME_FREE) {
                    r_move_bytes += sizeof(diritem_t);
                    continue;
                } else if (!is_locate_type_match(dir_item, locate_type)) {
                    r_move_bytes += sizeof(diritem_t);
                    continue;
                }

                if ((path == (const char *) 0)
                    || (*path == 0)
                    || is_filename_match((const char *) dir_item->DIR_Name, path)) {

                    u32_t total_offset = i * xdisk->sector_size + j * sizeof(diritem_t);
                    *dir_cluster = curr_cluster;
                    *move_bytes = r_move_bytes + sizeof(diritem_t);
                    *cluster_offset = total_offset;
                    if (r_diritem) {
                        *r_diritem = dir_item;
                    }

                    return FS_ERR_OK;
                }

                r_move_bytes += sizeof(diritem_t);
            }
        }

        err = get_next_cluster(xfat, curr_cluster, &curr_cluster);
        if (err < 0) {
            return err;
        }

        initial_sector = 0;
        initial_offset = 0;
    }while (is_cluster_valid(curr_cluster));

    return FS_ERR_EOF;
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
    u32_t parent_cluster = dir_cluster;
    u32_t parent_cluster_offset = 0;

    path = skip_first_path_sep(path);

    // �������·����Ϊ�գ���鿴��Ŀ¼
    // ����ֱ����Ϊdir_clusterָ�����һ��Ŀ¼�����ڴ򿪸�Ŀ¼
    if ((path != 0) && (*path != '\0')) {
        diritem_t * dir_item = (diritem_t *)0;
        u32_t file_start_cluster = 0;
        const char * curr_path = path;

       // �ҵ�path��Ӧ����ʼ��
        while (curr_path != (const char *)0) {
            u32_t moved_bytes = 0;
            dir_item = (diritem_t *)0;

            // �ڸ�Ŀ¼�²���ָ��·����Ӧ���ļ�
            xfat_err_t err = locate_file_dir_item(xfat, XFILE_LOCATE_DOT | XFILE_LOCATE_NORMAL,
                    &parent_cluster, &parent_cluster_offset,curr_path, &moved_bytes, &dir_item);
            if (err < 0) {
                return err;
            }

            if (dir_item == (diritem_t *)0) {
                return FS_ERR_NONE;
            }

            curr_path = get_child_path(curr_path);
            if (curr_path != (const char *)0) {
                parent_cluster = get_diritem_cluster(dir_item);
                parent_cluster_offset = 0;
            } else {
                file_start_cluster = get_diritem_cluster(dir_item);

                // �����..�Ҷ�Ӧ��Ŀ¼����clusterֵΪ0���������ȷ��ֵ
                if ((memcmp(dir_item->DIR_Name, DOT_DOT_FILE, SFN_LEN) == 0) && (file_start_cluster == 0)) {
                    file_start_cluster = xfat->root_cluster;
                }
            }
        }

        file->size = dir_item->DIR_FileSize;
        file->type = get_file_type(dir_item);
        file->start_cluster = file_start_cluster;
        file->curr_cluster = file_start_cluster;
    } else {
        file->size = 0;
        file->type = FAT_DIR;
        file->start_cluster = parent_cluster;
        file->curr_cluster = parent_cluster;
    }

    file->xfat = xfat;
    file->pos = 0;
    file->err = FS_ERR_OK;
    return FS_ERR_OK;
}

/**
 * ��ָ�����ļ���Ŀ¼
 * @param xfat xfat�ṹ
 * @param file �򿪵��ļ���Ŀ¼
 * @param path �ļ���Ŀ¼���ڵ�����·�����ݲ�֧�����·��
 * @return
 */
xfat_err_t xfile_open(xfat_t * xfat, xfile_t * file, const char * path) {
	path = skip_first_path_sep(path);

	// ��Ŀ¼�������ϼ�Ŀ¼
	// ������.��ֱ�ӹ��˵�·��
	if (memcmp(path, "..", 2) == 0) {
		return FS_ERR_NONE;
	} else if (memcmp(path, ".", 1) == 0) {
		path++;
	}

    return open_sub_file(xfat, xfat->root_cluster, file, path);
}

/**
 * ����ָ��Ŀ¼�µĵ�һ���ļ���Ϣ
 * @param file �Ѿ��򿪵��ļ�
 * @param info ��һ���ļ����ļ���Ϣ
 * @return
 */
xfat_err_t xdir_first_file (xfile_t * file, xfileinfo_t * info) {
    diritem_t * diritem = (diritem_t *)0;
    xfat_err_t err;
    u32_t moved_bytes = 0;
    u32_t cluster_offset;

    // ��������Ŀ¼������
    if (file->type != FAT_DIR) {
        return FS_ERR_PARAM;
    }

    // ���µ�������λ��
    file->curr_cluster = file->start_cluster;
    file->pos = 0;

    cluster_offset = 0;
    err = locate_file_dir_item(file->xfat, XFILE_LOCATE_NORMAL,
            &file->curr_cluster, &cluster_offset, "", &moved_bytes, &diritem); // moved_bytes��������pos������posָ����������������ص�ƫ��֮�ͣ�����
    if (err < 0) {
        return err;
    }

    if (diritem == (diritem_t *)0) {
        return FS_ERR_EOF;
    }

    // ����pos
    file->pos += moved_bytes;

    // �ҵ��󣬿����ļ���Ϣ
    copy_file_info(info, diritem); // ��diritem����һ���ļ���Ϣ������Ϊ�������
    return err;
}

/**
 * ����ָ��Ŀ¼���������ļ��������ļ�����)
 * ԭ���ڵ�ǰfile��ָ��Ŀ¼�У���file�д洢��(xfat, pos)��ָ���λ�ÿ�ʼ������Ķ����Ŀ¼�ļ����õ���һ���ļ��ļ�¼��
 * ���������1���������û���ļ����򷵻�FS_ERR_EOF��2��������file����Ŀ¼�ļ�������FS_ERR_PARAM��
 * @param file �Ѿ��򿪵�Ŀ¼
 * @param info ��õ��ļ���Ϣ
 * @return
 */
xfat_err_t xdir_next_file (xfile_t * file, xfileinfo_t * info) {
    xfat_err_t err;
    diritem_t * dir_item = (diritem_t *)0;
    u32_t moved_bytes = 0;
    u32_t cluster_offset;

    // ������Ŀ¼
    if (file->type != FAT_DIR) {
        return FS_ERR_PARAM;
    }

    // �����ļ���Ŀ¼
    cluster_offset = to_cluster_offset(file->xfat, file->pos);// pos������Ŀ¼�ļ��е�ƫ�ƣ���Խ0~n����
                                                              //��32λ�޷���������ʾ4GB����fat32�б�ʾһ���ļ��ڵ�ƫ��/��С��ô�����ˣ���
    err = locate_file_dir_item(file->xfat, XFILE_LOCATE_NORMAL,
            &file->curr_cluster, &cluster_offset, "", &moved_bytes, &dir_item);
    if (err != FS_ERR_OK) {
        return err;
    }

    // ������û���ļ���
    if (dir_item == (diritem_t *)0) {
        return FS_ERR_EOF;
    }

    // ����pos
    file->pos += moved_bytes;

    // �������淵�ص�cluster_offset������һ�α�����ʼλ�ã�curr_cluster��cluster_offset����
    // �������������Ǽ���Ƿ�Ҫ���´أ����������ȡһĿ¼���Խ����ǰ���ˣ���ǰ����һ����
    if (cluster_offset + sizeof(diritem_t) >= file->xfat->cluster_byte_size) { 
        err = get_next_cluster(file->xfat, file->curr_cluster, &file->curr_cluster);
        if (err < 0) {
            return err;
        }
    }

    copy_file_info(info, dir_item);
    return err;
}

/**
 * �ر��Ѿ��򿪵��ļ�
 * @param file ���رյ��ļ�
 * @return
 */
xfat_err_t xfile_close(xfile_t *file) {
    return FS_ERR_OK;
}
