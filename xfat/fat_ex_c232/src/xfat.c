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

#define is_path_sep(ch)         (((ch) == '\\') || ((ch == '/')))       // 判断是否是文件名分隔符
#define xfat_get_disk(xfat)     ((xfat)->disk_part->disk)               // 获取disk结构
#define to_sector(disk, offset)     ((offset) / (disk)->sector_size)    // 将依稀转换为扇区号
#define to_sector_offset(disk, offset)   ((offset) % (disk)->sector_size)   // 获取扇区中的相对偏移

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
 * @param my_name 要转换的文件名
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
 * 获取子路径：如输入"/abc/def/456.txt"，则返回”def/456.txt“
 * @param dir_path 包含上一级名称的路径
 * @return 
 */
const char * get_child_path(const char *dir_path) {

    // 举例：此时为 "/abc/456.txt"
    const char * c = skip_first_path_sep(dir_path);

    // 举例：此时为 "abc/456.txt"
    // 跳过父目录，不含父目录后面的分隔符
    while ((*c != '\0') && !is_path_sep(*c)) {
        c++; 
    }

    // 举例：此时为 "/456.txt",则返回"456.txt"；如果已经啥都没了，则返回指向空的指针；
    return (*c == '\0') ? (const char *)0 : c + 1;
}

/**
 * 获取目录项所指文件的类型（自定义的3种文件类型，简化了复杂的分类情况）
 * @param diritem 需解析的diritem
 * @return
 */
static xfile_type_t get_file_type(const diritem_t *diritem) {
    xfile_type_t type;

    if (diritem->DIR_Attr & DIRITEM_ATTR_VOLUME_ID) {
        type = FAT_VOL;
    } else if (diritem->DIR_Attr & DIRITEM_ATTR_DIRECTORY) {
        type = FAT_DIR;
    } else { // 不是卷标文件、目录文件，一律当作普通文件
        type = FAT_FILE;
    }

    return type;
}

/**
 * 获取一个目录项记录的的文件的起始簇号
 * 原理：传入一个目录项，使用目录项的高低16位记录，计算那个文件的起始簇
 * @param item
 * @return
 */
static u32_t get_diritem_cluster (diritem_t * item) {
    return (item->DIR_FstClusHI << 16) | item->DIR_FstClusL0; 
    // 簇号由高低16位求和获得，等价于这里将DIR_FstClusHI << 16之后和DIR_FstClusL0做或运算；
}

/**
 在给定的xfat分区的某一个目录文件的（initial_sector扇区，initial_offset比特）位置开始寻找path路径中描述的子文件，
 找到之后，存入r_diritem返回，并且记录查找时所经过的偏移move_bytes（其间可能跨过0~n个簇，如果跨簇，这个偏移怎么搞？）
    如果path为空，直接返回当前目录文件的（initial_sector扇区，initial_offset比特）位置的目录项；
    如果没找到？？？？？？？？？——————————————————
 要一个扇区一个扇区的读取，查找
 * xfat 传入的，xfat结构
 查找起始位置：（簇，簇中偏移）：
 * dir_cluster 传入的，要查找的file所在的目录数据所在的簇号（可能是目录文件中的某一簇），从这一簇开始向后扫描目录项
 * cluster_offset 簇中的偏移（单位B），从簇中的这个目录项序号开始寻找
 找到的结果：
 * move_bytes 返回的，找到的目标目录项，相对于上面的起始位置的偏移（单位B）；（目前不清楚是否仅仅是最终找到目录文件时，才记录这一项）
 * path 传入的，文件或目录的完整路径————处理时要截断前面的吗？？？——————————————————————？？？？？？？？
 * r_diritem 返回的，查找到的diritem项，如果指向为0，说明路径不对，当前这一级根本没有这个文件或者目录文件
 */
static xfat_err_t locate_file_dir_item(xfat_t *xfat, u32_t *dir_cluster, u32_t *cluster_offset,
                                    const char *path, u32_t *move_bytes, diritem_t **r_diritem) {
    u32_t curr_cluster = *dir_cluster;// 当前从dir_cluster这个簇开始
    xdisk_t * xdisk = xfat_get_disk(xfat);// 获取当前所在的disk
    u32_t initial_sector = to_sector(xdisk, *cluster_offset);   // 相对于簇开头的扇区号，其实就是cluster_offset这个簇中字节偏移对应的簇中扇区偏移；
                                                                // 计算：用偏移cluster_offset和disk的扇区大小计算；
    u32_t initial_offset = to_sector_offset(xdisk, *cluster_offset);    // 上面这个扇区中的偏移，单位B。
    u32_t r_move_bytes = 0;// 循环遍历目录文件不同簇时记录的偏移，最后返回给move_bytes用？？？？？？？

    // cluster
    do {
        u32_t i;
        xfat_err_t err;
        u32_t start_sector = cluster_fist_sector(xfat, curr_cluster); // curr_cluster 的开始扇区

        
        // 接下来从（initial_sector扇区，initial_offset比特）位置开始遍历该curr_cluster所处的整个目录文件
        // 当然，从第二次开始（应该是这样），开始位置就是（下一扇区，initial_sector = 0，initial_offset = 0）；
        for (i = initial_sector; i < xfat->sec_per_cluster; i++) { // 从initial_sector这个扇区开始，遍历所有扇区直到一个簇结尾，
            u32_t j;

            err = xdisk_read_sector(xdisk, temp_buffer, start_sector + i, 1);// 读取一个扇区到缓存
            if (err < 0) {
                return err;
            }

            for (j = initial_offset / sizeof(diritem_t); j < xdisk->sector_size / sizeof(diritem_t); j++) { // 从initial_offset对应的那个目录项开始，遍历所有目录项直到整个扇区结尾
                diritem_t *dir_item = ((diritem_t *) temp_buffer) + j;// 先转换为diritem_t类型指针，再+j表示偏移j位

                // 当前目录项无效的两种情况
                if (dir_item->DIR_Name[0] == DIRITEM_NAME_END) {
                    return FS_ERR_EOF;
                } else if (dir_item->DIR_Name[0] == DIRITEM_NAME_FREE) {// 继续前进
                    r_move_bytes += sizeof(diritem_t); 
                    continue;
                }

                if ((path == (const char *) 0)
                    || (*path == 0)
                    || is_filename_match((const char *) dir_item->DIR_Name, path)) {    // 路径为空，或者完成了匹配（这两种情况都可以停止了）
                                                                                        // ————————什么时候路径会为空？？？难道不应该要么匹配要么路径不太对？？？
                                                                                        // 直接返回当前目录

                    u32_t total_offset = i * xdisk->sector_size + j * sizeof(diritem_t); // 相对于扇区开始的总偏移 = 当前扇区的偏移 + 当前目录项在当前扇区中的偏移；
                    *dir_cluster = curr_cluster;
                    *move_bytes = r_move_bytes + sizeof(diritem_t); // 匹配了，则move_bytes等于之前累计的r_move_bytes加上一个目录项的大小
                                                                    // ——————————————是当前的这个目录项吗？要寻找的不就是它？为什么还要累加？？
                    *cluster_offset = total_offset;
                    if (r_diritem) { // 如果调用者传入的是一个有效的指针，则这里将结果赋给这个指针，让调用者使用找到的目录项；——————————————？？？？这个判断意义何在？？？？？
                        *r_diritem = dir_item;
                    }

                    return FS_ERR_OK;
                }

                r_move_bytes += sizeof(diritem_t);
            }
        }

        // 前往下一簇
        // 下一次循环，从(下一簇，0扇区，0偏移)开始扫描
        err = get_next_cluster(xfat, curr_cluster, &curr_cluster);
        if (err < 0) {
            return err;
        }

        initial_sector = 0;
        initial_offset = 0;
    }while (is_cluster_valid(curr_cluster));

    return FS_ERR_EOF; // 遍历完了目录文件，都没有找到符合path的下一级，返回错误码
}

/**
 * 传入一个目录文件的簇地址dir_cluster，以此目录文件作为根节点，层层向下查询path所指向的那个目标文件，将那个文件的解析结果存入file返回。
 * 如果path为空，则返回的file代表的是当前dir_cluster所在目录文件本身。
 * 原理：
 * @param xfat xfat结构
 * @param dir_cluster 目录的起始簇链，从这个簇所存放的那个目录文件为根节点，向下查找目标文件
 * @param file 返回的文件file结构，当找到目标文件后，将会填写；如果path为空，则填写的是根目录这个文件的内容；（类似默认构造函数）
 * @param path 以dir_cluster所对应的目录为起点的完整路径
 * @return
 */
static xfat_err_t open_sub_file (xfat_t * xfat, u32_t dir_cluster, xfile_t * file, const char * path) {
    u32_t parent_cluster = dir_cluster;
    u32_t parent_cluster_offset = 0; // 一簇中的某一项，一种偏移，本课时中无意义

    path = skip_first_path_sep(path);// 跳过path开头的分隔符'\\'或'/'

    // 如果上面跳过了分隔符之后后续还有内容，则正常打开下层文件；
    // 如果为空，则说明原路径只有一个/，则表示的是根目录，所以返回根目录这个file
    if ((path != 0) && (*path != '\0')) { // 指针指向0和null，有区别吗？——————————————————————————————
        diritem_t * dir_item = (diritem_t *)0;// 初始化指针，指向地址0
        u32_t file_start_cluster = 0;
        const char * curr_path = path;

       // 循环深入，直到找到path指向的目标文件的起始簇
        while (curr_path != (const char *)0) {
            u32_t moved_bytes = 0;// 在这一簇目录文件中找到的目标文件相对parent_cluster_offset的偏移，单位字节，本课时中无意义
            dir_item = (diritem_t *)0;

            // 在父目录下查找指定路径对应的文件—————
            xfat_err_t err = locate_file_dir_item(xfat, &parent_cluster, &parent_cluster_offset,
                                                curr_path, &moved_bytes, &dir_item);
            if (err < 0) {
                return err;
            }
             
            if (dir_item == (diritem_t *)0) { // 找到的item指向位0，表示没有找到文件。（说明路径不对，根本没有找到这个文件或者目录文件）
                return FS_ERR_NONE;
            }

            curr_path = get_child_path(curr_path);  // 剥离path中的最外层目录部分——相当于进到其指向的下一层目录中，此时有可能已经指向文件了，这时这一行函数会返回地址0

            // 根据curr_path的指向，决定是否要继续循环
            if (curr_path != (const char *)0) {
                // 当前curr_path != 0，说明目标文件还在更深的地方，需要继续下一次循环前进
                parent_cluster = get_diritem_cluster(dir_item);// 下一次循环的父目录文件就是当前的dir_item
                parent_cluster_offset = 0; // 偏移重新置为0，本课时中无意义
            } else {
                // 当前curr_path = 0，说明dir_item已经指向了记录着目标文件的目录项
                // 使用这个目录项获取文件起始簇
                file_start_cluster = get_diritem_cluster(dir_item);
                // 这里完全可以break了吧————————————————————————？？？？？？？
            }
        }

        file->size = dir_item->DIR_FileSize; // 获取大小
        file->type = get_file_type(dir_item);// 获取当前文件的类型，传入其目录项指针，返回类型？？？是attr？？？？？
        file->start_cluster = file_start_cluster;// 该文件的起始簇
        file->curr_cluster = file_start_cluster;// 当前簇，此时让其指向起始簇
    } else {
        file->size = 0;
        file->type = FAT_DIR;
        file->start_cluster = parent_cluster;
        file->curr_cluster = parent_cluster;
    }

    // 其他的一些属性：
    file->xfat = xfat; 
    file->pos = 0;
    file->err = FS_ERR_OK;
    return FS_ERR_OK;
}

/**
 * 打开指定的文件或目录
 * @param xfat xfat结构
 * @param file 打开的文件或目录
 * @param path 文件或目录所在的完整路径
 * @return
 */
xfat_err_t xfile_open(xfat_t * xfat, xfile_t * file, const char * path) {
    return open_sub_file(xfat, xfat->root_cluster, file, path);
}

/**
 * 关闭已经打开的文件
 * @param file 待关闭的文件
 * @return
 */
xfat_err_t xfile_close(xfile_t *file) {
    return FS_ERR_OK;
}
