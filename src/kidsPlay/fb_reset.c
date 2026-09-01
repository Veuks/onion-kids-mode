#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define MI_AO_SETMUTE 0x4008690d
#define MI_AO_SETVOLUME 0x4008690b
#define MI_AO_GETVOLUME 0xc008690c

static int get_audio_volume(void)
{
    int fd = open("/dev/mi_ao", O_RDWR);
    if (fd < 0)
        return -60;
    int payload[] = {0, -60};
    uint64_t request[] = {sizeof(payload), (uintptr_t)payload};
    if (ioctl(fd, MI_AO_GETVOLUME, request) < 0)
        payload[1] = -60;
    close(fd);
    return payload[1];
}

static void set_audio_volume(int raw)
{
    if (raw < -60)
        raw = -60;
    if (raw > 30)
        raw = 30;
    int fd = open("/dev/mi_ao", O_RDWR);
    if (fd < 0)
        return;
    int payload[] = {0, raw};
    uint64_t request[] = {sizeof(payload), (uintptr_t)payload};
    ioctl(fd, MI_AO_SETVOLUME, request);
    close(fd);
}

static void fade_audio_to(int target)
{
    int start = get_audio_volume();
    for (int i = 1; i <= 8; i++) {
        int value = start + (target - start) * i / 8;
        set_audio_volume(value);
        usleep(15000);
    }
}

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
    if (argc == 3 && strcmp(argv[1], "--fade-to") == 0) {
        fade_audio_to(atoi(argv[2]));
        return 0;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    int fd = open("/dev/fb0", O_RDWR);

    if (fd < 0)
        return 1;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        close(fd);
        return 2;
    }

    /* Both SDL generations use the same two framebuffer pages.  A player or
     * short-lived parent-menu process can terminate while the hidden page
     * still contains a frame prepared in the other driver's orientation.
     * Clear every mapped page before handing display ownership to the next
     * process, rather than merely selecting page zero and allowing the stale
     * page to reappear on its first flip. */
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == 0 && finfo.smem_len > 0) {
        void *pages = mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
        if (pages != MAP_FAILED) {
            memset(pages, 0, finfo.smem_len);
            msync(pages, finfo.smem_len, MS_SYNC);
            munmap(pages, finfo.smem_len);
        }
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
