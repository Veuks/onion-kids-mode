#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MI_AO_SETMUTE 0x4008690d

static int set_audio_mute(int mute)
{
    int fd = open("/dev/mi_ao", O_RDWR);
    if (fd < 0)
        return 1;
    int payload[] = {0, mute ? 1 : 0};
    uint64_t request[] = {sizeof(payload), (uintptr_t)payload};
    int result = ioctl(fd, MI_AO_SETMUTE, request);
    close(fd);
    return result < 0 ? 2 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--mute") == 0)
        return set_audio_mute(1);
    if (argc == 2 && strcmp(argv[1], "--unmute") == 0)
        return set_audio_mute(0);

    struct fb_var_screeninfo vinfo;
    int fd = open("/dev/fb0", O_RDWR);

    if (fd < 0)
        return 1;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        close(fd);
        return 2;
    }

    /* KidsPlay page-flips between y=0 and y=480. Onion expects the first
     * framebuffer page when control returns to it. */
    vinfo.xoffset = 0;
    vinfo.yoffset = 0;
    if (ioctl(fd, FBIOPAN_DISPLAY, &vinfo) < 0) {
        close(fd);
        return 3;
    }

    close(fd);
    return 0;
}
