/**
 * 本源码配套的课程为 - 从0到1动手写FAT32文件系统。每个例程对应一个课时，尽可能注释。
 * 作者：李述铜
 * 课程网址：http://01ketang.cc
 * 版权声明：本源码非开源，二次开发，或其它商用前请联系作者。
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "xfat.h"
#include "xdisk.h"

extern u8_t temp_buffer[512];      // todo: 缓存优化

// 内置的.和..文件名                    "12345678ext"
#define DOT_FILE_NAME                ".          "
#define DOT_DOT_FILE_NAME            "..         "

#define is_path_sep(ch)         (((ch) == '\\') || ((ch == '/')))       // 判断是否是文件名分隔符
#define xfat_get_disk(xfat)     ((xfat)->disk_part->disk)               // 获取disk结构
#define to_sector(disk, offset)     ((offset) / (disk)->sector_size)    // 将依稀转换为扇区号
#define to_sector_offset(disk, offset)   ((offset) % (disk)->sector_size)   // 获取扇区中的相对偏移
#define to_cluster_offset(xfat, pos)      ((pos) % ((xfat)->cluster_byte_size)) // 获取簇中的相对偏移
#define to_cluster_number(xfat, pos, cur_cluster)    ()

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
    err = xdisk_read_sector(xdisk, (u8_t *)xfat->fat_buffer, xfat->fat_start_sector, xfat->fat_tbl_sectors);
    if (err < 0) {
        return err;
    }

    return FS_ERR_OK;
}

/**
 * 获取指定簇号的第一个扇区编号
 * @param xfat xfat结构
 * @param cluster_no  簇号
 * @return 扇区号
 */
u32_t cluster_fist_sector(xfat_t *xfat, u32_t cluster_no) {
    u32_t data_start_sector = xfat->fat_start_sector + xfat->fat_tbl_sectors * xfat->fat_tbl_nr;
    return data_start_sector + (cluster_no - 2) * xfat->sec_per_cluster;    // 前两个簇号保留
}

/**
 * 检查指定簇是否可用，非占用或坏簇
 * @param cluster 待检查的簇
 * @return
 */
int is_cluster_valid(u32_t cluster) {
    cluster &= 0x0FFFFFFF;
    return (cluster < 0x0FFFFFF0) && (cluster >= 0x2);     // 值是否正确
}

/**
 * 获取指定簇的下一个簇
 * @param xfat xfat结构
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
 * 读取一个簇的内容到指定缓冲区
 * @param xfat xfat结构
 * @param buffer 数据存储的缓冲区
 * @param cluster 读取的起始簇号
 * @param count 读取的簇数量
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
 * 将指定的name按FAT 8+3命名转换
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

    // 跳过开头的分隔符
    while (is_path_sep(*my_name)) {
        my_name++;
    }

    // 找到第一个斜杠之前的字符串，将ext_dot定位到那里，且记录有效长度
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

    // 如果文件名以.结尾，意思就是没有扩展名？
    // todo: 长文件名处理?
    ext_existed = (ext_dot > my_name) && (ext_dot < (my_name + name_len - 1));

    // 遍历名称，逐个复制字符, 算上.分隔符，最长12字节，如果分离符，则只应有
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
 * 判断两个文件名是否匹配
 * @param name_in_item fatdir中的文件名格式
 * @param my_name 应用可读的文件名格式
 * @return
 */
static u8_t is_filename_match(const char *name_in_dir, const char *to_find_name) {
    char temp_name[SFN_LEN];

    // FAT文件名的比较检测等，全部转换成大写比较
    // 根据目录的大小写配置，将其转换成8+3名称，再进行逐字节比较
    // 但实际显示时，会根据diritem->NTRes进行大小写转换
    to_sfn(temp_name, to_find_name);
    return memcmp(temp_name, name_in_dir, SFN_LEN) == 0;
}

/**
 * 跳过开头的分隔符
 * @param path 目标路径
 * @return
 */
static const char * skip_first_path_sep (const char * path) {
    const char * c = path;

    // 跳过开头的分隔符
    while (is_path_sep(*c)) {
        c++;
    }
    return c;
}

/**
 * 获取子路径，如果子路径为结束符，则将返回地址0，否则返回指向子路径第一个字符的指针
 * @param dir_path 
 * @return
 */
const char * get_child_path(const char *dir_path) {
    const char * c = skip_first_path_sep(dir_path);

    // 跳过父目录
    while ((*c != '\0') && !is_path_sep(*c)) {
        c++;
    }

    return (*c == '\0') ? (const char *)0 : c + 1;
}

/**
 * 解析diritem，获取文件类型
 * @param diritem 需解析的diritem
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
 * 复制来自diritem中的Date、time等时间信息到xfile_time_t中
 * @param dest 指定存储的时间信息结构
 * @param date fat格式的日期
 * @param time fat格式的时间
 * @param mil_sec fat格式的10毫秒
 */
static void copy_date_time(xfile_time_t *dest, const diritem_date_t *date,
                           const diritem_time_t *time, const u8_t mil_sec) {
    // date和time两个指针都有可能是空
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
 * 从fat_dir格式的文件名中拷贝成用户可读的文件名到dest_name
 * @param dest_name 转换后的文件名存储缓冲区
 * @param raw_name fat_dir格式的文件名
 */
static void sfn_to_myname(char *dest_name, const diritem_t * diritem) {
    int i;
    char * dest = dest_name;
    char * raw_name = (char *)diritem->DIR_Name;

    // 判断是否有扩展名：
    u8_t ext_exist = raw_name[8] != 0x20; // diritem从DIR_Name开始的第八个字符（对应着DIR_ExtName[3]的第一个字符），如果不是空格，说明扩展名不为空(0x20)（也就是有扩展名）
    
    // 存放结果文件名的最长长度
    u8_t scan_len = ext_exist ? SFN_LEN + 1 : SFN_LEN; // 如果存在扩展名，则最长长度为11+1(包含一个.符)

    memset(dest_name, 0, X_FILEINFO_NAME_SIZE);   // 清空结果名称容器，不知道为什么清空的长度是32.
    
    // 要考虑大小写问题，根据NTRes配置转换成相应的大小写
    for (i = 0; i < scan_len; i++) {
        if (*raw_name == ' ') {// 如果遍历8+3文件名遇到了空格，则跳过，比如原文件名可能是"123   ABC"
            raw_name++;
        } else if ((i == 8) && ext_exist) { // 扫描到文件名结束,并且有扩展名，则在结果字符串的下一个位置上填入一个.扩展符
           *dest++ = '.';
        } else {
            // 是普通字符的情况，需要处理大小写问题
            u8_t lower = 0; // 默认大写

            if (((i < 8) && (diritem->DIR_NTRes & DIRITEM_NTRES_BODY_LOWER)) // DIRITEM_NTRES_BODY_LOWER表示文件名小写，值为00001000
                || ((i > 8) && (diritem->DIR_NTRes & DIRITEM_NTRES_EXT_LOWER))) { // 这个表示扩展名小写，值为
                lower = 1;
            }

            *dest++ = lower ? tolower(*raw_name++) : toupper(*raw_name++); // 以大小写形式输出
        }
    }

    *dest = '\0'; // 返回一个字符串格式的结果时，最好加上结束符，这样更好用；
}

/**
 * 获取diritem的文件起始簇号
 * @param item
 * @return
 */
static u32_t get_diritem_cluster (diritem_t * item) {
    return (item->DIR_FstClusHI << 16) | item->DIR_FstClusL0;
}

/**
 * 将dir_item中相应的文件信息转换存至fs_fileinfo_t中
 * @param info 信息存储的位置
 * @param dir_item fat的diritem
 */
static void copy_file_info(xfileinfo_t *info, const diritem_t * dir_item) {
    sfn_to_myname(info->file_name, dir_item); // dir_item所指的目录项的文件名，转普通应用层文件名。
    info->size = dir_item->DIR_FileSize;
    info->attr = dir_item->DIR_Attr;
    info->type = get_file_type(dir_item);

    // 创建、最近访问、修改时间
    copy_date_time(&info->create_time, &dir_item->DIR_CrtDate, &dir_item->DIR_CrtTime, dir_item->DIR_CrtTimeTeenth); 
    copy_date_time(&info->last_acctime, &dir_item->DIR_LastAccDate, (diritem_time_t *) 0, 0) ; //最近访问时间，fat只记录了Date。所以其他填0
    copy_date_time(&info->modify_time, &dir_item->DIR_WrtDate, &dir_item->DIR_WrtTime, 0); // 创建时间只记录了date、时分秒
}

/**
 * 检查文件名和类型是否匹配——————————对SYSTEM、HIDDEN、VOLUME_ID、LOCATE_DOT、LOCATE_NORMAL五种，LOCATE_ALL并没有操作（话说啊，0xff好像也不是所有之和A啊？？）
 * @param dir_item
 * @param locate_type
 * @return
 */
static u8_t is_locate_type_match (diritem_t * dir_item, u8_t locate_type) {
    u8_t match = 1;// 表示是否匹配，1表示dir_item和locate_type匹配（无需过滤）；

    if ((dir_item->DIR_Attr & DIRITEM_ATTR_SYSTEM) && !(locate_type & XFILE_LOCALE_SYSTEM)) {
        match = 0;  // 是系统文件，且用户没有传入XFILE_LOCALE_SYSTEM，——用户没说明要在locate_file_dir_item执行时，考虑系统文件；
        // 一般path是确定的，用不到这个函数吧————————其实是这样：有些时候locate_file_dir_item函数拿来遍历目录？？？！！！
    } else if ((dir_item->DIR_Attr & DIRITEM_ATTR_HIDDEN) && !(locate_type & XFILE_LOCATE_HIDDEN)) {
        match = 0;  // 不显示隐藏文件
    } else if ((dir_item->DIR_Attr & DIRITEM_ATTR_VOLUME_ID) && !(locate_type & XFILE_LOCATE_VOL)) {
        match = 0;  // 不显示卷标
    } else if ((memcmp(DOT_FILE_NAME /*也就是".          "*/ , dir_item->DIR_Name, SFN_LEN) == 0)
                || (memcmp(DOT_DOT_FILE_NAME /*也就是"..         "*/ , dir_item->DIR_Name, SFN_LEN) == 0)) {
        if (!(locate_type & XFILE_LOCATE_DOT)) {
            match = 0;// 不显示dot文件
        }
    } else if (!(locate_type & XFILE_LOCATE_NORMAL)) { // 普通文件都要过滤——则直接一律match = 0？？？？
        match = 0;
    }
    return match;
}

/**
 * 查找指定dir_item，并返回相应的结构
 * @param xfat xfat结构
 * @param locate_type 定位的item类型
 * @param dir_cluster dir_item所在的目录数据簇号
 * @param cluster_offset 簇中的偏移
 * @param move_bytes 查找到相应的item项后，相对于最开始传入的偏移值，移动了多少个字节才定位到该item
 * @param path 文件或目录的完整路径
 * @param r_diritem 查找到的diritem项
 * @return
 */
static xfat_err_t locate_file_dir_item(xfat_t *xfat, u8_t locate_type, u32_t *dir_cluster, u32_t *cluster_offset,// 改变————————————增加用户传入的类型locate_type；
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
                } 
                //// 各类type在这里统一处理——————————判断当前文件和用户传入的类型locate_type是否一致；
                //else if (!is_locate_type_match(dir_item, locate_type))  // 在匹配用户输入的path（下方代码）之前，判断类型，如果类型不对，根本匹配不到。
                //                                                        // ————————老师设计的本函数的职责是“根据path递归查找目标文件并注意过滤”
                //{ 
                //    r_move_bytes += sizeof(diritem_t);
                //    continue;
                //}

               
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
 * 打开指定dir_cluster开始的簇链中包含的子文件。
 * 如果path为空，则以dir_cluster创建一个打开的目录对像
 * @param xfat xfat结构
 * @param dir_cluster 查找的顶层目录的起始簇链
 * @param file 打开的文件file结构
 * @param path 以dir_cluster所对应的目录为起点的完整路径
 * @return
 */
static xfat_err_t open_sub_file (xfat_t * xfat, u32_t dir_cluster, xfile_t * file, const char * path) {
    u32_t parent_cluster = dir_cluster;
    u32_t parent_cluster_offset = 0;

    path = skip_first_path_sep(path);

    // 如果传入路径不为空，则查看子目录
    // 否则，直接认为dir_cluster指向的是一个目录，用于打开根目录
    if ((path != 0) && (*path != '\0')) {
        diritem_t * dir_item = (diritem_t *)0;
        u32_t file_start_cluster = 0;
        const char * curr_path = path;

       // 找到path对应的起始簇
        while (curr_path != (const char *)0) {
            u32_t moved_bytes = 0;
            dir_item = (diritem_t *)0;

            // 在父目录下查找指定路径对应的文件
            // ————在这里对用户输入的path.和..提供了支持！让其支持 /read/a/./b/..路径，能返回普通文件和dot文件的目录项diritem：
            xfat_err_t err = locate_file_dir_item(xfat, XFILE_LOCATE_DOT | XFILE_LOCATE_NORMAL, 
                    &parent_cluster, &parent_cluster_offset,curr_path, &moved_bytes, &dir_item);
            if (err < 0) {
                return err;
            }

            if (dir_item == (diritem_t *)0) {
                return FS_ERR_NONE;
            }

            curr_path = get_child_path(curr_path);
            if (curr_path != (const char *)0) 
                // path没有走完，将两个参数继承到下一次循环：parent_cluster、parent_cluster_offset                             //     -> 是 ——> 以dir_item解析file数据，返回
                // 问题：下面的所有参数都是根据dir_item的信息获取得到的，可否让locate_file_dir_item直接接受这个dir传入呢？          //    /
                // 答：不可，请观察本函数的执行：第一次循环xfat+dir_cluster+path——>locate_file_dir_item——>dir_item——>是否抵达目的文件？——> 否 ——> 以dir_item解析dir_cluster等数据，进入下一次循环
            {                                                                                                         
                // parent_cluster = get_diritem_cluster(dir_item); // 不是，这里怎么不照顾“当前..这个目录项的值是0”的情况？？？完全有可能中间某一级别是..啊？？？/read/../modify/ = /modify/
                if ((memcmp(dir_item->DIR_Name, DOT_DOT_FILE_NAME, SFN_LEN) == 0) && (file_start_cluster == 0)) {
                    parent_cluster = xfat->root_cluster;
                }
                else
                {
                    parent_cluster = get_diritem_cluster(dir_item);
                }
                
                parent_cluster_offset = 0;
            } 
            else 
                // path走完了，（当前diritem为..或.时前往其指向的方向）————在这里对用户输入的path.和..提供了支持！！————————等等；
            {
                file_start_cluster = get_diritem_cluster(dir_item);

                // 一个fat32的特殊情况：如果diritem是..且当前..这个目录项的值是0（说明当前在根目录），则重置当前位置为根目录文件开始簇   
                // 原因：
                // 在层层进入path中标注的”内层“目录时，可能会遇到..文件夹，这种情况其实不是进入内层，而是退回外层；
                // 而这个特殊情况还有一个特别糟糕的子情况——目录项指向..文件，指向的是根目录，这时向其目录项获取地址会直接返回0，而不是根目录的实际地址（这是因为不同的设备上根目录或许不同）。               
                if ((memcmp(dir_item->DIR_Name, DOT_DOT_FILE_NAME, SFN_LEN) == 0) && (file_start_cluster == 0)) {
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
 * 打开指定的文件或目录，因为要支持各种输入的path，所以原理是调用一个open_sub_file()函数，让其从根目录开始向下匹配path
 * @param xfat xfat结构
 * @param file 打开的文件或目录
 * @param path 文件或目录所在的完整路径，暂不支持相对路径
 * @return
 */
xfat_err_t xfile_open(xfat_t * xfat, xfile_t * file, const char * path) {
    // 检查开头的分隔符\\、/
	path = skip_first_path_sep(path);

	// 检查特殊情况：用户在根目录输入.或..文件（根目录没有这两个文件）——————没有对“/.txt”这种输入做处理，一棍子打死了
	if (memcmp(path, "..", 2) == 0) {// 根目录不存在上级目录————直接报错
		return FS_ERR_NONE;
	} else if (memcmp(path, ".", 1) == 0) {// 若含有.，直接忽略，跳过
		path++;
	}

    return open_sub_file(xfat, xfat->root_cluster, file, path);
}

/**
 * 返回指定目录下的第一个文件信息
 * @param file 已经打开的文件对象，就像一个句柄
 * @param info 第一个文件的文件信息
 * @return
 */
xfat_err_t xdir_first_file (xfile_t * file, xfileinfo_t * info) {
    // 思路：
    diritem_t * diritem = (diritem_t *)0;
    xfat_err_t err;
    u32_t moved_bytes = 0;
    u32_t cluster_offset;

    // 目前只支持对dir文件搜索下层文件，所以这里要判断
    if (file->type != FAT_DIR) {
        return FS_ERR_PARAM;
    }

    // 设定搜索位置，从(start_cluster,cluster_offset=0)开始
    file->curr_cluster = file->start_cluster;
    cluster_offset = 0;
    

    file->pos = 0;// 传入，让其记录遍历到的位置，并始终指向下一个位置
    
    // 传入path参数为空，将会返回第一个查找到的dir；
    err = locate_file_dir_item(file->xfat, XFILE_LOCATE_NORMAL,
            &file->curr_cluster, &cluster_offset, "", &moved_bytes, &diritem);  // moved_bytes用来更新pos（所以pos指的是连续遍历多个簇的偏移之和？？）
                                                                                // 此函数将会更新cur_cluster值
    // 处理可能的错误：
    // locate_file_dir_item两种可能的错误情况，见其函数说明。
    if (err < 0 || FS_ERR_EOF == err ) {
        return err;
    }

    // 更新pos，之后调用findnext时，将会使用pos得到cluster_offset用于继续向后遍历;  // [注意，pos代表的是簇上的偏移，而不是硬盘上起始簇到当前簇的直线距离，所以不能用pos/sizeofCluster 计算得到cur_cluster]
                                                                            // [cur_cluster的更新在locate_file_dir_item中完成，因为这个函数会一次查fat表找到目标目录项] 
    file->pos += moved_bytes;

    // 找到后，拷贝文件信息
    copy_file_info(info, diritem); // 用diritem生成一个文件信息对象，作为结果返回
    return err;
}

/**
 * 返回指定目录接下来的文件（用于文件遍历)
 * 特殊情况：1如果接下来没有文件了，则返回FS_ERR_EOF；2如果传入的file不是目录文件，返回FS_ERR_PARAM；
 * @param file 已经打开的目录
 * @param info 获得的文件信息
 * @return
 */
xfat_err_t xdir_next_file (xfile_t * file, xfileinfo_t * info) {
    // 原理：在当前file所指的目录中，从file中存储的(xfat, pos)所指向的位置开始，向后阅读这个目录文件，得到下一个文件的记录。
    // 另外还有职责：更新当前file(目录文件)遍历到的位置(curr_cluster,cluster_offset)，让用户如果某个时间想要继续调用本函数，将正确向后遍历

    xfat_err_t err;
    diritem_t * dir_item = (diritem_t *)0;
    u32_t moved_bytes = 0;
    u32_t cluster_offset;

    // 仅用于目录
    if (file->type != FAT_DIR) {
        return FS_ERR_PARAM;
    }

    // 搜索文件或目录
    cluster_offset = to_cluster_offset(file->xfat, file->pos);// pos是整个目录文件中的偏移，跨越0~n个簇
                                                              //（32位无符号数最大表示4GB，在fat32中表示一个文件内的偏移/大小足够了，但是为了兼容大型硬盘和一些特殊情况，还是决定全部使用64位数字）；
    
    err = locate_file_dir_item(file->xfat, XFILE_LOCATE_NORMAL,
            &file->curr_cluster, &cluster_offset, "", &moved_bytes, &dir_item);
    // 处理可能的错误：
    // locate_file_dir_item两种可能的错误情况，见其函数说明。
    if (err < 0 || FS_ERR_EOF == err) {
        return err;
    }

    // 查找成功，尝试更新file对象的pos和curr_cluster这两个字段；
    // 更新pos
    file->pos += moved_bytes;

    // 根据上面返回的cluster_offset更新下一次遍历开始位置（curr_cluster，cluster_offset），
    // 这里做的事情是检查是否要更新簇：如果再向后读取一目录项就越出当前簇了，则前往下一个簇
    if (cluster_offset + sizeof(diritem_t) >= file->xfat->cluster_byte_size) {
        err = get_next_cluster(file->xfat, file->curr_cluster, &file->curr_cluster);
        if (err < 0) {
            return err;
        }
    }

    // 2.更新cluster_offset，也就是更新pos（始终指向下一个目录项，我觉得不如干脆叫next_pos）
    file->pos += moved_bytes;
    
    return err;
}

/**
 * 关闭已经打开的文件
 * @param file 待关闭的文件
 * @return
 */
xfat_err_t xfile_close(xfile_t *file) {
    return FS_ERR_OK;
}
