#include "host.h"
#include "cJSON/cJSON.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <poll.h>

#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <openssl/err.h>

#define H_PRINT_ERR (1 << 0)
#define H_PRINT_CMD (1 << 1)
#define H_PRINT_INFO (1 << 2)
#define H_PRINT_DISPLAY (H_PRINT_ERR | H_PRINT_INFO)
#define host_printf(type, format, ...) \
    if (H_PRINT_DISPLAY & (type))      \
        printf(format, ##__VA_ARGS__)

typedef enum
{
    CCMD_INFO = 0,
    CCMD_INIT = 1,
    CCMD_SCAN = 2,
    CCMD_PAIR = 3,
    CCMD_CONNECT = 4,
    CCMD_UNPAIR = 5,
    CCMD_DATA = 6,
    CCMD_SET_TIMEOUT = 7,
    CCMD_SET_MAXRET = 8,
    CCMD_LOGIN = -1,
    CCMD_REGISTER = -2,
    CCMD_CHANGE_PASSWORD = -3,

} client_cmd;

typedef enum
{
    HCMD_ACK = 0,
    HCMD_INFO = 1,
    HCMD_CONFIRM = -1,
} host_cmd;

struct fl_user
{
    struct fl_host *host;
    char *username;
    struct fl_dev_i **available_devs;
    int n_available_devs;
    struct fl_client **clients;
    int n_clients;
    int is_use_only;

    uint8_t password_hash[HOST_PASSWORD_HASH_SIZE];
    uint8_t password_salt[HOST_PASSWORD_SALT_SIZE];
};

struct fl_client
{
    struct fl_host *host;
    struct fl_user *user;
    struct sockaddr_in addr;

    int fd;
    SSL *ssl;
    pthread_t receive_thread;
    pthread_mutex_t ssl_mutex;
};

struct fl_host
{
    struct fl_base_i *base;
    struct fl_user **users;
    int n_users;
    struct fl_client **clients;
    int n_clients;
    struct sockaddr_in addr;

    int fd;
    SSL_CTX *ctx;
    pthread_t accept_thread;
    struct fl_client *client_pairing_dev;

    host_change_callback_t host_change_callback;
    void *host_change_private_arg;
};

static uint32_t host_rd32(const uint8_t *p)
{
    uint32_t rv;
    rv = p[3];
    rv = rv << 8 | p[2];
    rv = rv << 8 | p[1];
    rv = rv << 8 | p[0];
    return rv;
}

static void host_wr32(uint8_t *p, uint32_t val)
{
    *p++ = (uint8_t)val;
    val >>= 8;
    *p++ = (uint8_t)val;
    val >>= 8;
    *p++ = (uint8_t)val;
    val >>= 8;
    *p++ = (uint8_t)val;
}

static uint8_t host_chksum8(const uint8_t *bytes, size_t len)
{
    uint8_t chksum8 = 0;
    while (len--)
        chksum8 += *(bytes++);
    return ~chksum8;
}

static int host_random(uint8_t *dest, size_t size)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd == -1)
    {
        fd = open("/dev/random", O_RDONLY);
        if (fd == -1)
            perror("no random generator");
    }

    char *ptr = (char *)dest;
    size_t left = size;
    while (left > 0)
    {
        ssize_t bytes_read = read(fd, ptr, left);
        if (bytes_read <= 0)
        {
            close(fd);
            return EIO;
        }
        left -= bytes_read;
        ptr += bytes_read;
    }

    close(fd);
    return 0;
}

static void host_call_host_change(struct fl_host *h, struct fl_dev_i *dev, host_change_type type)
{
    if (h->host_change_callback != NULL)
        h->host_change_callback((struct fl_host_i *)h, dev, type, h->host_change_private_arg);
}

static int host_transmit(
    struct fl_client *c,
    char *json_str)
{
    size_t len = strlen(json_str);
    uint8_t head[8];
    head[0] = 0;
    strncpy((char *)&head[1], "CMD", 3);
    host_wr32(&head[4], (uint32_t)len);
    head[0] = host_chksum8(head, 8);
    uint8_t *msg = malloc(8 + len);
    memcpy(msg, head, 8);
    memcpy(&msg[8], json_str, len);

    pthread_mutex_lock(&c->ssl_mutex);
    int bytes_write = SSL_write(c->ssl, msg, 8 + len);
    pthread_mutex_unlock(&c->ssl_mutex);

    host_printf(H_PRINT_CMD, "H: %s:%hu: %s\n", inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port), json_str);

    free(msg);
    return bytes_write != 8 + len;
}

static int host_hcmd_ack(struct fl_client *c, struct fl_dev_i *dev)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddItemToObject(json, "hcmd", cJSON_CreateNumber(HCMD_ACK));
    cJSON_AddItemToObject(json, "id", cJSON_CreateNumber(dev->id));
    cJSON_AddItemToObject(json, "delay", cJSON_CreateNumber(dev->tx_packet_delay));
    cJSON_AddItemToObject(json, "count", cJSON_CreateNumber(dev->tx_packet_count));
    cJSON_AddItemToObject(json, "loss", cJSON_CreateNumber(dev->tx_packet_loss));

    char *json_str = cJSON_PrintUnformatted(json);

    int res = host_transmit(c, json_str);

    cJSON_Delete(json);
    return res;
}

static int host_hcmd_info(struct fl_client *c)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddItemToObject(json, "hcmd", cJSON_CreateNumber(HCMD_INFO));
    cJSON_AddItemToObject(json, "username", cJSON_CreateString(c->user->username));
    cJSON_AddItemToObject(json, "is_use_only", cJSON_CreateBool(c->user->is_use_only));
    cJSON *json_devs = cJSON_CreateArray();
    for (int i = 0; i < c->user->n_available_devs; i++)
    {
        struct fl_dev_i *d = c->user->available_devs[i];
        cJSON *json_dev = cJSON_CreateObject();
        cJSON_AddItemToObject(json_dev, "id", cJSON_CreateNumber(d->id));
        cJSON_AddItemToObject(json_dev, "type", cJSON_CreateNumber(d->type));
        cJSON_AddItemToObject(json_dev, "version", cJSON_CreateNumber(d->version));
        cJSON_AddItemToObject(json_dev, "name", cJSON_CreateString(d->name));
        cJSON_AddItemToObject(json_dev, "state", cJSON_CreateNumber(d->state));
        cJSON_AddItemToObject(json_dev, "timeout", cJSON_CreateNumber(d->timeout));
        cJSON_AddItemToObject(json_dev, "max_retrans", cJSON_CreateNumber(d->max_retrans));
        cJSON_AddItemToObject(json_dev, "tx_packet_delay", cJSON_CreateNumber(d->tx_packet_delay));
        cJSON_AddItemToObject(json_dev, "tx_packet_count", cJSON_CreateNumber(d->tx_packet_count));
        cJSON_AddItemToObject(json_dev, "tx_packet_loss", cJSON_CreateNumber(d->tx_packet_loss));
        cJSON_AddItemToArray(json_devs, json_dev);
    }
    cJSON_AddItemToObject(json, "devs", json_devs);
    char *json_str = cJSON_PrintUnformatted(json);

    int res = host_transmit(c, json_str);

    cJSON_Delete(json);
    return res;
}

static int host_hcmd_info_broadcast(struct fl_user *u)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddItemToObject(json, "hcmd", cJSON_CreateNumber(HCMD_INFO));
    cJSON_AddItemToObject(json, "username", cJSON_CreateString(u->username));
    cJSON_AddItemToObject(json, "is_use_only", cJSON_CreateBool(u->is_use_only));
    cJSON *json_devs = cJSON_CreateArray();
    for (int i = 0; i < u->n_available_devs; i++)
    {
        struct fl_dev_i *d = u->available_devs[i];
        cJSON *json_dev = cJSON_CreateObject();
        cJSON_AddItemToObject(json_dev, "id", cJSON_CreateNumber(d->id));
        cJSON_AddItemToObject(json_dev, "type", cJSON_CreateNumber(d->type));
        cJSON_AddItemToObject(json_dev, "version", cJSON_CreateNumber(d->version));
        cJSON_AddItemToObject(json_dev, "name", cJSON_CreateString(d->name));
        cJSON_AddItemToObject(json_dev, "state", cJSON_CreateNumber(d->state));
        cJSON_AddItemToObject(json_dev, "timeout", cJSON_CreateNumber(d->timeout));
        cJSON_AddItemToObject(json_dev, "max_retrans", cJSON_CreateNumber(d->max_retrans));
        cJSON_AddItemToObject(json_dev, "tx_packet_delay", cJSON_CreateNumber(d->tx_packet_delay));
        cJSON_AddItemToObject(json_dev, "tx_packet_count", cJSON_CreateNumber(d->tx_packet_count));
        cJSON_AddItemToObject(json_dev, "tx_packet_loss", cJSON_CreateNumber(d->tx_packet_loss));
        cJSON_AddItemToArray(json_devs, json_dev);
    }
    cJSON_AddItemToObject(json, "devs", json_devs);

    char *json_str = cJSON_PrintUnformatted(json);
    int res;
    for (int i = 0; i < u->n_clients; i++)
    {
        res = host_transmit(u->clients[i], json_str);
        if (res)
            break;
    }

    cJSON_Delete(json);
    return res;
}

static int host_hcmd_confirm(struct fl_client *c, int is_success)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddItemToObject(json, "hcmd", cJSON_CreateNumber(HCMD_CONFIRM));
    cJSON_AddItemToObject(json, "is_success", cJSON_CreateBool(is_success));

    char *json_str = cJSON_PrintUnformatted(json);

    int res = host_transmit(c, json_str);

    cJSON_Delete(json);
    return res;
}

static struct fl_user *host_user_get_by_name(
    struct fl_host *h,
    const char *username)
{
    for (int i = 0; i < h->n_users; i++)
        if (strcmp(h->users[i]->username, username) == 0)
            return h->users[i];
    return NULL;
}

static int host_user_dev_is_accessable(
    struct fl_user *u,
    struct fl_dev_i *d)
{
    for (int i = 0; i < u->n_available_devs; i++)
        if (u->available_devs[i] == d)
            return 1;
    return 0;
}

static void host_user_client_add(
    struct fl_user *u,
    struct fl_client *c)
{
    for (int i = 0; i < u->n_clients; i++)
        if (u->clients[i] == c)
            return;
    u->clients = realloc(u->clients, (u->n_clients + 1) * sizeof(struct fl_dev_i *));
    u->clients[u->n_clients] = c;
    u->n_clients++;
    c->user = u;
    host_hcmd_info(c);
}

static void host_user_client_remove(struct fl_client *c)
{
    struct fl_user *u = c->user;

    if (u == NULL)
        return;

    int index;
    for (index = 0; index < u->n_clients; index++)
        if (u->clients[index] == c)
            break;
    if (index >= u->n_clients)
        return;
    for (int i = index; i < u->n_clients - 1; i++)
        u->clients[i] = u->clients[i + 1];
    u->n_clients--;
    u->clients[u->n_clients] = NULL;
    c->user = NULL;
}

static struct fl_user *host_user_add(
    struct fl_host *h,
    const char *username,
    uint8_t password_hash[HOST_PASSWORD_HASH_SIZE],
    uint8_t password_salt[HOST_PASSWORD_SALT_SIZE],
    int is_use_only)
{
    for (int i = 0; i < h->n_users; i++)
        if (strcmp(username, h->users[i]->username) == 0)
            return NULL;

    struct fl_user *u = malloc(sizeof(struct fl_user));
    u->host = h;
    u->username = malloc(strlen(username) + 1);
    strcpy(u->username, username);
    u->available_devs = malloc(8 * sizeof(struct fl_dev_i *));
    u->n_available_devs = 0;
    u->clients = malloc(8 * sizeof(struct fl_client *));
    u->n_clients = 0;
    memcpy(u->password_hash, password_hash, HOST_PASSWORD_HASH_SIZE);
    memcpy(u->password_salt, password_salt, HOST_PASSWORD_SALT_SIZE);
    u->is_use_only = is_use_only;

    h->users = realloc(h->users, (h->n_users + 1) * sizeof(struct fl_user *));
    h->users[h->n_users] = u;
    h->n_users++;

    host_call_host_change(h, NULL, HOST_CHANGE_USER_ADD);
    return u;
}

static void host_user_remove(struct fl_user *u)
{
    struct fl_host *h = u->host;
    int index;

    for (index = 0; index < h->n_users; index++)
        if (h->users[index] == u)
            break;
    if (index >= h->n_users)
        return;

    for (int i = index; i < h->n_users - 1; i++)
        h->users[i] = h->users[i + 1];
    h->n_users--;
    h->users[h->n_users] = NULL;

    host_call_host_change(u->host, NULL, HOST_CHANGE_USER_REMOVE);
    free(u->username);
    free(u->available_devs);
    free(u->clients);
    free(u);
}

static int host_ccmd_info_handler(struct fl_client *c)
{
    return host_hcmd_info(c);
}

static int host_ccmd_init_handler(struct fl_client *c)
{

    return 0;
}

static int host_ccmd_scan_handler(struct fl_client *c)
{
    if (c->user->is_use_only)
        return 0;

    int res = fl_scan(c->host->base);
    if (res)
        host_printf(H_PRINT_ERR, "FeLink: scan ERROR : %s\n", strerror(res));

    return 0;
}

static int host_ccmd_pair_handler(struct fl_client *c, cJSON *json)
{
    if (c->user->is_use_only)
        return 0;

    cJSON *json_id = cJSON_GetObjectItemCaseSensitive(json, "id");
    if (!cJSON_IsNumber(json_id))
        return ENOMSG;
    uint32_t id = cJSON_GetNumberValue(json_id);
    struct fl_dev_i *d = fl_get_dev_by_id(c->host->base, id);
    if (d == NULL)
        return ENOMSG;

    int res = fl_pair(c->host->base, d);
    if (res)
    {
        host_printf(H_PRINT_ERR, "FeLink: pair ERROR, <%08X> : %s\n", id, strerror(res));
        return 0;
    }

    c->host->client_pairing_dev = c;

    return 0;
}

static int host_ccmd_connect_handler(struct fl_client *c, cJSON *json)
{
    if (c->user->is_use_only)
        return 0;

    cJSON *json_id = cJSON_GetObjectItemCaseSensitive(json, "id");
    if (!cJSON_IsNumber(json_id))
        return ENOMSG;
    uint32_t id = cJSON_GetNumberValue(json_id);
    struct fl_dev_i *d = fl_get_dev_by_id(c->host->base, id);
    if (d == NULL)
        return ENOMSG;

    int res = fl_connect(c->host->base, d);
    if (res)
    {
        host_printf(H_PRINT_ERR, "FeLink: connect ERROR, <%08X> : %s\n", id, strerror(res));
        return 0;
    }

    return host_hcmd_ack(c, d);
}

static int host_ccmd_unpair_handler(struct fl_client *c, cJSON *json)
{
    if (c->user->is_use_only)
        return 0;

    cJSON *json_id = cJSON_GetObjectItemCaseSensitive(json, "id");
    if (!cJSON_IsNumber(json_id))
        return ENOMSG;
    uint32_t id = cJSON_GetNumberValue(json_id);
    struct fl_dev_i *d = fl_get_dev_by_id(c->host->base, id);
    if (d == NULL)
        return ENOMSG;

    int res = fl_unpair(c->host->base, d);
    if (res)
        host_printf(H_PRINT_ERR, "FeLink: unpair ERROR, <%08X> : %s\n", id, strerror(res));

    return 0;
}

static int host_ccmd_data_handler(struct fl_client *c, cJSON *json)
{
    cJSON *json_id = cJSON_GetObjectItemCaseSensitive(json, "id");
    if (!cJSON_IsNumber(json_id))
        return ENOMSG;
    uint32_t id = cJSON_GetNumberValue(json_id);
    struct fl_dev_i *d = fl_get_dev_by_id(c->host->base, id);
    if (d == NULL)
        return 0;
    if (!host_user_dev_is_accessable(c->user, d))
        return 0;

    cJSON *json_count = cJSON_GetObjectItemCaseSensitive(json, "count");
    if (!cJSON_IsNumber(json_count))
        return ENOMSG;
    uint32_t count = cJSON_GetNumberValue(json_count);

    cJSON *json_padding_align = cJSON_GetObjectItemCaseSensitive(json, "padding_align");
    if (!cJSON_IsNumber(json_padding_align))
        return ENOMSG;
    uint32_t padding_align = cJSON_GetNumberValue(json_padding_align);

    cJSON *json_is_plaintext = cJSON_GetObjectItemCaseSensitive(json, "is_plaintext");
    if (!cJSON_IsBool(json_is_plaintext))
        return ENOMSG;

    cJSON *json_data = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (!cJSON_IsString(json_data))
        return ENOMSG;
    char *data_base64 = cJSON_GetStringValue(json_data);
    int base64_len = strlen(data_base64);
    uint8_t data[base64_len];
    EVP_DecodeBlock(data, (uint8_t *)data_base64, base64_len);

    int res = fl_data(c->host->base, d, data, count, padding_align, cJSON_IsTrue(json_is_plaintext));
    if (res)
    {
        host_printf(H_PRINT_ERR, "FeLink: send data ERROR, <%08X> : %s\n", id, strerror(res));
        return 0;
    }

    return host_hcmd_ack(c, d);
}

static int host_ccmd_set_timeout_handler(struct fl_client *c, cJSON *json)
{
    cJSON *json_id = cJSON_GetObjectItemCaseSensitive(json, "id");
    if (!cJSON_IsNumber(json_id))
        return ENOMSG;
    uint32_t id = cJSON_GetNumberValue(json_id);
    struct fl_dev_i *d = fl_get_dev_by_id(c->host->base, id);
    if (d == NULL)
        return 0;
    if (!host_user_dev_is_accessable(c->user, d))
        return 0;

    cJSON *json_timeout = cJSON_GetObjectItemCaseSensitive(json, "timeout");
    if (!cJSON_IsNumber(json_timeout))
        return ENOMSG;
    d->timeout = cJSON_GetNumberValue(json_timeout);

    return 0;
}

static int host_ccmd_set_maxret_handler(struct fl_client *c, cJSON *json)
{
    cJSON *json_id = cJSON_GetObjectItemCaseSensitive(json, "id");
    if (!cJSON_IsNumber(json_id))
        return ENOMSG;
    uint32_t id = cJSON_GetNumberValue(json_id);
    struct fl_dev_i *d = fl_get_dev_by_id(c->host->base, id);
    if (d == NULL)
        return 0;
    if (!host_user_dev_is_accessable(c->user, d))
        return 0;

    cJSON *json_max_retrans = cJSON_GetObjectItemCaseSensitive(json, "max_retrans");
    if (!cJSON_IsNumber(json_max_retrans))
        return ENOMSG;
    d->max_retrans = cJSON_GetNumberValue(json_max_retrans);

    return 0;
}

static int host_ccmd_login_handler(struct fl_client *c, cJSON *json)
{
    cJSON *json_username = cJSON_GetObjectItemCaseSensitive(json, "username");
    if (!cJSON_IsString(json_username))
        return ENOMSG;
    char *username = cJSON_GetStringValue(json_username);
    struct fl_user *u = host_user_get_by_name(c->host, username);
    if (u == NULL)
        goto host_ccmd_login_error;
    if (strcmp(c->host->users[HOST_USER_GUEST]->username, username) == 0)
        goto host_ccmd_login_success;

    cJSON *json_password = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (!cJSON_IsString(json_password))
        return ENOMSG;
    char *password = cJSON_GetStringValue(json_password);

    uint8_t password_hash[HOST_PASSWORD_HASH_SIZE];
    PKCS5_PBKDF2_HMAC_SHA1(password, -1, u->password_salt, HOST_PASSWORD_SALT_SIZE, 2048, HOST_PASSWORD_HASH_SIZE, password_hash);
    if (memcmp(password_hash, u->password_hash, HOST_PASSWORD_HASH_SIZE))
        goto host_ccmd_login_error;

host_ccmd_login_success:
    host_user_client_remove(c);
    host_user_client_add(u, c);

    host_printf(H_PRINT_INFO, "Host: client %s:%hu login <%s>\n", inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port), username);

    return host_hcmd_confirm(c, 1);
host_ccmd_login_error:
    sleep(1);
    return host_hcmd_confirm(c, 0);
}

static int host_ccmd_register_handler(struct fl_client *c, cJSON *json)
{
    if (strcmp(c->user->username, c->host->users[HOST_USER_ADMIN]->username) != 0)
        goto host_ccmd_register_error;

    cJSON *json_username = cJSON_GetObjectItemCaseSensitive(json, "username");
    if (!cJSON_IsString(json_username))
        return ENOMSG;
    char *username = cJSON_GetStringValue(json_username);

    cJSON *json_password = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (!cJSON_IsString(json_password))
        return ENOMSG;
    char *password = cJSON_GetStringValue(json_password);

    cJSON *json_is_use_only = cJSON_GetObjectItemCaseSensitive(json, "is_use_only");
    if (!cJSON_IsBool(json_is_use_only))
        return ENOMSG;

    uint8_t password_hash[HOST_PASSWORD_HASH_SIZE], password_salt[HOST_PASSWORD_SALT_SIZE];
    host_random(password_salt, HOST_PASSWORD_SALT_SIZE);
    PKCS5_PBKDF2_HMAC_SHA1(password, -1, password_salt, HOST_PASSWORD_SALT_SIZE, 2048, HOST_PASSWORD_HASH_SIZE, password_hash);

    struct fl_user *u = host_user_get_by_name(c->host, username);
    if (u != NULL)
    {
        if (!u->is_use_only)
            goto host_ccmd_register_error;
        memcpy(u->password_hash, password_hash, HOST_PASSWORD_HASH_SIZE);
        memcpy(u->password_salt, password_salt, HOST_PASSWORD_SALT_SIZE);
        u->is_use_only = cJSON_IsTrue(json_is_use_only);
        host_call_host_change(c->host, NULL, HOST_CHANGE_USER_CHANGE);
    }
    else
    {
        struct fl_user *u = host_user_add(c->host, username, password_hash, password_salt, cJSON_IsTrue(json_is_use_only));
        if (u == NULL)
            goto host_ccmd_register_error;
    }

    host_printf(H_PRINT_INFO, "Host: client %s:%hu registers <%s>\n", inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port), username);

    return host_hcmd_confirm(c, 1);
host_ccmd_register_error:
    sleep(1);
    return host_hcmd_confirm(c, 0);
}

static int host_ccmd_change_password_handler(struct fl_client *c, cJSON *json)
{
    if (c->user == NULL || c->user->is_use_only)
        goto host_ccmd_change_password_error;

    cJSON *json_username = cJSON_GetObjectItemCaseSensitive(json, "username");
    if (!cJSON_IsString(json_username))
        return ENOMSG;
    char *username = cJSON_GetStringValue(json_username);
    struct fl_user *u = host_user_get_by_name(c->host, username);
    if (u == NULL)
        goto host_ccmd_change_password_error;
    if (strcmp(u->username, c->host->users[HOST_USER_GUEST]->username) == 0)
        goto host_ccmd_change_password_error;

    cJSON *json_old_password = cJSON_GetObjectItemCaseSensitive(json, "old_password");
    if (!cJSON_IsString(json_old_password))
        return ENOMSG;
    char *old_password = cJSON_GetStringValue(json_old_password);

    uint8_t old_password_hash[HOST_PASSWORD_HASH_SIZE];
    PKCS5_PBKDF2_HMAC_SHA1(old_password, -1, u->password_salt, HOST_PASSWORD_SALT_SIZE, 2048, HOST_PASSWORD_HASH_SIZE, old_password_hash);
    if (memcmp(old_password_hash, u->password_hash, HOST_PASSWORD_HASH_SIZE))
        goto host_ccmd_change_password_error;

    cJSON *json_new_password = cJSON_GetObjectItemCaseSensitive(json, "new_password");
    if (!cJSON_IsString(json_new_password))
        return ENOMSG;
    char *new_password = cJSON_GetStringValue(json_new_password);

    uint8_t new_password_hash[HOST_PASSWORD_HASH_SIZE], password_salt[HOST_PASSWORD_SALT_SIZE];
    host_random(password_salt, HOST_PASSWORD_SALT_SIZE);
    PKCS5_PBKDF2_HMAC_SHA1(new_password, -1, password_salt, HOST_PASSWORD_SALT_SIZE, 2048, HOST_PASSWORD_HASH_SIZE, new_password_hash);
    memcpy(u->password_hash, new_password_hash, HOST_PASSWORD_HASH_SIZE);
    memcpy(u->password_salt, password_salt, HOST_PASSWORD_SALT_SIZE);

    host_printf(H_PRINT_INFO, "Host: client %s:%hu changes <%s>'s password\n", inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port), username);

    return host_hcmd_confirm(c, 1);
host_ccmd_change_password_error:
    sleep(1);
    return host_hcmd_confirm(c, 0);
}

static struct fl_client *host_client_create(
    struct fl_host *h,
    int fd,
    struct sockaddr_in addr)
{
    struct fl_client *c = malloc(sizeof(struct fl_client));
    c->host = h;
    c->addr = addr;
    c->fd = fd;
    pthread_mutex_init(&c->ssl_mutex, NULL);

    return c;
}

static void host_client_disconnected(struct fl_client *c)
{
    struct fl_host *h = c->host;

    host_user_client_remove(c);

    int index;
    for (index = 0; index < h->n_clients; index++)
        if (h->clients[index] == c)
            break;
    if (index >= h->n_clients)
        return;

    for (int i = index; i < h->n_clients - 1; i++)
        h->clients[i] = h->clients[i + 1];
    h->n_clients--;
    h->clients[h->n_clients] = NULL;

    host_call_host_change(c->host, NULL, HOST_CHANGE_CLIENT_DISCON);
    pthread_mutex_destroy(&c->ssl_mutex);
    SSL_free(c->ssl);
    close(c->fd);
    free(c);
}

static void *host_receive_thread(void *args)
{
    struct fl_client *c = args;
    int res;

    c->ssl = SSL_new(c->host->ctx);
    SSL_set_fd(c->ssl, c->fd);
    res = SSL_accept(c->ssl);
    if (res <= 0)
    {
        ERR_print_errors_fp(stderr);
        SSL_free(c->ssl);
        close(c->fd);
        free(c);
        return NULL;
    }
    res = fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL, 0) | O_NONBLOCK);
    if (res == -1)
    {
        perror("ssl fcntl");
        return NULL;
    }

    struct fl_host *h = c->host;
    h->clients = realloc(h->clients, (h->n_clients + 1) * sizeof(struct fl_client *));
    h->clients[h->n_clients] = c;
    h->n_clients++;
    host_user_client_add(h->users[HOST_USER_GUEST], c);

    host_call_host_change(c->host, NULL, HOST_CHANGE_CLIENT_CON);
    host_printf(H_PRINT_INFO, "Host: client %s:%hu connected\n", inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port));

    struct pollfd pfd;
    pfd.fd = c->fd;
    pfd.events = POLLIN;
    uint8_t head[8]; // chksum8 "CMD" len[4]
    while (1)
    {
        res = poll(&pfd, 1, -1);
        if (res < 0 || pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            goto receive_thread_exit;
        size_t bytes_read;
        pthread_mutex_lock(&c->ssl_mutex);
        res = SSL_read_ex(c->ssl, head, 8, &bytes_read);
        pthread_mutex_unlock(&c->ssl_mutex);
        if (res <= 0)
            if (SSL_get_error(c->ssl, res) != SSL_ERROR_WANT_READ)
                goto receive_thread_exit;
        if (bytes_read != 8)
            continue;

        if (strncmp((char *)&head[1], "CMD", 3) != 0 || host_chksum8(head, 8) != 0)
        {
            int exist = SSL_pending(c->ssl);
            uint8_t temp[exist];
            SSL_read_ex(c->ssl, temp, exist, &bytes_read);
            continue;
        }
        uint32_t len = host_rd32(&head[4]);

        char json_str[len + 1];
        json_str[len] = '\0';
        pthread_mutex_lock(&c->ssl_mutex);
        res = SSL_read_ex(c->ssl, json_str, len, &bytes_read);
        pthread_mutex_unlock(&c->ssl_mutex);
        if (res <= 0)
            if (SSL_get_error(c->ssl, res) != SSL_ERROR_WANT_READ)
                goto receive_thread_exit;
        if (bytes_read < len)
            continue;

        cJSON *json = cJSON_ParseWithLength(json_str, len);
        if (json == NULL)
            goto receive_thread_next;

        host_printf(H_PRINT_CMD, "C (%s:%hu, %s): %s\n", inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port), c->user->username, json_str);

        cJSON *json_cmd = cJSON_GetObjectItemCaseSensitive(json, "ccmd");
        if (!cJSON_IsNumber(json_cmd))
            goto receive_thread_next;
        client_cmd cmd = (client_cmd)cJSON_GetNumberValue(json_cmd);
        switch (cmd)
        {
        case CCMD_INFO:
            res = host_ccmd_info_handler(c);
            break;
        case CCMD_INIT:
            res = host_ccmd_init_handler(c);
            break;
        case CCMD_SCAN:
            res = host_ccmd_scan_handler(c);
            break;
        case CCMD_PAIR:
            res = host_ccmd_pair_handler(c, json);
            break;
        case CCMD_CONNECT:
            res = host_ccmd_connect_handler(c, json);
            break;
        case CCMD_UNPAIR:
            res = host_ccmd_unpair_handler(c, json);
            break;
        case CCMD_DATA:
            res = host_ccmd_data_handler(c, json);
            break;
        case CCMD_SET_TIMEOUT:
            res = host_ccmd_set_timeout_handler(c, json);
            break;
        case CCMD_SET_MAXRET:
            res = host_ccmd_set_maxret_handler(c, json);
            break;
        case CCMD_LOGIN:
            res = host_ccmd_login_handler(c, json);
            break;
        case CCMD_REGISTER:
            res = host_ccmd_register_handler(c, json);
            break;
        case CCMD_CHANGE_PASSWORD:
            res = host_ccmd_change_password_handler(c, json);
            break;
        default:
            res = 0;
            break;
        }
        if (res)
            host_printf(H_PRINT_ERR, "Host: client cmd ERROR : %s\n", strerror(res));

    receive_thread_next:
        cJSON_Delete(json);
    }

receive_thread_exit:
    ERR_print_errors_fp(stdout);
    host_printf(H_PRINT_INFO, "Host: client %s:%hu disconnected\n", inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port));
    host_client_disconnected(c);
    return NULL;
}

static void *host_accept_thread(void *args)
{
    struct fl_host *h = args;

    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(struct sockaddr_in);
    while (1)
    {
        client_fd = accept(h->fd, (struct sockaddr *)&client_addr, &len);
        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }
        struct fl_client *c = host_client_create(h, client_fd, client_addr);
        pthread_create(&c->receive_thread, NULL, host_receive_thread, c);
    }

    return NULL;
}

static void host_user_dev_add(
    struct fl_user *u,
    struct fl_dev_i *d)
{
    if (u == NULL || d == NULL)
        return;
    for (int i = 0; i < u->n_available_devs; i++)
        if (u->available_devs[i] == d)
            return;
    u->available_devs = realloc(u->available_devs, (u->n_available_devs + 1) * sizeof(struct fl_dev_i *));
    u->available_devs[u->n_available_devs] = d;
    u->n_available_devs++;
    host_hcmd_info_broadcast(u);
    host_call_host_change(u->host, d, HOST_CHANGE_USER_DEV_ADD);
}

static void host_user_dev_remove(
    struct fl_user *u,
    struct fl_dev_i *d)
{
    int index;
    for (index = 0; index < u->n_available_devs; index++)
        if (u->available_devs[index] == d)
            break;
    if (index >= u->n_available_devs)
        return;
    for (int i = index; i < u->n_available_devs - 1; i++)
        u->available_devs[i] = u->available_devs[i + 1];
    u->n_available_devs--;
    u->available_devs[u->n_available_devs] = NULL;
    host_hcmd_info_broadcast(u);
    host_call_host_change(u->host, d, HOST_CHANGE_USER_DEV_REMOVE);
}

void host_dev_change_handler(
    struct fl_base_i *base,
    struct fl_dev_i *dev,
    uint32_t old_id,
    fl_dev_change_type type,
    void *host)
{
    struct fl_host *h = host;

    switch (type)
    {
    case DEV_CHANGE_ADD:
        for (int i = 0; i < h->n_users; i++)
            if (!h->users[i]->is_use_only)
                host_user_dev_add(h->users[i], dev);
        break;
    case DEV_CHANGE_REMOVE:
        for (int i = 0; i < h->n_users; i++)
            host_user_dev_remove(h->users[i], dev);
        break;
    case DEV_CHANGE_PAIR:
        for (int i = 0; i < h->n_users; i++)
            host_user_dev_remove(h->users[i], dev);
        host_user_dev_add(h->users[HOST_USER_ADMIN], dev);
        if (h->client_pairing_dev == NULL)
            break;
        struct fl_client *c = h->client_pairing_dev;
        h->client_pairing_dev = NULL;
        host_user_dev_add(c->user, dev);
        host_printf(H_PRINT_INFO, "Host: client %s:%hu<%s> pairs device <%08X>\n", inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port), c->user->username, dev->id);
        break;
    case DEV_CHANGE_PAIR_START:
    case DEV_CHANGE_ID_CHANGE:
    case DEV_CHANGE_CONNECT:
    case DEV_CHANGE_CONNECT_TIMEOUT:
        for (int i = 0; i < h->n_users; i++)
            if (host_user_dev_is_accessable(h->users[i], dev))
                host_hcmd_info_broadcast(h->users[i]);
        break;
    default:
        for (int i = 0; i < base->n_devs; i++)
        {
            const struct fl_dev_i *d = base->devs[i];
            printf("\t<%08X> -> Type: %04hX, State: %d, Name: \'%s\'\n", d->id, d->type, d->state, d->name);
        }
        return;
    }

    host_call_host_change(h, dev, type);
    host_printf(H_PRINT_INFO, " <%08X> -> Type: %04hX, State: %d, Name: \'%s\'\n", dev->id, dev->type, dev->state, dev->name);
}

struct fl_host_i *host_init(struct fl_base_i *base)
{
    struct fl_host *h = malloc(sizeof(struct fl_host));

    h->base = base;
    h->users = malloc(8 * sizeof(struct fl_user *));
    h->n_users = 0;
    h->clients = malloc(8 * sizeof(struct fl_client *));
    h->n_clients = 0;
    h->client_pairing_dev = NULL;
    h->host_change_callback = NULL;
    h->host_change_private_arg = NULL;

    uint8_t password_hash[HOST_PASSWORD_HASH_SIZE], password_salt[HOST_PASSWORD_SALT_SIZE];
    host_random(password_salt, HOST_PASSWORD_SALT_SIZE);
    PKCS5_PBKDF2_HMAC_SHA1("123456", -1, password_salt, HOST_PASSWORD_SALT_SIZE, 2048, HOST_PASSWORD_HASH_SIZE, password_hash);
    host_user_add(h, "admin", password_hash, password_salt, 0);
    memset(password_hash, 0, HOST_PASSWORD_HASH_SIZE);
    memset(password_salt, 0, HOST_PASSWORD_SALT_SIZE);
    host_user_add(h, "guest", password_hash, password_salt, 1);

    fl_set_devs_change_callback(base, host_dev_change_handler, h);

    return (struct fl_host_i *)h;
}

int host_start(
    struct fl_host_i *host,
    uint16_t port,
    const char *cert,
    const char *key)
{
    struct fl_host *h = (struct fl_host *)host;
    int res;

    SSL_library_init();
    SSLeay_add_ssl_algorithms();
    SSL_load_error_strings();

    SSL_CTX *ctx = SSL_CTX_new(SSLv23_server_method());
    if (ctx == NULL)
    {
        ERR_print_errors_fp(stdout);
        return 1;
    }
    h->ctx = ctx;

    // openssl req -nodes -x509 -days 730 -newkey rsa:2048 -keyout cert/privatekey.pem -out cert/certificate.pem
    res = SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM);
    if (res != 1)
    {
        ERR_print_errors_fp(stdout);
        goto host_start_error;
    }
    res = SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM);
    if (res != 1)
    {
        ERR_print_errors_fp(stdout);
        goto host_start_error;
    }
    res = SSL_CTX_check_private_key(ctx);
    if (res != 1)
    {
        ERR_print_errors_fp(stdout);
        goto host_start_error;
    }

    h->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (h->fd < 0)
    {
        perror("socket");
        goto host_start_error;
    }

    int optval = 1;
    setsockopt(h->fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    h->addr.sin_family = AF_INET;
    h->addr.sin_port = htons(port);
    h->addr.sin_addr.s_addr = htonl(INADDR_ANY);

    res = bind(h->fd, (struct sockaddr *)&h->addr, sizeof(struct sockaddr_in));
    if (res)
    {
        perror("bind");
        close(h->fd);
        goto host_start_error;
    }

    res = listen(h->fd, 1);
    if (res)
    {
        perror("listen");
        close(h->fd);
        goto host_start_error;
    }

    pthread_create(&h->accept_thread, NULL, host_accept_thread, h);

    host_printf(H_PRINT_INFO, "Host: server start listening on port %hu\n", port);

    return 0;
host_start_error:
    SSL_CTX_free(ctx);
    return 1;
}

void host_stop(struct fl_host_i *host)
{
    struct fl_host *h = (struct fl_host *)host;

    fl_set_devs_change_callback(h->base, NULL, NULL);

    pthread_cancel(h->accept_thread);
    pthread_join(h->accept_thread, NULL);
    for (int i = h->n_clients - 1; i >= 0; i--)
    {
        pthread_cancel(h->clients[i]->receive_thread);
        pthread_join(h->clients[i]->receive_thread, NULL);
        host_client_disconnected(h->clients[i]);
    }
}

void host_delete(struct fl_host_i *host)
{
    struct fl_host *h = (struct fl_host *)host;

    for (int i = h->n_users - 1; i >= 0; i--)
        host_user_remove(h->users[i]);

    SSL_CTX_free(h->ctx);
    close(h->fd);
    free(h->users);
    free(h->clients);
    free(h);
}

void host_set_host_change_callback(
    struct fl_host_i *host,
    host_change_callback_t callback,
    void *private_arg)
{
    struct fl_host *h = (struct fl_host *)host;

    h->host_change_callback = callback;
    h->host_change_private_arg = private_arg;
}

struct fl_host_i *host_load(
    struct fl_base_i *base,
    const uint8_t *sav,
    size_t count)
{
    char *json_str = (char *)sav;

    struct fl_host *h = malloc(sizeof(struct fl_host));

    h->base = base;
    h->users = malloc(8 * sizeof(struct fl_user *));
    h->n_users = 0;
    h->clients = malloc(8 * sizeof(struct fl_client *));
    h->n_clients = 0;
    h->client_pairing_dev = NULL;
    h->host_change_callback = NULL;
    h->host_change_private_arg = NULL;

    cJSON *json = cJSON_ParseWithLength(json_str, count);
    if (json == NULL)
    {
        host_printf(H_PRINT_ERR, "json: %s\n", cJSON_GetErrorPtr());
        goto host_load_error_0;
    }

    cJSON *json_users = cJSON_GetObjectItemCaseSensitive(json, "users");
    if (!cJSON_IsArray(json_users) || cJSON_GetArraySize(json_users) < 2)
        goto host_load_error;
    cJSON *json_user;
    cJSON_ArrayForEach(json_user, json_users)
    {
        cJSON *json_user_username = cJSON_GetObjectItemCaseSensitive(json_user, "username");
        cJSON *json_user_available_devs_id = cJSON_GetObjectItemCaseSensitive(json_user, "available_devs_id");
        cJSON *json_user_is_use_only = cJSON_GetObjectItemCaseSensitive(json_user, "is_use_only");
        cJSON *json_user_password_hash = cJSON_GetObjectItemCaseSensitive(json_user, "password_hash");
        cJSON *json_user_password_salt = cJSON_GetObjectItemCaseSensitive(json_user, "password_salt");
        if (!cJSON_IsString(json_user_username) ||
            !cJSON_IsArray(json_user_available_devs_id) ||
            !cJSON_IsBool(json_user_is_use_only) ||
            !cJSON_IsString(json_user_password_hash) ||
            !cJSON_IsString(json_user_password_salt))
            goto host_load_error;

        char *password_hash_base64 = cJSON_GetStringValue(json_user_password_hash);
        char *password_salt_base64 = cJSON_GetStringValue(json_user_password_salt);
        int password_hash_base64_len = strlen(password_hash_base64);
        int password_salt_base64_len = strlen(password_salt_base64);
        uint8_t password_hash[password_hash_base64_len];
        uint8_t password_salt[password_salt_base64_len];
        EVP_DecodeBlock(password_hash, (uint8_t *)password_hash_base64, password_hash_base64_len);
        EVP_DecodeBlock(password_salt, (uint8_t *)password_salt_base64, password_salt_base64_len);
        host_user_add(h, cJSON_GetStringValue(json_user_username), password_hash, password_salt, cJSON_IsTrue(json_user_is_use_only));

        cJSON *json_user_available_dev_id;
        cJSON_ArrayForEach(json_user_available_dev_id, json_user_available_devs_id)
        {
            if (!cJSON_IsNumber(json_user_available_dev_id))
                goto host_load_error;
            host_user_dev_add(h->users[h->n_users - 1], fl_get_dev_by_id(base, cJSON_GetNumberValue(json_user_available_dev_id)));
        }
    }

    fl_set_devs_change_callback(base, host_dev_change_handler, h);

    cJSON_Delete(json);
    return (struct fl_host_i *)h;
host_load_error:
    cJSON_Delete(json);
host_load_error_0:
    free(h->users);
    free(h->clients);
    free(h);
    return NULL;
}

size_t host_save(
    struct fl_host_i *host,
    uint8_t **sav)
{
    struct fl_host *h = (struct fl_host *)host;

    cJSON *json = cJSON_CreateObject();
    cJSON *json_users = cJSON_CreateArray();
    for (int i = 0; i < h->n_users; i++)
    {
        struct fl_user *u = h->users[i];
        cJSON *json_user = cJSON_CreateObject();
        cJSON_AddItemToObject(json_user, "username", cJSON_CreateString(u->username));
        cJSON *json_user_available_devs_id = cJSON_CreateArray();
        for (int j = 0; j < u->n_available_devs; j++)
            if (u->available_devs[j]->state >= STATE_PAIRED)
                cJSON_AddItemToArray(json_user_available_devs_id, cJSON_CreateNumber(u->available_devs[j]->id));
        cJSON_AddItemToObject(json_user, "available_devs_id", json_user_available_devs_id);
        cJSON_AddItemToObject(json_user, "is_use_only", cJSON_CreateBool(u->is_use_only));
        char password_hash_base64[HOST_PASSWORD_HASH_SIZE * 2];
        EVP_EncodeBlock((uint8_t *)password_hash_base64, u->password_hash, HOST_PASSWORD_HASH_SIZE);
        cJSON_AddItemToObject(json_user, "password_hash", cJSON_CreateString(password_hash_base64));
        char password_salt_base64[HOST_PASSWORD_SALT_SIZE * 2];
        EVP_EncodeBlock((uint8_t *)password_salt_base64, u->password_salt, HOST_PASSWORD_SALT_SIZE);
        cJSON_AddItemToObject(json_user, "password_salt", cJSON_CreateString(password_salt_base64));
        cJSON_AddItemToArray(json_users, json_user);
    }
    cJSON_AddItemToObject(json, "users", json_users);

    char *json_str = cJSON_PrintUnformatted(json);
    int json_str_len = strlen(json_str);
    *sav = malloc(json_str_len);
    strcpy((char *)*sav, json_str);

    cJSON_Delete(json);
    return json_str_len;
}
