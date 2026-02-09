#ifndef _FELINK_BASE_FELINK_
#define FELINK_BASE_VERSION 0x0002
#define FELINK_DEV_VERSION_MAX_COMPATIBILITY 0x0002
#define FELINK_DEV_VERSION_MIN_COMPATIBILITY 0x0002
#define _FELINK_BASE_FELINK_ FELINK_BASE_VERSION

#include "userconf.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <errno.h>

typedef enum
{
    STATE_HANDSHAKED = 0,
    STATE_PAIRING = 1,
    STATE_PAIRED = 2,
    STATE_CONNECTED = 3,
} fl_state;

typedef enum
{
    DEV_CHANGE_ADD = 0,
    DEV_CHANGE_REMOVE = 1,
    DEV_CHANGE_ID_CHANGE = 2,
    DEV_CHANGE_PAIR_START = 3,
    DEV_CHANGE_PAIR = 4,
    DEV_CHANGE_CONNECT = 5,
    DEV_CHANGE_CONNECT_TIMEOUT = 6,
} fl_dev_change_type;

struct fl_dev_i
{
    const struct flbase *const base;

    const uint32_t id;
    const uint32_t type;
    const uint16_t version;
    const char *name;
    const fl_state state;
    uint16_t timeout;
    uint8_t max_retrans;
    const time_t tx_packet_delay;
    const size_t tx_packet_count;
    const size_t tx_packet_loss;
    const uint32_t salt;
};

struct fl_base_i
{
    const struct fl_dev_i *const *const devs;
    const int n_devs;
};

typedef int (*fl_tx_func_t)(struct fl_dev_i *dev, uint8_t *buf, size_t count, void *private_arg);
typedef void (*fl_devs_change_callback_t)(struct fl_base_i *base, struct fl_dev_i *dev, uint32_t old_id, fl_dev_change_type type, void *private_arg);

int fl_receive_handler(
    struct fl_base_i *base,
    const uint8_t *buf,
    size_t count);
struct fl_dev_i *fl_get_dev_by_id(
    struct fl_base_i *b,
    uint32_t id);
int fl_scan(
    struct fl_base_i *base);
int fl_pair(
    struct fl_base_i *base,
    const struct fl_dev_i *dev);
int fl_connect(
    struct fl_base_i *base,
    const struct fl_dev_i *dev);
int fl_unpair(
    struct fl_base_i *base,
    const struct fl_dev_i *dev);
int fl_data(
    struct fl_base_i *base,
    const struct fl_dev_i *dev,
    const uint8_t *buf,
    size_t count,
    size_t padding_align,
    int is_plaintext);
struct fl_base_i *fl_init(void);
void fl_delete(
    struct fl_base_i *base);
void fl_set_tx_func(
    struct fl_base_i *base,
    fl_tx_func_t func,
    void *private_arg);
void fl_set_devs_change_callback(
    struct fl_base_i *base,
    fl_devs_change_callback_t callback,
    void *private_arg);
struct fl_base_i *fl_load(
    const uint8_t *sav,
    size_t count);
size_t fl_save(
    struct fl_base_i *base,
    uint8_t **sav);

#endif // !_FELINK_BASE_FELINK_
