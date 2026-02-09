#ifndef _FELINK_HOST_
#define _FELINK_HOST_

#include "FeLinkBase/felink.h"

#include <stdint.h>
#include <arpa/inet.h>

#define HOST_USER_ADMIN 0
#define HOST_USER_GUEST (HOST_USER_ADMIN + 1)

#define HOST_PASSWORD_HASH_SIZE 32
#define HOST_PASSWORD_SALT_SIZE 32

typedef enum
{
    HOST_CHANGE_USER_ADD = -1,
    HOST_CHANGE_USER_CHANGE = -2,
    HOST_CHANGE_USER_REMOVE = -3,
    HOST_CHANGE_USER_DEV_ADD = -4,
    HOST_CHANGE_USER_DEV_REMOVE = -5,
    HOST_CHANGE_CLIENT_CON = -6,
    HOST_CHANGE_CLIENT_DISCON = -7,
} host_change_type;

struct fl_user_i
{
    const struct fl_host_i *const host;
    const char *const username;
    const struct fl_dev_i *const *const available_devs;
    const int n_available_devs;
    const struct fl_client_i *const *const clients;
    const int n_clients;
    const int is_use_only;
};

struct fl_client_i
{
    const struct fl_host_i *const host;
    struct fl_user_i *const user;
    const struct sockaddr_in addr;
};

struct fl_host_i
{
    const struct fl_base_i *const base;
    const struct fl_user_i *const *const users;
    const int n_users;
    const struct fl_client_i *const *constclients;
    const int n_clients;
    const struct sockaddr_in addr;
};

typedef void (*host_change_callback_t)(struct fl_host_i *host, struct fl_dev_i *dev, int type, void *private_arg);

void host_dev_change_handler(
    struct fl_base_i *base,
    struct fl_dev_i *dev,
    uint32_t old_id,
    fl_dev_change_type type,
    void *host);
struct fl_host_i *host_init(struct fl_base_i *base);
int host_start(
    struct fl_host_i *host,
    uint16_t port,
    const char *cert,
    const char *key);
void host_stop(struct fl_host_i *host);
void host_delete(struct fl_host_i *host);
void host_set_host_change_callback(
    struct fl_host_i *host,
    host_change_callback_t callback,
    void *private_arg);
struct fl_host_i *host_load(
    struct fl_base_i *base,
    const uint8_t *sav,
    size_t count);
size_t host_save(
    struct fl_host_i *host,
    uint8_t **sav);

#endif
