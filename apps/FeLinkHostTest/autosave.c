#define _POSIX_C_SOURCE 199309L

#include "autosave.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/timerfd.h>
#include <time.h>

struct fl_autosave
{
    struct fl_base_i *base;
    struct fl_host_i *host;
    time_t idle_secs;

    int timer_fd;
    pthread_t save_thread;
    int changes;
};

static void *autosave_save_thread(void *args)
{
    struct fl_autosave *as = args;
    int res;

    while (1)
    {
        uint64_t count;
        res = read(as->timer_fd, &count, sizeof(uint64_t));
        pthread_testcancel();
        if (res <= 0)
        {
            perror("autosave read timer");
            return NULL;
        }

        printf("Autosave: save\n");
        if (as->changes & AUTOSAVE_BASE_CHANGE)
        {
            int felink_sav_fd = open(AUTOSAVE_BASE_FILE, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU);
            if (felink_sav_fd < 0)
            {
                perror("Autosave: open " AUTOSAVE_BASE_FILE);
                goto save_thread_base_save_skip;
            }
            uint8_t *sav_buf;
            size_t sav_size;
            sav_size = fl_save(as->base, &sav_buf);
            write(felink_sav_fd, sav_buf, sav_size);
            close(felink_sav_fd);
            free(sav_buf);
        }
    save_thread_base_save_skip:
        if (as->changes & AUTOSAVE_HOST_CHANGE)
        {
            int host_sav_fd = open(AUTOSAVE_HOST_FILE, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU);
            if (host_sav_fd < 0)
            {
                perror("Autosave: open " AUTOSAVE_HOST_FILE);
                goto save_thread_host_save_skip;
            }
            uint8_t *sav_buf;
            size_t sav_size;
            sav_size = host_save(as->host, &sav_buf);
            write(host_sav_fd, sav_buf, sav_size);
            close(host_sav_fd);
            free(sav_buf);
        }
    save_thread_host_save_skip:
        as->changes = 0;
    }

    return NULL;
}

int autosave_add_change(
    struct fl_autosave_i *autosave,
    int changes)
{
    struct fl_autosave *as = (struct fl_autosave *)autosave;
    struct itimerspec timeval;

    as->changes |= changes;
    timeval.it_value.tv_nsec = 0;
    timeval.it_value.tv_sec = as->idle_secs;
    timeval.it_interval.tv_nsec = 0;
    timeval.it_interval.tv_sec = 0;
    return timerfd_settime(as->timer_fd, 0, &timeval, NULL);
}

struct fl_autosave_i *autosave_start(
    struct fl_base_i *base,
    struct fl_host_i *host,
    time_t save_idle_secs)
{
    struct fl_autosave *as = malloc(sizeof(struct fl_autosave));

    as->base = base;
    as->host = host;
    as->idle_secs = save_idle_secs;
    as->timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (as->timer_fd <= 0)
        goto autosave_start_error;
    pthread_create(&as->save_thread, NULL, autosave_save_thread, as);

    return (struct fl_autosave_i *)as;
autosave_start_error:
    free(as);
    return NULL;
}

void autosave_stop(struct fl_autosave_i *autosave)
{
    struct fl_autosave *as = (struct fl_autosave *)autosave;

    pthread_cancel(as->save_thread);

    struct itimerspec timeval;
    timeval.it_value.tv_nsec = 0;
    timeval.it_value.tv_sec = 0;
    timeval.it_interval.tv_nsec = 0;
    timeval.it_interval.tv_sec = 0;
    timerfd_settime(as->timer_fd, 0, &timeval, NULL);

    pthread_join(as->save_thread, NULL);
    close(as->timer_fd);
    free(as);
}
