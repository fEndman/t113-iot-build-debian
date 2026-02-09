#ifndef _FELINK_AUTOSAVE_
#define _FELINK_AUTOSAVE_

#include "FeLinkBase/felink.h"
#include "host.h"

#include <stdint.h>
#include <sys/time.h>
#include <errno.h>

#define AUTOSAVE_BASE_FILE "felink.sav"
#define AUTOSAVE_HOST_FILE "host.sav"

#define AUTOSAVE_BASE_CHANGE (1 << 0)
#define AUTOSAVE_HOST_CHANGE (1 << 1)

struct fl_autosave_i
{
    const struct fl_base_i *const base;
    const struct fl_host_i *const host;
    time_t idle_secs;
};

int autosave_add_change(
    struct fl_autosave_i *autosave,
    int changes);
struct fl_autosave_i *autosave_start(
    struct fl_base_i *base,
    struct fl_host_i *host,
    time_t save_idle_secs);
void autosave_stop(struct fl_autosave_i *autosave);

#endif