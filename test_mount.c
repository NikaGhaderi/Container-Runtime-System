#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <errno.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <device> <mount_point>\n", argv[0]);
        return 1;
    }
    char *device = argv[1];
    char *mount_point = argv[2];
    if (mount(device, mount_point, "ext4", 0, NULL) != 0) {
        fprintf(stderr, "Mount failed: %s (errno %d)\n", strerror(errno), errno);
        return 1;
    }
    printf("Successfully mounted %s on %s\n", device, mount_point);
    return 0;
}