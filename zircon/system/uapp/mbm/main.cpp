// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <utility>

namespace {

struct file_info_t {
    int fd;
    char path[512];
};

int create_all(const char* path, const char* basename, int files, int flags,
               file_info_t* file_infos) {
    for (int i = 0; i < files; i++) {
        sprintf(file_infos[i].path, "%s%s-%d", path, basename, i);
        file_infos[i].fd = open(file_infos[i].path, O_CREAT | O_RDWR | flags, S_IRWXU);
        if (file_infos[i].fd < 0) {
            printf("create failed -%s- %d\n", file_infos[i].path, i);
            return 2;
        }
    }
    return 0;
}

int lseek_all(file_info_t* file_infos, int files) {
    for (int i = 0; i < files; i++) {
        if (lseek(file_infos[i].fd, 0, SEEK_SET) < 0) {
            printf("lseek failed -%s- %d\n", file_infos[i].path, i);
            return 1;
        }
    }
    return 0;
}

int write_in_chunks(file_info_t* file, char* buf, uint64_t buf_size, uint64_t chunk_size) {
    bool print_once = true;
    off_t offset = lseek(file->fd, 0, SEEK_CUR);
    for (uint64_t i = 0; i < chunk_size; i += buf_size) {
        if (write(file->fd, buf, (i + buf_size) < chunk_size ? buf_size : chunk_size - i) < 0) {
            printf("write failed -%s- %lu\n", file->path, i);
            return 1;
        }
        print_once = true;
    }
    if (print_once) {
        printf("Writing %s %lu %lu at %lld\n", file->path, chunk_size, buf_size, offset);
    }
    return 0;
}

int write_all(file_info_t* files, int filec, uint64_t file_size, uint64_t chunk_size,
              uint64_t buf_size, char x) {
    if (file_size % chunk_size != 0) {
        fprintf(stderr, "invalid file_size or chunk_size\n");
        return 1;
    }

    if (chunk_size % buf_size != 0) {
        fprintf(stderr, "invalid chunk_size or buf_size\n");
        return 2;
    }

    char* buf = (char*)alloca(buf_size);
    memset(buf, x, buf_size);
    for (uint64_t offset = 0; offset <= file_size; offset += chunk_size) {
        for (int i = 0; i < filec; i++) {
            if (write_in_chunks(&files[i], buf, buf_size, chunk_size) != 0) {
                printf("write_all failed -%s- %d\n", files[i].path, i);
                return 3;
            }
        }
    }
    return 0;
}

int fsync_all(file_info_t* files, int filec, bool reopen, int flags = 0) {
    for (int i = 0; i < filec; i++) {
        if (fsync(files[i].fd) < 0) {
            fprintf(stderr, "fsync for %s failed\n", files[i].path);
            return 1;
        }

        if (close(files[i].fd) < 0) {
            fprintf(stderr, "close for %s failed\n", files[i].path);
            return 2;
        }

        if (reopen) {
            files[i].fd = open(files[i].path, O_RDWR | flags);
            if (files[i].fd < 0) {
                fprintf(stderr, "Reopen for %s failed\n", files[i].path);
                return 3;
            }
        }
    }

    return 0;
}

int parse_size(const char* size_str, size_t* out) {
    char* end;
    size_t size = strtoull(size_str, &end, 10);

    switch (end[0]) {
    case 'K':
    case 'k':
        size *= 1024;
        end++;
        break;
    case 'M':
    case 'm':
        size *= (1024 * 1024);
        end++;
        break;
    case 'G':
    case 'g':
        size *= (1024 * 1024 * 1024);
        end++;
        break;
    }

    if (end[0] || size == 0) {
        fprintf(stderr, "Bad size: %s\n", size_str);
        return -1;
    }

    *out = size;
    return 0;
}

int Usage2() {
    fprintf(stdout, "usage: executable mbm options\n");
    fprintf(stdout, "file_count         : Number of file to create\n");
    fprintf(stdout, "file_size          : Final size of each file\n");
    fprintf(stdout, "chunk_size         : Size of contiguous write\n");
    fprintf(stdout, "buf_size           : Size of each write buffer\n");
    fprintf(stdout, "path[1024]         : Base directory\n");
    fprintf(stdout, "basename[1024]     : prefix for each filename\n");
    fprintf(stdout, "sync               : sync after write all: \n");
    return -1;
}

struct MicroBenchOptions {
    uint64_t file_count;
    uint64_t file_size;
    uint64_t chunk_size;
    uint64_t buf_size;
    char path[1024];
    char basename[1024];
    bool sync;
};

int BMParseCommandLineArguments(int argc, char** argv, MicroBenchOptions* options) {
    static const struct option opts[] = {
        {"path", required_argument, NULL, 'p'},
        {"basename", required_argument, NULL, 'z'},
        {"file_count", required_argument, NULL, 'f'},
        {"file_size", required_argument, NULL, 's'},
        {"chunk_size", required_argument, NULL, 'c'},
        {"buf_size", required_argument, NULL, 'b'},
        {"sync", required_argument, NULL, 'n'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };
    int option_index = 0;
    for (int opt; (opt = getopt_long(argc, argv, "p:z:f:s:c:b:n::h", opts, &option_index)) != -1;) {
        printf("parsed %c\n", opt);
        switch (opt) {
        case 'p':
            strncpy(options->path, optarg, sizeof(options->path) - 1);
            break;
        case 'z':
            strncpy(options->basename, optarg, sizeof(options->basename) - 1);
            break;
        case 'f':
            if (parse_size(optarg, &options->file_count) != 0) {
                Usage2();
                return 1;
            }
            break;
        case 's':
            if (parse_size(optarg, &options->file_size) != 0) {
                Usage2();
                return 2;
            }
            break;
        case 'c':
            if (parse_size(optarg, &options->chunk_size) != 0) {
                Usage2();
                return 3;
            }
            break;
        case 'b':
            if (parse_size(optarg, &options->buf_size) != 0) {
                Usage2();
                return 4;
            }
            break;
        case 'n':
            options->sync = (optarg == nullptr || strcmp(optarg, "true") == 0);
            break;
        case 'h':
        default:
            Usage2();
        }
    }
    if (options->file_count == 0 || options->file_size == 0 || options->chunk_size == 0 ||
        options->buf_size == 0 || strlen(options->path) == 0) {
        Usage2();
        return 5;
    }
    return 0;
}

void PrintMetrics(const char* bpath, const char* msg) {
    int fd = open("/sys/block/loop1/stat", O_RDONLY);
    char buf[200];
    if (fd < 0) {
        printf("Failed to open stat file\n");
        exit(1);
    }
    if (read(fd, buf, 200) < 0) {
        printf("Failed to read stat file\n");
        exit(2);
    }
    close(fd);
    printf("==== %s - for: %s: %s. =====\n", msg, bpath, buf);
    printf("==== %s Blocks written %lu ====\n", msg, 0);
}

int run_load(const char* path, const char* basename, int file_count, uint64_t file_size,
             uint64_t chunk_size, uint64_t buf_size, bool reopen) {
    file_info_t* file_infos = (file_info_t*)alloca(sizeof(file_info_t) * file_count);
    char bpath[1024];
    snprintf(bpath, 1023, "%s", "/sys/block/loop1/stat");

    char bname[1024];
    int file_count_iteration = 128;

    for (int i = 0; i < file_count; i += file_count_iteration) {
        if (i + file_count_iteration > file_count) {
            file_count_iteration = file_count - i;
        }
        snprintf(bname, 1023, "%s-%d-", basename, i);
        PrintMetrics(bpath, "Before create");
        if (create_all(path, bname, file_count_iteration, 0, file_infos) != 0) {
            return 1;
        }

        PrintMetrics(bpath, "After create before write_all");
        if (write_all(file_infos, file_count_iteration, file_size, chunk_size, buf_size, 'a')) {
            return 2;
        }

        PrintMetrics(bpath, "After write_all and before lseek_all");
        if (lseek_all(file_infos, file_count_iteration) != 0) {
            return 3;
        }

        PrintMetrics(bpath, "After lseek_all and before fsync_all");
        if (fsync_all(file_infos, file_count_iteration, true) != 0) {
            return 4;
        }

        PrintMetrics(bpath, "After fsync_all and before update_all");
        if (write_all(file_infos, file_count_iteration, file_size, chunk_size, buf_size, 'b')) {
            return 5;
        }

        PrintMetrics(bpath, "After update_all and before fsync_again");
        if (fsync_all(file_infos, file_count_iteration, false) != 0) {
            return 6;
        }

        PrintMetrics(bpath, "Before return");
    }
    return 0;
}

} // namespace

int mbm(int argc, char** argv) {
    MicroBenchOptions options;

    if (BMParseCommandLineArguments(argc, argv, &options) != 0) {
        printf(
            "path:%s\n file_count:%lu\n file_size:%lu\n chunk_size:%lu\n bug_size:%lu\n sync:%d\n",
            options.path, options.file_count, options.file_size, options.chunk_size,
            options.buf_size, options.sync);
        return 1;
    }
    printf("path:%s\n file_count:%lu\n file_size:%lu\n chunk_size:%lu\n bug_size:%lu\n sync:%d\n",
           options.path, options.file_count, options.file_size, options.chunk_size,
           options.buf_size, options.sync);
    return run_load(options.path, options.basename, static_cast<int>(options.file_count),
                    options.file_size, options.chunk_size, options.buf_size, options.sync);
}

int main(int argc, char** argv) {
    if (strcmp(argv[1], "mbm") == 0) {
        return mbm(argc - 1, &argv[1]);
    }
    return 0;
}
