#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <regex.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <mtd/mtd-user.h>

#define LOG printf
#define ERR printf


#define MAX_ERROR_MSG 0x1000
int compile_regex(regex_t * r, const char * regex_text) {
    int status = regcomp(r, regex_text, REG_EXTENDED | REG_NEWLINE | REG_ICASE);
    if (status != 0) {
        char error_message[MAX_ERROR_MSG];
        regerror(status, r, error_message, MAX_ERROR_MSG);
        ERR("Regex error compiling '%s': %s\n", regex_text, error_message); fflush(stdout);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

struct MtdName {
    int id;
    char name[64];
    char path[64];
};

#define MTD_MAX 16
struct MtdName mtds[MTD_MAX];
uint8_t mtd_count = 0;
int load_mtds() {
    int fd = open("/proc/mtd", O_RDONLY);

    unsigned char str[1024] = {0x00};
    lseek(fd, 0, SEEK_SET);
    ssize_t readed = read(fd, str, sizeof(str));

    regex_t regex;
    if(compile_regex(&regex, "^(mtd([[:digit:]]+)):[[:space:]]+[[:digit:]]+[[:space:]]+[[:digit:]]+[[:space:]]+\"([[:alnum:]_]+)\"$") < 0) { printf("compile_regex error\n"); return EXIT_FAILURE; };
    size_t n_matches = 4; // We have 3 capturing group + the whole match group
    regmatch_t m[n_matches];
    int start_pos = 0;
    mtd_count = 0;
    memset(mtds, 0, sizeof(struct MtdName)*MTD_MAX);
    while (true) {
        if (mtd_count >= MTD_MAX) break;
        int match = regexec(&regex, str + start_pos, n_matches, m, 0);
        if (match > 0) break;

        sprintf(mtds[mtd_count].path, "/dev/%.*s", (int)(m[1].rm_eo - m[1].rm_so), str + start_pos + m[1].rm_so);
        char id[32] = {0x00};
        sprintf(id, "%.*s", (int)(m[2].rm_eo - m[2].rm_so), str + start_pos + m[2].rm_so);
        mtds[mtd_count].id = atoi(id);
        sprintf(mtds[mtd_count].name, "%.*s", (int)(m[3].rm_eo - m[3].rm_so), str + start_pos + m[3].rm_so);
        start_pos += m[1].rm_eo;
        mtd_count++;
    }
    regfree(&regex);

    close(fd);
    return EXIT_SUCCESS;
}

char * getPathByName(const char *name) {
    for (int i = 0; i < mtd_count; ++i)
        if (strcmp(name, mtds[i].name) == 0) return mtds[i].path;
    return NULL;
}

void listPartitions() {
    for (int i = 0; i < mtd_count; ++i)
        printf("    Id: %d     Path: %s     Name: %s\n", mtds[i].id, mtds[i].path, mtds[i].name);
}

int write_image(const char *mtd_path, const char *mtd_name, const char *path) {
    int fd = open(mtd_path, O_RDWR);
    if (!fd) { ERR("Can't open mtd partition '%s'(%s)\n", mtd_name, mtd_path); close(fd); return EXIT_FAILURE; }
    mtd_info_t mtd_info;
    ioctl(fd, MEMGETINFO, &mtd_info);

    FILE *file = fopen(path, "r");
    if (!file) { ERR("Can't open file (%s)\n", path); fclose(file); close(fd); return EXIT_FAILURE; }
    fseek(file, 0L, SEEK_END);
    long file_size = ftell(file);
    if (file_size <= 0) { ERR("Size(%u) of file(%s) is zero\n", file_size, path); fclose(file); close(fd); return EXIT_FAILURE; }
    if (file_size > mtd_info.size) { ERR("Size(%u) of file(%s) is more than size (%u) of mtd partition '%s'(%s)\n", file_size, path, mtd_info.size, mtd_name, mtd_path); fclose(file); close(fd); return EXIT_FAILURE; }
    fseek(file, 0L, SEEK_SET);

    LOG("Erasing.. "); fflush(stdout);
    erase_info_t ei;
    ei.length = mtd_info.erasesize;
    for (ei.start = 0; ei.start < file_size; ei.start += ei.length) {
        ioctl(fd, MEMUNLOCK, &ei);
        ioctl(fd, MEMERASE, &ei);
    }
    lseek(fd, 0, SEEK_SET);
    LOG("Ok!\n");

    LOG("Writing.. "); fflush(stdout);
    const ssize_t buf_size = 128*1024;
    char buf[buf_size];
    size_t all = 0;
    while (true) {
        if (all >= file_size) break;

        size_t need_to_read = sizeof(buf);
        if (all + need_to_read > file_size) need_to_read = file_size - all;
        size_t readed = fread(buf, 1, need_to_read, file);
        if (readed != need_to_read) { ERR("Fail!\nCan't read from file '%s'\n", path); fclose(file); close(fd); return EXIT_FAILURE; }
        if (write(fd, buf, readed) != readed) { ERR("Fail!\nCan't write to mtd '%s'(%s)\n", mtd_name, mtd_path); fclose(file); close(fd); return EXIT_FAILURE; }
        all += readed;
    }
    fclose(file);
    close(fd);
    if (all != file_size) {
        ERR("Fail!\nError: Incorrect write image! %lu bytes was wrote. File size is %lu.\n", all, file_size);
        return EXIT_FAILURE;
    }
    LOG("Ok!\n%lu bytes was wrote. File size is %lu.\n", all, file_size);
    return EXIT_SUCCESS;
}

int check_image(const char *mtd_path, const char *mtd_name, const char *path) {
    int fd = open(mtd_path, O_RDONLY);
    if (!fd) { ERR("Can't open mtd partition '%s'(%s)\n", mtd_name, mtd_path); close(fd); return EXIT_FAILURE; }

    mtd_info_t mtd_info;
    ioctl(fd, MEMGETINFO, &mtd_info);

    FILE *file = fopen(path, "r");
    if (!file) { ERR("Can't open file (%s)\n", path); fclose(file); close(fd); return EXIT_FAILURE; }
    fseek(file, 0L, SEEK_END);
    long file_size = ftell(file);
    if (file_size <= 0) { ERR("Size(%lu) of file(%s) is zero\n", file_size, path); fclose(file); close(fd); return EXIT_FAILURE; }
    if (file_size > mtd_info.size) { ERR("Size(%lu) of file(%s) is more than size (%u) of mtd partition '%s'(%s)\n", file_size, path, mtd_info.size, mtd_name, mtd_path); fclose(file); close(fd); return EXIT_FAILURE; }
    fseek(file, 0L, SEEK_SET);

    LOG("Checking.. "); fflush(stdout);
    char buf_file[1024];
    char buf_mtd[sizeof(buf_file)];
    size_t all = 0;
    while (true) {
        if (all >= file_size) break;

        size_t need_to_read = sizeof(buf_file);
        if (all + need_to_read > file_size) need_to_read = file_size - all;
        size_t readed_file = fread(buf_file, 1, need_to_read, file);
        if (readed_file != need_to_read) { ERR("Fail!\nCan't read from file '%s'  %lu!=%lu\n", path, need_to_read, readed_file); fclose(file); close(fd); return EXIT_FAILURE; }
        ssize_t readed_mtd = read(fd, buf_mtd, need_to_read);
        if (readed_mtd != need_to_read) { ERR("Fail!\nCan't read from mtd '%s'(%s)\n", mtd_name, mtd_path); fclose(file); close(fd); return EXIT_FAILURE; }

        for (uint32_t i = 0; i < need_to_read; ++i)
            if (buf_mtd[i] != buf_file[i]) {
                ERR("Fail!\nData in file %s and mtd '%s'(%s) are differents in pos %lu.\n", path, mtd_name, mtd_path, all + i); fclose(file); close(fd); return EXIT_FAILURE;
            }
        all += need_to_read;
    }
    fclose(file); close(fd);
    if (all != file_size) {
        ERR("Fail!\nError: Incorrect check! %lu bytes was checked. File size is %lu.\n", all, file_size);
        return EXIT_FAILURE;
    }
    LOG("Ok!\n%lu bytes was checked. File size is %lu.\n", all, file_size);
    LOG("File '%s' and mtd '%s'(%s) data the same.\n", path, mtd_name, mtd_path);
    return EXIT_SUCCESS;
}

int write_file(const char *mtd_path, const char *mtd_name, const char *path) {
     int fd = open(mtd_path, O_RDWR);
    if (!fd) { ERR("Can't open mtd partition '%s'(%s)\n", mtd_name, mtd_path); close(fd); return EXIT_FAILURE; }
    mtd_info_t mtd_info;
    ioctl(fd, MEMGETINFO, &mtd_info);

    FILE *file = fopen(path, "r");
    if (!file) { ERR("Can't open file (%s)\n", path); fclose(file); close(fd); return EXIT_FAILURE; }
    fseek(file, 0L, SEEK_END);
    long file_size = ftell(file);
    if (file_size <= 0) { ERR("Size(%lu) of file(%s) is zero\n", file_size, path); fclose(file); close(fd); return EXIT_FAILURE; }
    if (file_size + sizeof(uint32_t)*2 > mtd_info.size) { ERR("Size(%lu) of file(%s) is more than size (%u) of mtd partition '%s'(%s)\n", file_size, path, mtd_info.size, mtd_name, mtd_path); fclose(file); close(fd); return EXIT_FAILURE; }
    fseek(file, 0L, SEEK_SET);

    LOG("Erasing.. "); fflush(stdout);
    erase_info_t ei;
    ei.length = mtd_info.erasesize;
    for (ei.start = 0; ei.start < file_size; ei.start += ei.length) {
        ioctl(fd, MEMUNLOCK, &ei);
        ioctl(fd, MEMERASE, &ei);
    }
    lseek(fd, 0, SEEK_SET);
    LOG("Ok!\n");

    LOG("Writing.. "); fflush(stdout);

    {
        uint32_t size = file_size;
        if (write(fd, &size, sizeof(uint32_t)) != sizeof(uint32_t)) { ERR("Fail!\nCan't write to mtd '%s'(%s)\n", mtd_name, mtd_path); fclose(file); close(fd); return EXIT_FAILURE; }
        if (write(fd, &size, sizeof(uint32_t)) != sizeof(uint32_t)) { ERR("Fail!\nCan't write to mtd '%s'(%s)\n", mtd_name, mtd_path); fclose(file); close(fd); return EXIT_FAILURE; }
    }

    const ssize_t buf_size = 128*1024;
    char buf[buf_size];
    size_t all = 0;
    while (true) {
        if (all >= file_size) break;
        size_t need_to_read = sizeof(buf);
        if (all + need_to_read > file_size) need_to_read = file_size - all;
        size_t readed = fread(buf, 1, need_to_read, file);
        if (readed != need_to_read) { ERR("Fail!\nCan't read from file '%s'\n", path); fclose(file); close(fd); return EXIT_FAILURE; }
        if (write(fd, buf, readed) != readed) { ERR("Fail!\nCan't write to mtd '%s'(%s)\n", mtd_name, mtd_path); fclose(file); close(fd); return EXIT_FAILURE; }
        all += readed;
    }
    fclose(file); close(fd);
    if (all != file_size) {
        ERR("Fail!\nError: Incorrect write file! %lu bytes was wrote. File data is %lu.\n", all, file_size);
        return EXIT_FAILURE;
    }
    LOG("Ok!\n%lu bytes was wrote. File data is %lu.\n", all, file_size);
    return EXIT_SUCCESS;
}

int read_file(const char *mtd_path, const char *mtd_name, const char *path) {
    int fd = open(mtd_path, O_RDONLY);
    if (!fd) { ERR("Can't open mtd partition '%s'(%s)\n", mtd_name, mtd_path); close(fd); return EXIT_FAILURE; }

    mtd_info_t mtd_info;
    ioctl(fd, MEMGETINFO, &mtd_info);

    uint32_t file_size = 0;
    uint32_t file_crc32 = 0;
    lseek(fd, 0, SEEK_SET);
    read(fd, &file_size, sizeof(uint32_t));
    read(fd, &file_crc32, sizeof(uint32_t));
    if (file_size > mtd_info.size - sizeof(uint32_t)) {
        ERR("Size of file (%u) in mtd '%s' is more than mtd partition size (%lu)\n", file_size, mtd_path, mtd_info.size); close(fd); return EXIT_FAILURE;
    }

    FILE *file = fopen(path, "w");
    if (!file) { ERR("Can't open file (%s)\n", path); fclose(file); close(fd); return EXIT_FAILURE; }

    LOG("Reading.. "); fflush(stdout);
    char read_buf[1024];
    size_t all = 0;
    while (true) {
        if (all >= file_size) break;

        size_t need_to_read = sizeof(read_buf);
        if (all + need_to_read > file_size) need_to_read = file_size - all;
        ssize_t readed = read(fd, read_buf, need_to_read);
        if (readed != need_to_read) { ERR("Can't read file from mtd '%s'(%s)\n", mtd_name, mtd_path); fclose(file); close(fd); return EXIT_FAILURE; }
        if (fwrite(read_buf, 1, readed, file) != readed) { ERR("Can't write to file '%s'\n", path); fclose(file); close(fd); return EXIT_FAILURE; }
        all += readed;
    }
    fclose(file); close(fd);
    if (all != file_size) {
        ERR("Fail!\nError: Incorrect file read! %lu bytes was read. File size is %u.\n", all, file_size);
        return EXIT_FAILURE;
    }
    LOG("Ok!\n%lu bytes was read.\n", all);
    return EXIT_SUCCESS;
}


int check_file(const char *mtd_path, const char *mtd_name, const char *path) {
    int fd = open(mtd_path, O_RDONLY);
    if (!fd) { ERR("Can't open mtd partition '%s'(%s)\n", mtd_name, mtd_path); close(fd); return EXIT_FAILURE; }

    mtd_info_t mtd_info;
    ioctl(fd, MEMGETINFO, &mtd_info);

    uint32_t mtd_file_size = 0;
    uint32_t mtd_file_crc32 = 0;
    lseek(fd, 0, SEEK_SET);
    read(fd, &mtd_file_size, sizeof(uint32_t));
    read(fd, &mtd_file_crc32, sizeof(uint32_t));
    if (mtd_file_size > mtd_info.size - sizeof(uint32_t)) {
        ERR("Size of file (%u) in mtd '%s' is more than mtd partition size (%lu)\n", mtd_file_size, mtd_path, mtd_info.size); close(fd); return EXIT_FAILURE;
    }

    FILE *file = fopen(path, "r");
    if (!file) { ERR("Can't open file (%s)\n", path); fclose(file); close(fd); return EXIT_FAILURE; }
    fseek(file, 0L, SEEK_END);
    long file_size = ftell(file);
    if (file_size <= 0) { ERR("Size(%lu) of file(%s) is zero\n", file_size, path); fclose(file); close(fd); return EXIT_FAILURE; }
    if (file_size > mtd_info.size) { ERR("Size(%lu) of file(%s) is more than size (%u) of mtd partition '%s'(%s)\n", file_size, path, mtd_info.size, mtd_name, mtd_path); fclose(file); close(fd); return EXIT_FAILURE; }
    fseek(file, 0L, SEEK_SET);

    if (mtd_file_size != file_size) { ERR("Size(%lu) of file(%s) is not equal size(%u) in mtd '%s'(%s)\n", file_size, path, mtd_file_size, mtd_name, mtd_path); fclose(file); close(fd); return EXIT_FAILURE; }

    LOG("Checking.. "); fflush(stdout);
    char buf_file[1024];
    char buf_mtd[sizeof(buf_file)];
    size_t all = 0;
    while (true) {
        if (all >= file_size) break;

        size_t need_to_read = sizeof(buf_file);
        if (all + need_to_read > file_size) need_to_read = file_size - all;
        size_t readed_file = fread(buf_file, 1, need_to_read, file);
        if (readed_file != need_to_read) { ERR("Fail!\nCan't read from file '%s'  %lu!=%lu\n", path, need_to_read, readed_file); fclose(file); close(fd); return EXIT_FAILURE; }
        ssize_t readed_mtd = read(fd, buf_mtd, need_to_read);
        if (readed_mtd != need_to_read) { ERR("Fail!\nCan't read from mtd '%s'(%s)\n", mtd_name, mtd_path); fclose(file); close(fd); return EXIT_FAILURE; }

        for (uint32_t i = 0; i < need_to_read; ++i)
            if (buf_mtd[i] != buf_file[i]) {
                ERR("Fail!\nData in file %s and mtd '%s'(%s) are differents in pos %lu.\n", path, mtd_name, mtd_path, all + i); fclose(file); close(fd); return EXIT_FAILURE;
            }
        all += need_to_read;
    }
    fclose(file); close(fd);
    if (all != file_size) {
        ERR("Fail!\nError: Incorrect check! %lu bytes was checked. File size is %lu.\n", all, file_size);
        return EXIT_FAILURE;
    }
    LOG("Ok!\n%lu bytes was checked. File size is %lu.\n", all, file_size);
    LOG("File '%s' and mtd '%s'(%s) data the same.\n", path, mtd_name, mtd_path);
    return EXIT_SUCCESS;
}


void Usage() {
    printf("Usage:\n");
    printf("   --list                                      List mtd partitions.\n");
    printf("   --write_image <mtd name> <path to image>    Write image to mtd partition\n");
    printf("   --check_image <mtd name> <path to image>    Check image in mtd partition\n");
    printf("   --write_file <mtd name> <path to file>      Write file to mtd partition.\n");
    printf("   --read_file <mtd name> <path to file>       Read file from mtd partition.\n");
    printf("   --check_file <mtd name> <path to file>      Check file in mtd partition.\n");
    printf("Available partitions:\n");
    listPartitions();
}

int main(int argc, char *argv[]) {
    if (load_mtds() == EXIT_FAILURE) return EXIT_FAILURE;
    if (argc < 2) { Usage(); return EXIT_SUCCESS; }

    char *cmd = argv[1];
    if (strcmp(cmd, "--list") == 0) { listPartitions(); return EXIT_SUCCESS; }
    if (argc != 4) { printf("Incorrect cmd args.\n"); Usage(); return EXIT_FAILURE; }
    char *mtd_name = argv[2];
    char *mtd_path = getPathByName(mtd_name);
    if (mtd_path == NULL) {
        printf("Error: mtd partition with name '%s' doesn't exists!\n", mtd_name);
        return EXIT_FAILURE;
    }

    char *file_path = argv[3];
    if (strcmp(cmd, "--write_image") == 0) { return write_image(mtd_path, mtd_name, file_path); }
    if (strcmp(cmd, "--check_image") == 0) { return check_image(mtd_path, mtd_name, file_path); }
    if (strcmp(cmd, "--read_file") == 0) { return read_file(mtd_path, mtd_name, file_path); }
    if (strcmp(cmd, "--write_file") == 0) { return write_file(mtd_path, mtd_name, file_path); }
    if (strcmp(cmd, "--check_file") == 0) { return check_file(mtd_path, mtd_name, file_path); }

    printf("Incorrect cmd args\n"); Usage(); return EXIT_FAILURE;
}
