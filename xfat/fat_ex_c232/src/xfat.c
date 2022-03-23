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
 * 原理：首先遍历输入的文件名，确认是否有后缀（.符在第二个字符~倒数第二个字符出现），如果属于“无后缀”（如.和..文件），则直接大写
 * @param dest_name 转换获得的8+3文件名
 * @param my_name 要转换的文件名，可以是一个路径，如：传入的是/a/b/c.txt,则将会处理a这个文件
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
    // 跳过开头的分隔符（如果存在的话）
    while (is_path_sep(*my_name)) {
        my_name++;
    }
    // a/b/c.txt

    // 找到第一个斜杠之前的字符串，将ext_dot定位到那里，且记录有效长度
    ext_dot = my_name;
    p = my_name;
    name_len = 0;
    // 遍历当前的这个文件名（从当前文件名的第一个字符向后遍历到当前文件们结束————也就是(*p != '\0') && !is_path_sep(*p)为假时）
    while ((*p != '\0') && !is_path_sep(*p)) {
        if (*p == '.') {// 如果发现了'.'，说明找到了代表后缀名的点
            ext_dot = p;
        }
        p++;
        name_len++;
    }

    // 注：这个是否存在扩展的判断，要求文件名不可以'.'号开头或结尾！！！！————这个是否符合规则，还不知道！——————————
    // todo: 长文件名处理？
    ext_existed = (ext_dot > my_name) && (ext_dot < (my_name + name_len - 1));  // 如果ext_dot指向的地方在名字第一个字符之后（文件名非空！），并且在最后一个字符之前（后缀名非空！），
                                                                                // 这说明有扩展名；注意，这里ext_dot、my_name是指针之间的比较；

    // 遍历名称，逐个复制字符, 算上.符，最长12字节，如果分离符，则只应有
    p = my_name;
    for (i = 0; (i < SFN_LEN) && (*p != '\0') && !is_path_sep(*p); i++) {
        if (ext_existed) 
        // 有扩展名的情况
        {
            if (p == ext_dot) {
                dest = dest_name + 8;// dest代表着写入位置，直接前进到第九位，因为已经出现了.符，说明文件名的部分结束了
                p++;// p指针正常前进
                i--;// 因为遇到了.符，这个char在8+3文件名中是不占位置的，所以这一次循环不需要i++。
                continue;// 没有意义的continue。。。。。
            }
            // 其余两种情况没区别，只要遇到字符，都转为大写存储
            else if (p < ext_dot) {
                *dest++ = toupper(*p++);
            }
            else {
                *dest++ = toupper(*p++);
            }
        }
        else 
        // 无扩展名的情况，也就是没有出现.（文件夹吧），以及.、..、.txt或abc.这样的比较奇怪的名称
        //（这些名称，现在全部都大写处理，对于.、..这两个常见的名字将会正确的处理（不会错误的识别为扩展名），
        // 问题是这将会导致.txt或abc.这样的文件名都当作没有扩展名处理，可能是漏掉了这种的处理）
        {
            *dest++ = toupper(*p++);
        }
    }
    return FS_ERR_OK;
}

/**
 * 判断两个文件名是否匹配
 * @param name_in_dir fatdir中的sfn格式文件名
 * @param to_find_name 应用层传入的文件名，可以是一个路径，如：传入的是/a/b/c.txt,则将会处理a这个文件
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
 * 获取一个目录项记录的的文件的起始簇号。
 * 原理：传入一个目录项，使用目录项的高低16位记录，计算那个文件的起始簇
 * @param item
 * @return
 */
static u32_t get_diritem_cluster (diritem_t * item) {
    return (item->DIR_FstClusHI << 16) | item->DIR_FstClusL0; 
    // 簇号由高低16位求和获得，等价于这里将DIR_FstClusHI << 16之后和DIR_FstClusL0做或运算；
}



 /**
  * 在给定的xfat分区的某一个目录文件的（initial_sector扇区，initial_offset比特）位置开始寻找path路径中描述的当前一级子文件
  * （比如输入path = "/abc/def/123.txt",则本次函数执行目标为在当前dir找到abc这一个文件对应的目录项，返回），
  * 找到之后，存入r_diritem返回，并且记录查找时所经过的偏移move_bytes（其间可能跨过0~n个簇，如果跨簇，这个偏移怎么搞？）
  * 1.如果path为空，直接返回当前目录文件的（initial_sector扇区，initial_offset比特）位置的目录项；
  * 2.如果遍历到某一簇的某一扇区的某一目录项找到了，则返回指向这个目录项指针的指针、所在簇、簇上偏移、move_bytes等定位用数据；
  * 3.如果遍历完了目录文件没找到，则返回ERR_EOF；
  * 
  * @param xfat 传入的，xfat结构
  * @param dir_cluster 传入的，要查找的file所在的目录数据所在的簇号（可能是目录文件中的某一簇），从这一簇开始向后扫描目录项
  * @param cluster_offset 传入的，簇中的偏移（单位B），从簇中的这个目录项序号开始寻找
  * @param move_bytes 返回的，找到的目标目录项，相对于上面的起始位置的偏移（单位B），现在我们没用；（每检测一个扇区上的一个目录项，就会累加一次，因此是；注：这个累加行为会持续经过多个簇，所以并不是记录的一簇内的偏移）
  * @param path 传入的，相对当前目录的路径———— （比如path = "/abc/def/123.txt",则本次函数执行目标为在当前dir找到abc这一个目录文件对应的目录项，返回）
  * @param r_diritem 返回的，查找到的diritem*指针的指针，如果用户传入的东西
  * @return 
  */
static xfat_err_t locate_file_dir_item(xfat_t *xfat, u32_t *dir_cluster, u32_t *cluster_offset,
                                    const char *path, u32_t *move_bytes, diritem_t **r_diritem) {
    u32_t curr_cluster = *dir_cluster;// 当前从dir_cluster这个簇开始
    xdisk_t * xdisk = xfat_get_disk(xfat);// 获取当前所在的disk

    // 使用cluster_offset计算下面两个值：（扇区，扇区偏移），因为之后遍历目录项时，是一次读取一个扇区的内容来遍历的，所以要获取到扇区的位置
    u32_t initial_sector = to_sector(xdisk, *cluster_offset);   // 相对于簇开头的扇区号，其实就是cluster_offset这个簇中字节偏移对应的簇中扇区偏移；
                                                                // 计算：用偏移cluster_offset和disk的扇区大小计算；
    u32_t initial_offset = to_sector_offset(xdisk, *cluster_offset);    // 使用簇中偏移计算出的在上面这个扇区中的偏移，单位B。组合起来可以得到（initial_sector扇区，initial_offset比特）定位了一个簇中的一个具体位置
    
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
                    || is_filename_match((const char *) dir_item->DIR_Name, path)) {    // 路径为空（因为本函数不改变path，所以路径为空的情况会在第一次进入这里时被捕获），或者完成了匹配（这两种情况都可以停止了）
                                                                                        // （路径是空，会在第一次运行时被这里捕获，然后执行终止）
                                                                                        // 直接返回当前目录

                    u32_t total_offset = i * xdisk->sector_size + j * sizeof(diritem_t); // 相对于扇区开始的总偏移 = 当前扇区的偏移 + 当前目录项在当前扇区中的偏移；
                    
                    *dir_cluster = curr_cluster; // 目标文件的目录项所在的簇
                    *cluster_offset = total_offset;// 所在簇上的具体偏移（字节）

                    *move_bytes = r_move_bytes + sizeof(diritem_t); // 匹配了，则move_bytes等于之前累计的r_move_bytes加上一个目录项的大小
                                                                    // 这里要专门加一个目录项大小，意义不明，因为目前open_sub_file中没有使用moved_bytes这个数据
                    
                    if (r_diritem) {    // 如果这个r_diritem（一个指针的指针）不为空，则让其管理的指针指向查找结果dir_item
                                        // （换言之只要用户着实传递了一个dir_item*进来，我就返回）
                                        // （如果，最后一个参数是0，下面这句操作就很危险（它将对地址0赋值），属于一种防御性编程吧） 
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
 * 传入一个目录文件的簇地址dir_cluster，以此目录文件作为根节点，"层层向下"查询path所指向的那个目标文件，
 * 将那个文件的解析结果存入file返回。如果path为空，则返回的file代表的是当前dir_cluster所在目录文件本身。
 * 原理：在dir_cluster指向的当前目录文件找到下一级；然后再前往下一级，更下一级...直到路径走完；或者中间某一级发现在此目录找不到目标的下一级（捕获到这种情况会返回FS_ERR_NONE）；
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
        // 本代码中，每一次循环会更新并用于下一次循环的参数是parent_cluster、curr_path
        while (curr_path != (const char *)0) {
            u32_t moved_bytes = 0;// 在这一簇目录文件中找到的目标文件相对parent_cluster_offset的偏移，单位字节，本课时中无意义
            dir_item = (diritem_t *)0;
            
            // 在父目录下查找指定路径对应的文件—————偏移值现在传入的是0，可能以后有用
            // 注意这里的dir_item是一个实际存在的对象，所以这个函数查询到之后将会把结果返回给dir_item这个指针；
            xfat_err_t err = locate_file_dir_item(xfat, &parent_cluster, &parent_cluster_offset,
                                                curr_path, &moved_bytes, &dir_item);

            // 两个异常的处理：

            if (err < 0) { // 上面这个函数运行中出现意外的错误（读取之类的遇到严重错误，导致函数异常结束）
                return err;
            }
            
            if (dir_item == (diritem_t *)0) {   // 上面这个函数遍历完了parent_cluster这个目录文件所有簇，都没有查找到curr_path指向的下一级文件/目录，
                                                // 返回FS_ERR_EOF（= 1），此时dir_item没有在locate_file_dir_item执行后得到目标item的指向；在这里捕获这种情况
                                                // 然后进行中断，不再继续向后执行
                return FS_ERR_NONE;
            }

            // 一切正常，找到了目标的dir_item项，开始判断是否到达目标层次的目标文件，如果没有，则需要前往下一级：

            curr_path = get_child_path(curr_path);  // 剥离path中的最外层目录部分——相当于进到其指向的下一层目录中，运行到此有可能已经指向目标文件了，这时这函数会返回(const char *)0

            // 根据curr_path的指向，决定是否要继续深入路径、寻找目标文件
            if (curr_path != (const char *)0) {
                // 当前curr_path != 0，说明目标文件还在更深的地方，需要继续下一次循环前进
                parent_cluster = get_diritem_cluster(dir_item);// 获得本次循环找到的文件的位置（簇号），如果当前是目录，则下一次循环将会以这个为dir簇
                parent_cluster_offset = 0; // 偏移重新置为0，本课时中无意义
            } else {
                // 当前curr_path = 0，说明dir_item已经指向了记录着目标文件的目录项
                // 获取目标文件起始簇
                file_start_cluster = get_diritem_cluster(dir_item);
                // 这里完全可以break了
            }
        }

        file->size = dir_item->DIR_FileSize;        // 获取大小
        file->type = get_file_type(dir_item);       // 获取当前文件的类型（自定义的三种文件类型）
        file->start_cluster = file_start_cluster;   // 该文件的起始簇
        file->curr_cluster = file_start_cluster;    // 当前簇，此时让其指向起始簇
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
    return open_sub_file(xfat, xfat->root_cluster, file, path); // 根据传入的根目录簇，path，层层向下找到目标文件的目录项，并且解析之后得到文件对象file返回
}

/**
 * 关闭已经打开的文件
 * @param file 待关闭的文件
 * @return
 */
xfat_err_t xfile_close(xfile_t *file) {
    return FS_ERR_OK;
}
