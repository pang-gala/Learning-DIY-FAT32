/**
 * 本源码配套的课程为 - 从0到1动手写FAT32文件系统。每个例程对应一个课时，尽可能注释。
 * 作者：李述铜
 * 课程网址：http://01ketang.cc
 * 版权声明：本源码非开源，二次开发，或其它商用前请联系作者。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xdisk.h"
#include "xfat.h"

extern xdisk_driver_t vdisk_driver;

const char * disk_path_test = "disk_test.img";
const char * disk_path = "disk.img";

static u32_t write_buffer[160*1024];
static u32_t read_buffer[160*1024];

xdisk_t disk;
xdisk_part_t disk_part;
xfat_t xfat;

// io测试，测试通过要注意关掉
int disk_io_test (void) {
    int err;
    xdisk_t disk_test;

    memset(read_buffer, 0, sizeof(read_buffer));

    err = xdisk_open(&disk_test, "vidsk_test", &vdisk_driver, (void *)disk_path_test);
    if (err) {
        printf("open disk failed!\n");
        return -1;
    }

    err = xdisk_write_sector(&disk_test, (u8_t *)write_buffer, 0, 2);
    if (err) {
        printf("disk write failed!\n");
        return -1;
    }

    err = xdisk_read_sector(&disk_test, (u8_t *)read_buffer, 0, 2);
    if (err) {
        printf("disk read failed!\n");
        return -1;
    }

    err = memcmp((u8_t *)read_buffer, (u8_t *)write_buffer, disk_test.sector_size * 2);
    if (err != 0) {
        printf("data no equal!\n");
        return -1;
    }

    err = xdisk_close(&disk_test);
    if (err) {
        printf("disk close failed!\n");
        return -1;
    }

    printf("disk io test ok!\n");
    return 0;
}

int disk_part_test (void) {
    u32_t count, i;
    xfat_err_t err = FS_ERR_OK;

    printf("partition read test...\n");

    err = xdisk_get_part_count(&disk, &count);
    if (err < 0) {
        printf("partion count detect failed!\n");
        return err;
    }
    printf("partition count:%d\n", count);

	for (i = 0; i < count; i++) {
		xdisk_part_t part;
		int err;

		err = xdisk_get_part(&disk, &part, i);
		if (err == -1) {
			printf("read partion in failed:%d\n", i);
			return -1;
		}

        printf("no %d: start: %d, count: %d, capacity:%.0f M\n",
               i, part.start_sector, part.total_sector,
               part.total_sector * disk.sector_size / 1024 / 1024.0);
    }
    return 0;
}

void show_dir_info (diritem_t * diritem) {
    char file_name[12];
    u8_t attr = diritem->DIR_Attr;

    // name 
    memset(file_name, 0, sizeof(file_name));
    memcpy(file_name, diritem->DIR_Name, 11);
    if (file_name[0] == 0x05) {
        file_name[0] = 0xE5;
    }
    printf("\n name: %s, ", file_name);

    // attr
    printf("\n\t");
    if (attr & DIRITEM_ATTR_READ_ONLY) {
        printf("readonly, ");
    }

    if (attr & DIRITEM_ATTR_HIDDEN) {
        printf("hidden, ");
    }

    if (attr & DIRITEM_ATTR_SYSTEM) {
        printf("system, ");
    }

    if (attr & DIRITEM_ATTR_DIRECTORY) {
        printf("directory, ");
    }

    if (attr & DIRITEM_ATTR_ARCHIVE) {
        printf("achinve, ");
    }

    // create time
    printf("\n\tcreate:%d-%d-%d, ", diritem->DIR_CrtDate.year_from_1980 + 1980,
            diritem->DIR_CrtDate.month, diritem->DIR_CrtDate.day);
    printf("\n\time:%d-%d-%d, ", diritem->DIR_CrtTime.hour, diritem->DIR_CrtTime.minute,
           diritem->DIR_CrtTime.second_2 * 2 + diritem->DIR_CrtTimeTeenth / 100);

    // last write time
    printf("\n\tlast write:%d-%d-%d, ", diritem->DIR_WrtDate.year_from_1980 + 1980,
           diritem->DIR_WrtDate.month, diritem->DIR_WrtDate.day);
    printf("\n\ttime:%d-%d-%d, ", diritem->DIR_WrtTime.hour,
           diritem->DIR_WrtTime.minute, diritem->DIR_WrtTime.second_2 * 2);

    // last acc time
    printf("\n\tlast acc:%d-%d-%d, ", diritem->DIR_LastAccDate.year_from_1980 + 1980,
           diritem->DIR_LastAccDate.month, diritem->DIR_LastAccDate.day);

    // size
    printf("\n\tsize %d kB, ", diritem->DIR_FileSize / 1024);
    printf("\n\tcluster %d, ", (diritem->DIR_FstClusHI << 16) | diritem->DIR_FstClusL0);

    printf("\n");
}


int fat_dir_test(void) {
    int err;
    u32_t curr_cluster;
    u8_t * culster_buffer;
    int index = 0;
    diritem_t * dir_item;
    u32_t j;

    printf("root dir read test...\n");

    u8_t* test_buffer = (u8_t*)malloc(5);
    culster_buffer = (u8_t *)malloc(xfat.cluster_byte_size); // 存放读取出来的簇的缓冲

    // 解析根目录所在的簇
    curr_cluster = xfat.root_cluster; // fat解析出来的根目录所在的簇号
    while (is_cluster_valid(curr_cluster)) { // 在簇有效的时候不断阅读下一个簇，直到完整地读取当前目录文件；
        err = read_cluster(&xfat, culster_buffer, curr_cluster, 1); // 读取一个簇到缓冲区（第一次从xfat.root_cluster开始）。需要fat配置信息，才知道从哪个扇区读取到哪个扇区（本质上都是读取扇区）
        if (err) {
            printf("read cluster %d failed\n", curr_cluster);
            return -1;
        }

        dir_item = (diritem_t *)culster_buffer; // 目录项类型指针，先让其指向当前簇的开头；
        for (j = 0; j < xfat.cluster_byte_size / sizeof(diritem_t); j++) { // 遍历当前簇中的目录项，遍历到了一个簇的大小时，就该停了
            u8_t  * name = (u8_t *)(dir_item[j].DIR_Name); // 当前目录项对应的DIR_Name字段
            if (name[0] == DIRITEM_NAME_FREE) { //name[0] == 0xE5, 则此目录为空 (目录项不包含文件和目录 ) ，向后遍历
                continue;
            } else if (name[0] == DIRITEM_NAME_END) { // name[0] == 0x00，则此目录为空 (同 0xE5), 并且此后的目录项都是空的，不必向后了
                break;
            }

            // 输出第index个文件的info，show_dir_info将会将可以解析的字段悉数打印出来
            index++;
            printf("no: %d, ", index); 
            show_dir_info(&dir_item[j]);
        }

        err = get_next_cluster(&xfat, curr_cluster, &curr_cluster);// 当前簇中的目录项遍历完成，前往下一簇【如何知道到了目录项的最后一簇？】————————————  
        if (err) {
            printf("get next cluster failed， current cluster %d\n", curr_cluster);
            return -1;
        }
    }

    return 0;
}

int main (void) {
    xfat_err_t err;
    int i;

    for (i = 0; i < sizeof(write_buffer) / sizeof(u32_t); i++) {
        write_buffer[i] = i;
    }

//    err = disk_io_test();
//    if (err) return err;

    err = xdisk_open(&disk, "vidsk", &vdisk_driver, (void *)disk_path);
    if (err) {
        printf("open disk failed!\n");
        return -1;
    }

    err = disk_part_test();
    if (err) return err;

    err = xdisk_get_part(&disk, &disk_part, 1);
    if (err < 0) {
        printf("read partition info failed!\n");
        return -1;
    }

    err = xfat_open(&xfat, &disk_part);
    if (err < 0) {
        return err;
    }

    err = fat_dir_test();
    if (err) return err;

    err = xdisk_close(&disk);
    if (err) {
        printf("disk close failed!\n");
        return -1;
    }

    printf("Test End!\n");
    return 0;
}
