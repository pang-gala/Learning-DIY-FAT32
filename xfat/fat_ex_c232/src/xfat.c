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

#define is_path_sep(ch)         (((ch) == '\\') || ((ch == '/')))       // �ж��Ƿ����ļ����ָ���
#define xfat_get_disk(xfat)     ((xfat)->disk_part->disk)               // ��ȡdisk�ṹ
#define to_sector(disk, offset)     ((offset) / (disk)->sector_size)    // ����ϡת��Ϊ������
#define to_sector_offset(disk, offset)   ((offset) % (disk)->sector_size)   // ��ȡ�����е����ƫ��

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
 * ԭ�����ȱ���������ļ�����ȷ���Ƿ��к�׺��.���ڵڶ����ַ�~�����ڶ����ַ����֣���������ڡ��޺�׺������.��..�ļ�������ֱ�Ӵ�д
 * @param dest_name ת����õ�8+3�ļ���
 * @param my_name Ҫת�����ļ�����������һ��·�����磺�������/a/b/c.txt,�򽫻ᴦ��a����ļ�
 * @return
 */
static xfat_err_t to_sfn(char* dest_name, const char* my_name) {
    int i, name_len;
    char * dest = dest_name;
    const char * ext_dot;
    const char * p;
    int ext_existed;

    memset(dest, ' ', SFN_LEN);

    // /a/b/c.txt
    // ������ͷ�ķָ�����������ڵĻ���
    while (is_path_sep(*my_name)) {
        my_name++;
    }
    // a/b/c.txt

    // �ҵ���һ��б��֮ǰ���ַ�������ext_dot��λ������Ҽ�¼��Ч����
    ext_dot = my_name;
    p = my_name;
    name_len = 0;
    // ������ǰ������ļ������ӵ�ǰ�ļ����ĵ�һ���ַ�����������ǰ�ļ��ǽ�����������Ҳ����(*p != '\0') && !is_path_sep(*p)Ϊ��ʱ��
    while ((*p != '\0') && !is_path_sep(*p)) {
        if (*p == '.') {// ���������'.'��˵���ҵ��˴����׺���ĵ�
            ext_dot = p;
        }
        p++;
        name_len++;
    }

    // ע������Ƿ������չ���жϣ�Ҫ���ļ���������'.'�ſ�ͷ���β��������������������Ƿ���Ϲ��򣬻���֪������������������������
    // todo: ���ļ�������
    ext_existed = (ext_dot > my_name) && (ext_dot < (my_name + name_len - 1));  // ���ext_dotָ��ĵط������ֵ�һ���ַ�֮���ļ����ǿգ��������������һ���ַ�֮ǰ����׺���ǿգ�����
                                                                                // ��˵������չ����ע�⣬����ext_dot��my_name��ָ��֮��ıȽϣ�

    // �������ƣ���������ַ�, ����.�����12�ֽڣ�������������ֻӦ��
    p = my_name;
    for (i = 0; (i < SFN_LEN) && (*p != '\0') && !is_path_sep(*p); i++) {
        if (ext_existed) 
        // ����չ�������
        {
            if (p == ext_dot) {
                dest = dest_name + 8;// dest������д��λ�ã�ֱ��ǰ�����ھ�λ����Ϊ�Ѿ�������.����˵���ļ����Ĳ��ֽ�����
                p++;// pָ������ǰ��
                i--;// ��Ϊ������.�������char��8+3�ļ������ǲ�ռλ�õģ�������һ��ѭ������Ҫi++��
                continue;// û�������continue����������
            }
            // �����������û����ֻҪ�����ַ�����תΪ��д�洢
            else if (p < ext_dot) {
                *dest++ = toupper(*p++);
            }
            else {
                *dest++ = toupper(*p++);
            }
        }
        else 
        // ����չ���������Ҳ����û�г���.���ļ��аɣ����Լ�.��..��.txt��abc.�����ıȽ���ֵ�����
        //����Щ���ƣ�����ȫ������д��������.��..���������������ֽ�����ȷ�Ĵ�����������ʶ��Ϊ��չ������
        // �������⽫�ᵼ��.txt��abc.�������ļ���������û����չ������������©�������ֵĴ���
        {
            *dest++ = toupper(*p++);
        }
    }
    return FS_ERR_OK;
}

/**
 * �ж������ļ����Ƿ�ƥ��
 * @param name_in_dir fatdir�е�sfn��ʽ�ļ���
 * @param to_find_name Ӧ�ò㴫����ļ�����������һ��·�����磺�������/a/b/c.txt,�򽫻ᴦ��a����ļ�
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
 * ��ȡ��·����������"/abc/def/456.txt"���򷵻ء�def/456.txt��
 * @param dir_path ������һ�����Ƶ�·��
 * @return 
 */
const char * get_child_path(const char *dir_path) {

    // ��������ʱΪ "/abc/456.txt"
    const char * c = skip_first_path_sep(dir_path);

    // ��������ʱΪ "abc/456.txt"
    // ������Ŀ¼��������Ŀ¼����ķָ���
    while ((*c != '\0') && !is_path_sep(*c)) {
        c++; 
    }

    // ��������ʱΪ "/456.txt",�򷵻�"456.txt"������Ѿ�ɶ��û�ˣ��򷵻�ָ��յ�ָ�룻
    return (*c == '\0') ? (const char *)0 : c + 1;
}

/**
 * ��ȡĿ¼����ָ�ļ������ͣ��Զ����3���ļ����ͣ����˸��ӵķ��������
 * @param diritem �������diritem
 * @return
 */
static xfile_type_t get_file_type(const diritem_t *diritem) {
    xfile_type_t type;

    if (diritem->DIR_Attr & DIRITEM_ATTR_VOLUME_ID) {
        type = FAT_VOL;
    } else if (diritem->DIR_Attr & DIRITEM_ATTR_DIRECTORY) {
        type = FAT_DIR;
    } else { // ���Ǿ���ļ���Ŀ¼�ļ���һ�ɵ�����ͨ�ļ�
        type = FAT_FILE;
    }

    return type;
}

/**
 * ��ȡһ��Ŀ¼���¼�ĵ��ļ�����ʼ�غš�
 * ԭ������һ��Ŀ¼�ʹ��Ŀ¼��ĸߵ�16λ��¼�������Ǹ��ļ�����ʼ��
 * @param item
 * @return
 */
static u32_t get_diritem_cluster (diritem_t * item) {
    return (item->DIR_FstClusHI << 16) | item->DIR_FstClusL0; 
    // �غ��ɸߵ�16λ��ͻ�ã��ȼ������ｫDIR_FstClusHI << 16֮���DIR_FstClusL0�������㣻
}



 /**
  * �ڸ�����xfat������ĳһ��Ŀ¼�ļ��ģ�initial_sector������initial_offset���أ�λ�ÿ�ʼѰ��path·���������ĵ�ǰһ�����ļ�
  * ����������path = "/abc/def/123.txt",�򱾴κ���ִ��Ŀ��Ϊ�ڵ�ǰdir�ҵ�abc��һ���ļ���Ӧ��Ŀ¼����أ���
  * �ҵ�֮�󣬴���r_diritem���أ����Ҽ�¼����ʱ��������ƫ��move_bytes�������ܿ��0~n���أ������أ����ƫ����ô�㣿��
  * 1.���pathΪ�գ�ֱ�ӷ��ص�ǰĿ¼�ļ��ģ�initial_sector������initial_offset���أ�λ�õ�Ŀ¼�
  * 2.���������ĳһ�ص�ĳһ������ĳһĿ¼���ҵ��ˣ��򷵻�ָ�����Ŀ¼��ָ���ָ�롢���ڴء�����ƫ�ơ�move_bytes�ȶ�λ�����ݣ�
  * 3.�����������Ŀ¼�ļ�û�ҵ����򷵻�ERR_EOF��
  * 
  * @param xfat ����ģ�xfat�ṹ
  * @param dir_cluster ����ģ�Ҫ���ҵ�file���ڵ�Ŀ¼�������ڵĴغţ�������Ŀ¼�ļ��е�ĳһ�أ�������һ�ؿ�ʼ���ɨ��Ŀ¼��
  * @param cluster_offset ����ģ����е�ƫ�ƣ���λB�����Ӵ��е����Ŀ¼����ſ�ʼѰ��
  * @param move_bytes ���صģ��ҵ���Ŀ��Ŀ¼�������������ʼλ�õ�ƫ�ƣ���λB������������û�ã���ÿ���һ�������ϵ�һ��Ŀ¼��ͻ��ۼ�һ�Σ�����ǣ�ע������ۼ���Ϊ�������������أ����Բ����Ǽ�¼��һ���ڵ�ƫ�ƣ�
  * @param path ����ģ���Ե�ǰĿ¼��·���������� ������path = "/abc/def/123.txt",�򱾴κ���ִ��Ŀ��Ϊ�ڵ�ǰdir�ҵ�abc��һ��Ŀ¼�ļ���Ӧ��Ŀ¼����أ�
  * @param r_diritem ���صģ����ҵ���diritem*ָ���ָ�룬����û�����Ķ���
  * @return 
  */
static xfat_err_t locate_file_dir_item(xfat_t *xfat, u32_t *dir_cluster, u32_t *cluster_offset,
                                    const char *path, u32_t *move_bytes, diritem_t **r_diritem) {
    u32_t curr_cluster = *dir_cluster;// ��ǰ��dir_cluster����ؿ�ʼ
    xdisk_t * xdisk = xfat_get_disk(xfat);// ��ȡ��ǰ���ڵ�disk

    // ʹ��cluster_offset������������ֵ��������������ƫ�ƣ�����Ϊ֮�����Ŀ¼��ʱ����һ�ζ�ȡһ�������������������ģ�����Ҫ��ȡ��������λ��
    u32_t initial_sector = to_sector(xdisk, *cluster_offset);   // ����ڴؿ�ͷ�������ţ���ʵ����cluster_offset��������ֽ�ƫ�ƶ�Ӧ�Ĵ�������ƫ�ƣ�
                                                                // ���㣺��ƫ��cluster_offset��disk��������С���㣻
    u32_t initial_offset = to_sector_offset(xdisk, *cluster_offset);    // ʹ�ô���ƫ�Ƽ��������������������е�ƫ�ƣ���λB������������Եõ���initial_sector������initial_offset���أ���λ��һ�����е�һ������λ��
    
    u32_t r_move_bytes = 0;// ѭ������Ŀ¼�ļ���ͬ��ʱ��¼��ƫ�ƣ���󷵻ظ�move_bytes�ã�������������

    // cluster
    do {
        u32_t i;
        xfat_err_t err;
        u32_t start_sector = cluster_fist_sector(xfat, curr_cluster); // curr_cluster �Ŀ�ʼ����

        
        // �������ӣ�initial_sector������initial_offset���أ�λ�ÿ�ʼ������curr_cluster����������Ŀ¼�ļ�
        // ��Ȼ���ӵڶ��ο�ʼ��Ӧ��������������ʼλ�þ��ǣ���һ������initial_sector = 0��initial_offset = 0����
        for (i = initial_sector; i < xfat->sec_per_cluster; i++) { // ��initial_sector���������ʼ��������������ֱ��һ���ؽ�β��
            u32_t j;

            err = xdisk_read_sector(xdisk, temp_buffer, start_sector + i, 1);// ��ȡһ������������
            if (err < 0) {
                return err;
            }

            for (j = initial_offset / sizeof(diritem_t); j < xdisk->sector_size / sizeof(diritem_t); j++) { // ��initial_offset��Ӧ���Ǹ�Ŀ¼�ʼ����������Ŀ¼��ֱ������������β
                diritem_t *dir_item = ((diritem_t *) temp_buffer) + j;// ��ת��Ϊdiritem_t����ָ�룬��+j��ʾƫ��jλ

                // ��ǰĿ¼����Ч���������
                if (dir_item->DIR_Name[0] == DIRITEM_NAME_END) {
                    return FS_ERR_EOF;
                } else if (dir_item->DIR_Name[0] == DIRITEM_NAME_FREE) {// ����ǰ��
                    r_move_bytes += sizeof(diritem_t); 
                    continue;
                }

                if ((path == (const char *) 0)
                    || (*path == 0)
                    || is_filename_match((const char *) dir_item->DIR_Name, path)) {    // ·��Ϊ�գ���Ϊ���������ı�path������·��Ϊ�յ�������ڵ�һ�ν�������ʱ�����񣩣����������ƥ�䣨���������������ֹͣ�ˣ�
                                                                                        // ��·���ǿգ����ڵ�һ������ʱ�����ﲶ��Ȼ��ִ����ֹ��
                                                                                        // ֱ�ӷ��ص�ǰĿ¼

                    u32_t total_offset = i * xdisk->sector_size + j * sizeof(diritem_t); // �����������ʼ����ƫ�� = ��ǰ������ƫ�� + ��ǰĿ¼���ڵ�ǰ�����е�ƫ�ƣ�
                    
                    *dir_cluster = curr_cluster; // Ŀ���ļ���Ŀ¼�����ڵĴ�
                    *cluster_offset = total_offset;// ���ڴ��ϵľ���ƫ�ƣ��ֽڣ�

                    *move_bytes = r_move_bytes + sizeof(diritem_t); // ƥ���ˣ���move_bytes����֮ǰ�ۼƵ�r_move_bytes����һ��Ŀ¼��Ĵ�С
                                                                    // ����Ҫר�ż�һ��Ŀ¼���С�����岻������ΪĿǰopen_sub_file��û��ʹ��moved_bytes�������
                    
                    if (r_diritem) {    // ������r_diritem��һ��ָ���ָ�룩��Ϊ�գ�����������ָ��ָ����ҽ��dir_item
                                        // ������ֻ֮Ҫ�û���ʵ������һ��dir_item*�������Ҿͷ��أ�
                                        // ����������һ��������0�������������ͺ�Σ�գ������Ե�ַ0��ֵ��������һ�ַ����Ա�̰ɣ� 
                        *r_diritem = dir_item; 
                    }

                    return FS_ERR_OK;
                }

                r_move_bytes += sizeof(diritem_t);
            }
        }

        // ǰ����һ��
        // ��һ��ѭ������(��һ�أ�0������0ƫ��)��ʼɨ��
        err = get_next_cluster(xfat, curr_cluster, &curr_cluster);
        if (err < 0) {
            return err;
        }

        initial_sector = 0;
        initial_offset = 0;
    }while (is_cluster_valid(curr_cluster));

    return FS_ERR_EOF; // ��������Ŀ¼�ļ�����û���ҵ�����path����һ�������ش�����
}

/**
 * ����һ��Ŀ¼�ļ��Ĵص�ַdir_cluster���Դ�Ŀ¼�ļ���Ϊ���ڵ㣬"�������"��ѯpath��ָ����Ǹ�Ŀ���ļ���
 * ���Ǹ��ļ��Ľ����������file���ء����pathΪ�գ��򷵻ص�file������ǵ�ǰdir_cluster����Ŀ¼�ļ�����
 * ԭ����dir_clusterָ��ĵ�ǰĿ¼�ļ��ҵ���һ����Ȼ����ǰ����һ��������һ��...ֱ��·�����ꣻ�����м�ĳһ�������ڴ�Ŀ¼�Ҳ���Ŀ�����һ����������������᷵��FS_ERR_NONE����
 * @param xfat xfat�ṹ
 * @param dir_cluster Ŀ¼����ʼ�����������������ŵ��Ǹ�Ŀ¼�ļ�Ϊ���ڵ㣬���²���Ŀ���ļ�
 * @param file ���ص��ļ�file�ṹ�����ҵ�Ŀ���ļ��󣬽�����д�����pathΪ�գ�����д���Ǹ�Ŀ¼����ļ������ݣ�������Ĭ�Ϲ��캯����
 * @param path ��dir_cluster����Ӧ��Ŀ¼Ϊ��������·��
 * @return
 */
static xfat_err_t open_sub_file (xfat_t * xfat, u32_t dir_cluster, xfile_t * file, const char * path) {
    u32_t parent_cluster = dir_cluster;
    u32_t parent_cluster_offset = 0; // һ���е�ĳһ�һ��ƫ�ƣ�����ʱ��������

    path = skip_first_path_sep(path);// ����path��ͷ�ķָ���'\\'��'/'

    // ������������˷ָ���֮������������ݣ����������²��ļ���
    // ���Ϊ�գ���˵��ԭ·��ֻ��һ��/�����ʾ���Ǹ�Ŀ¼�����Է��ظ�Ŀ¼���file
    if ((path != 0) && (*path != '\0')) { // ָ��ָ��0��null���������𣿡�����������������������������������������������������������
        diritem_t * dir_item = (diritem_t *)0;// ��ʼ��ָ�룬ָ���ַ0
        u32_t file_start_cluster = 0; 
        const char * curr_path = path;

        // ѭ�����룬ֱ���ҵ�pathָ���Ŀ���ļ�����ʼ��
        // �������У�ÿһ��ѭ������²�������һ��ѭ���Ĳ�����parent_cluster��curr_path
        while (curr_path != (const char *)0) {
            u32_t moved_bytes = 0;// ����һ��Ŀ¼�ļ����ҵ���Ŀ���ļ����parent_cluster_offset��ƫ�ƣ���λ�ֽڣ�����ʱ��������
            dir_item = (diritem_t *)0;
            
            // �ڸ�Ŀ¼�²���ָ��·����Ӧ���ļ�����������ƫ��ֵ���ڴ������0�������Ժ�����
            // ע�������dir_item��һ��ʵ�ʴ��ڵĶ����������������ѯ��֮�󽫻�ѽ�����ظ�dir_item���ָ�룻
            xfat_err_t err = locate_file_dir_item(xfat, &parent_cluster, &parent_cluster_offset,
                                                curr_path, &moved_bytes, &dir_item);

            // �����쳣�Ĵ���

            if (err < 0) { // ����������������г�������Ĵ��󣨶�ȡ֮����������ش��󣬵��º����쳣������
                return err;
            }
            
            if (dir_item == (diritem_t *)0) {   // �������������������parent_cluster���Ŀ¼�ļ����дأ���û�в��ҵ�curr_pathָ�����һ���ļ�/Ŀ¼��
                                                // ����FS_ERR_EOF��= 1������ʱdir_itemû����locate_file_dir_itemִ�к�õ�Ŀ��item��ָ�������ﲶ���������
                                                // Ȼ������жϣ����ټ������ִ��
                return FS_ERR_NONE;
            }

            // һ���������ҵ���Ŀ���dir_item���ʼ�ж��Ƿ񵽴�Ŀ���ε�Ŀ���ļ������û�У�����Ҫǰ����һ����

            curr_path = get_child_path(curr_path);  // ����path�е������Ŀ¼���֡����൱�ڽ�����ָ�����һ��Ŀ¼�У����е����п����Ѿ�ָ��Ŀ���ļ��ˣ���ʱ�⺯���᷵��(const char *)0

            // ����curr_path��ָ�򣬾����Ƿ�Ҫ��������·����Ѱ��Ŀ���ļ�
            if (curr_path != (const char *)0) {
                // ��ǰcurr_path != 0��˵��Ŀ���ļ����ڸ���ĵط�����Ҫ������һ��ѭ��ǰ��
                parent_cluster = get_diritem_cluster(dir_item);// ��ñ���ѭ���ҵ����ļ���λ�ã��غţ��������ǰ��Ŀ¼������һ��ѭ�����������Ϊdir��
                parent_cluster_offset = 0; // ƫ��������Ϊ0������ʱ��������
            } else {
                // ��ǰcurr_path = 0��˵��dir_item�Ѿ�ָ���˼�¼��Ŀ���ļ���Ŀ¼��
                // ��ȡĿ���ļ���ʼ��
                file_start_cluster = get_diritem_cluster(dir_item);
                // ������ȫ����break��
            }
        }

        file->size = dir_item->DIR_FileSize;        // ��ȡ��С
        file->type = get_file_type(dir_item);       // ��ȡ��ǰ�ļ������ͣ��Զ���������ļ����ͣ�
        file->start_cluster = file_start_cluster;   // ���ļ�����ʼ��
        file->curr_cluster = file_start_cluster;    // ��ǰ�أ���ʱ����ָ����ʼ��
    } else {
        file->size = 0;
        file->type = FAT_DIR;
        file->start_cluster = parent_cluster;
        file->curr_cluster = parent_cluster;
    }

    // ������һЩ���ԣ�
    file->xfat = xfat; 
    file->pos = 0;
    file->err = FS_ERR_OK;
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
    return open_sub_file(xfat, xfat->root_cluster, file, path); // ���ݴ���ĸ�Ŀ¼�أ�path����������ҵ�Ŀ���ļ���Ŀ¼����ҽ���֮��õ��ļ�����file����
}

/**
 * �ر��Ѿ��򿪵��ļ�
 * @param file ���رյ��ļ�
 * @return
 */
xfat_err_t xfile_close(xfile_t *file) {
    return FS_ERR_OK;
}
