#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
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
