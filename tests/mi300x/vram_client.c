/* SPDX-License-Identifier: BSD-3-Clause */

#include <err.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "tran_sock.h"

static void
negotiate(int sock)
{
    struct vfio_user_version version = {0};
    version.major = LIB_VFIO_USER_MAJOR;
    version.minor = LIB_VFIO_USER_MINOR;
    char capabilities[] = "{\"capabilities\":{\"max_msg_fds\":8}}";
    struct iovec vectors[3] = {0};
    vectors[1].iov_base = &version;
    vectors[1].iov_len = sizeof(version);
    vectors[2].iov_base = capabilities;
    vectors[2].iov_len = sizeof(capabilities);
    struct vfio_user_header header;
    void *reply = NULL;
    size_t length = 0;
    if (tran_sock_send_iovec(sock, 1, false, VFIO_USER_VERSION, vectors, 3,
                             NULL, 0, 0) < 0 ||
        tran_sock_recv_alloc(sock, &header, true, NULL, &reply, &length) < 0) {
        err(EXIT_FAILURE, "vfio-user version negotiation");
    }
    free(reply);
}

static void
access_bar(int sock, bool write, uint64_t offset, uint8_t *data, size_t size)
{
    struct vfio_user_region_access request = {0};
    request.offset = offset;
    request.region = VFU_PCI_DEV_BAR0_REGION_IDX;
    request.count = size;
    struct iovec vectors[3] = {0};
    vectors[1].iov_base = &request;
    vectors[1].iov_len = sizeof(request);
    vectors[2].iov_base = data;
    vectors[2].iov_len = size;
    size_t length = sizeof(request) + (write ? 0 : size);
    struct vfio_user_region_access *reply = calloc(1, length);
    if (!reply) {
        err(EXIT_FAILURE, "allocate region reply");
    }
    if (tran_sock_msg_iovec(
            sock, 3, write ? VFIO_USER_REGION_WRITE : VFIO_USER_REGION_READ,
            vectors, write ? 3 : 2, NULL, 0, NULL, reply, length, NULL,
            NULL) < 0) {
        err(EXIT_FAILURE, "access VRAM BAR");
    }
    if (reply->count != size || reply->offset != offset) {
        errx(EXIT_FAILURE, "invalid VRAM access reply");
    }
    if (!write) {
        memcpy(data, reply + 1, size);
    }
    free(reply);
}

int
main(int argc, char **argv)
{
    if (argc != 5) {
        errx(EXIT_FAILURE, "usage: %s socket pool offset size", argv[0]);
    }
    uint64_t offset = strtoull(argv[3], NULL, 0);
    uint64_t size = strtoull(argv[4], NULL, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(argv[1]) >= sizeof(address.sun_path)) {
        errx(EXIT_FAILURE, "socket path is too long");
    }
    memcpy(address.sun_path, argv[1], strlen(argv[1]) + 1);
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct timeval timeout = {.tv_sec = 10};
    if (sock < 0 ||
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) <
            0 ||
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) <
            0 ||
        connect(sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
        err(EXIT_FAILURE, "connect MI300X vfio-user server");
    }
    negotiate(sock);

    struct vfio_region_info request = {0};
    request.argsz = sizeof(request);
    request.index = VFU_PCI_DEV_BAR0_REGION_IDX;
    if (tran_sock_msg(sock, 2, VFIO_USER_DEVICE_GET_REGION_INFO, &request,
                      sizeof(request), NULL, &request, sizeof(request)) < 0) {
        err(EXIT_FAILURE, "query VRAM region size");
    }
    struct vfio_region_info *region = calloc(1, request.argsz);
    if (!region) {
        err(EXIT_FAILURE, "allocate VRAM region info");
    }
    *region = request;
    int descriptors[8];
    size_t count = 8;
    if (tran_sock_msg_fds(sock, 2, VFIO_USER_DEVICE_GET_REGION_INFO, &request,
                          sizeof(request), NULL, region, request.argsz,
                          descriptors, &count) < 0) {
        err(EXIT_FAILURE, "receive VRAM backing descriptor");
    }
    if (count != 1 || region->offset != offset || region->size != size ||
        !(region->flags & VFIO_REGION_INFO_FLAG_MMAP)) {
        errx(EXIT_FAILURE, "VRAM region differs from the GPU backing");
    }
    struct stat actual, expected;
    if (fstat(descriptors[0], &actual) < 0 || stat(argv[2], &expected) < 0) {
        err(EXIT_FAILURE, "inspect VRAM backing identity");
    }
    if (actual.st_dev != expected.st_dev || actual.st_ino != expected.st_ino) {
        errx(EXIT_FAILURE, "VRAM descriptor refers to a different file");
    }
    uint8_t *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                            descriptors[0], offset);
    if (mapping == MAP_FAILED) {
        err(EXIT_FAILURE, "map exported VRAM");
    }
    uint8_t first[] = "mapped VRAM writes";
    uint8_t second[] = "protocol VRAM writes";
    uint8_t received[sizeof(first)];
    memcpy(mapping + 4096, first, sizeof(first));
    access_bar(sock, false, 4096, received, sizeof(received));
    if (memcmp(received, first, sizeof(first))) {
        errx(EXIT_FAILURE, "protocol reads disagree with the mapped VRAM");
    }
    access_bar(sock, true, 8192, second, sizeof(second));
    if (memcmp(mapping + 8192, second, sizeof(second))) {
        errx(EXIT_FAILURE, "mapped VRAM misses a protocol write");
    }
    int pool = open(argv[2], O_RDONLY);
    uint8_t persisted[sizeof(second)];
    if (pool < 0 ||
        pread(pool, persisted, sizeof(persisted), offset + 8192) !=
            sizeof(persisted) ||
        memcmp(persisted, second, sizeof(second))) {
        errx(EXIT_FAILURE, "protocol writes missed the assigned pool offset");
    }
    close(pool);
    munmap(mapping, size);
    close(descriptors[0]);
    free(region);
    close(sock);
    puts("PASS: VRAM fd identity, offset, size and bidirectional data");
    return EXIT_SUCCESS;
}
