/* Adapted from HackiOS hackios-xnu-userland, userland/src/thermald.c
 * fcc3214 + a79ff07 (MIT). Copyright (c) 2026 OOSEX GANG (HackiOS).
 * Adaptations copyright (c) 2026 bangprovn. See LICENSE.HackiOS.
 * Publish the governor's OSThermalPressureLevel through Darwin notify.
 */
#include <errno.h>
#include <notify.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define PRESSURE_NAME "com.apple.system.thermalpressurelevel"
#define PRESSURE_SYSCTL "kern.fuxipressure"
static const char *const names[] = {
    "nominal", "moderate", "heavy", "trapping", "sleeping"
};

static int kernel_level(int *level)
{
    size_t len = sizeof(*level);
    if (sysctlbyname(PRESSURE_SYSCTL, level, &len, NULL, 0)) return -1;
    if (len != sizeof(*level) || *level < 0 || *level > 4) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

/* Commit last only after both operations succeed, so a failed publish retries
 * on the next poll. Never post after a failed state update. */
static int publish(int token, int level)
{
    uint32_t status = notify_set_state(token, (uint64_t)level);
    if (status != NOTIFY_STATUS_OK) {
        fprintf(stderr, "thermald: notify_set_state: %u\n", status);
        return -1;
    }
    status = notify_post(PRESSURE_NAME);
    if (status != NOTIFY_STATUS_OK) {
        fprintf(stderr, "thermald: notify_post: %u\n", status);
        return -1;
    }
    return 0;
}

static int serve(void)
{
    int token = -1, last = -1, reported_error = 0;
    for (;;) {
        int level;
        if (kernel_level(&level)) {
            int error = errno;
            if (error == ENOENT || error == ENOTSUP) {
                fprintf(stderr, "thermald: no pressure service; requires fuxiperf=1 fuxitp=1\n");
                if (token >= 0) notify_cancel(token);
                return 0;
            }
            /* EBUSY includes early governor initialization. Keep the previous
             * notification on any read failure; never invent a nominal sample. */
            if (error != reported_error) {
                fprintf(stderr, "thermald: %s: %s\n", PRESSURE_SYSCTL, strerror(error));
                reported_error = error;
            }
        } else {
            reported_error = 0;
            if (token < 0 && notify_register_check(PRESSURE_NAME, &token) != NOTIFY_STATUS_OK) {
                fprintf(stderr, "thermald: cannot register %s\n", PRESSURE_NAME);
                return 1;
            }
            if (level != last && !publish(token, level)) {
                printf("thermald: pressure %s (%d)\n", names[level], level);
                fflush(stdout);
                last = level;
            }
        }
        sleep(1);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && (!strcmp(argv[1], "show") || !strcmp(argv[1], "level"))) {
        int level;
        if (kernel_level(&level)) {
            fprintf(stderr, "thermald: %s: %s (requires fuxiperf=1 fuxitp=1)\n",
                    PRESSURE_SYSCTL, strerror(errno));
            return 1;
        }
        printf("%s: %d %s\n", PRESSURE_SYSCTL, level, names[level]);
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: thermald [show|level]\n");
        return 2;
    }
    return serve();
}
