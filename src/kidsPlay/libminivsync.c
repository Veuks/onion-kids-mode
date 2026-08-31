#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, unsigned int)
#endif

#define VSYNC_LOG "/mnt/SDCARD/.tmp_update/logs/kidsplay-vsync.log"

typedef int (*ioctl_fn)(int, unsigned long, ...);

static ioctl_fn real_ioctl;
static bool first_pan = true;
static bool wait_supported = true;

static unsigned long monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000UL +
           (unsigned long)ts.tv_nsec / 1000UL;
}

static void log_first_result(const char *status, unsigned long wait_us,
                             int error_number)
{
    FILE *file = fopen(VSYNC_LOG, "w");
    if (file == NULL)
        return;

    fprintf(file,
            "status=%s\n"
            "hook=FBIOPAN_DISPLAY\n"
            "wait_us=%lu\n"
            "errno=%d\n"
            "error=%s\n",
            status, wait_us, error_number,
            error_number != 0 ? strerror(error_number) : "none");
    fclose(file);
}

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
        unsigned long started = monotonic_us();
        int wait_result = real_ioctl(fd, FBIO_WAITFORVSYNC, &crtc);
        unsigned long elapsed = monotonic_us() - started;

        if (wait_result < 0) {
            int wait_error = errno;
            wait_supported = false;
            if (first_pan)
                log_first_result("vsync-not-supported", elapsed, wait_error);
        }
        else if (first_pan) {
            log_first_result("vsync-supported", elapsed, 0);
        }
        first_pan = false;
    }

    return real_ioctl(fd, request, argument);
}
