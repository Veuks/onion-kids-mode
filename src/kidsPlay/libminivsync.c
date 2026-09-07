#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/ioctl.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, unsigned int)
#endif

typedef int (*ioctl_fn)(int, unsigned long, ...);

static ioctl_fn real_ioctl;
static bool wait_supported = true;

int ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    void *argument;

    if (real_ioctl == NULL)
        real_ioctl = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");
    if (real_ioctl == NULL) {
        errno = ENOSYS;
        return -1;
    }

    va_start(args, request);
    argument = va_arg(args, void *);
    va_end(args);

    if (request == FBIOPAN_DISPLAY && wait_supported) {
        unsigned int crtc = 0;
        int wait_result = real_ioctl(fd, FBIO_WAITFORVSYNC, &crtc);
        if (wait_result < 0)
            wait_supported = false;
    }

    return real_ioctl(fd, request, argument);
}
