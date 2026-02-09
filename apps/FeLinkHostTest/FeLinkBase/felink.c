#include "felink.h"
#include "micro-ecc/uECC.h"
#include "tea.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>

#define FELINK_SIGN 0xFE

#define FELINK_PCMD 0xAA
#define FELINK_CCMD 0xCC
#define FELINK_DCMD 0xDD

#define FELINK_PCMD_BASE_SEARCH 0xAA
#define FELINK_PCMD_DEV_HANDSHAKE 0xBA
#define FELINK_PCMD_BASE_PAIR 0xAB
#define FELINK_PCMD_DEV_PAIR 0xBB
#define FELINK_PCMD_BASE_ID_INVALID 0xAE
#define FELINK_PCMD_DEV_ID_CHANGE 0xBE

#define FELINK_CCMD_DEV_ACK 0xBA
#define FELINK_CCMD_BASE_CONNECT 0xAC
#define FELINK_CCMD_BASE_UNPAIR 0xAB

#define FELINK_DCMD_DATA 0xAD

struct fl_msg_t
{
    uint8_t *buf;
    uint32_t count;
    uint8_t sign;
};

struct fl_dev
{
    struct fl_base *base;

    uint32_t id;
    uint32_t type;
    uint16_t version;
    char *name;
    fl_state state;
    uint16_t timeout;
    uint8_t max_retrans;
    time_t tx_packet_delay;
    size_t tx_packet_count;
    size_t tx_packet_loss;
    uint32_t salt;

    uint32_t connect_count;
    uint8_t *tea_key;

    uint8_t ack_sign;
    pthread_cond_t ack_cond;
    pthread_mutex_t ack_mutex;
};

struct fl_base
{
    struct fl_dev **devs;
    int n_devs;

    fl_tx_func_t tx_func;
    void *tx_func_private_arg;
    fl_devs_change_callback_t devs_change_callback;
    void *devs_change_private_arg;
    uint8_t *pri_key;
    uint8_t *pub_key;
};

static uint16_t fl_rd16(const uint8_t *p)
{
    uint16_t rv;
    rv = p[1];
    rv = rv << 8 | p[0];
    return rv;
}

static uint32_t fl_rd32(const uint8_t *p)
{
    uint32_t rv;
    rv = p[3];
    rv = rv << 8 | p[2];
    rv = rv << 8 | p[1];
    rv = rv << 8 | p[0];
    return rv;
}

static void fl_wr16(uint8_t *p, uint16_t val)
{
    *p++ = (uint8_t)val;
    val >>= 8;
    *p++ = (uint8_t)val;
}

static void fl_wr32(uint8_t *p, uint32_t val)
{
    *p++ = (uint8_t)val;
    val >>= 8;
    *p++ = (uint8_t)val;
    val >>= 8;
    *p++ = (uint8_t)val;
    val >>= 8;
    *p++ = (uint8_t)val;
}

static uint8_t fl_chksum8(const uint8_t *bytes, size_t len)
{
    uint8_t chksum8 = 0;
    while (len--)
        chksum8 += *(bytes++);
    return ~chksum8;
}

static int fl_random(uint8_t *dest, size_t size)
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

static void fl_timespec_add_ns(struct timespec *t, uint64_t ns)
{
    uint64_t temp = t->tv_nsec + ns;
    t->tv_nsec = temp % 1000000000;
    t->tv_sec += temp / 1000000000;
}

static time_t fl_timeval_interval_us(struct timeval start, struct timeval end)
{
    return (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
}

static void fl_call_devs_change(struct fl_base *b, struct fl_dev *d, uint32_t id, fl_dev_change_type type)
{
    if (b->devs_change_callback != NULL)
        b->devs_change_callback((struct fl_base_i *)b, (struct fl_dev_i *)d, id, type, b->devs_change_private_arg);
}

static struct fl_msg_t *fl_msg_create(size_t count)
{
    struct fl_msg_t *msg = malloc(sizeof(struct fl_msg_t));
    if (msg == NULL)
        return NULL;
    msg->buf = malloc(count);
    if (msg->buf == NULL)
    {
        free(msg);
        return NULL;
    }
    msg->count = count;

    return msg;
}

static void fl_msg_delete(
    struct fl_msg_t *msg)
{
    free(msg->buf);
    free(msg);
}

static struct fl_dev *fl_dev_add(
    struct fl_base *b,
    uint32_t id,
    uint32_t type,
    uint16_t version,
    const char *name)
{
    struct fl_dev *d = malloc(sizeof(struct fl_dev));

    d->base = b;
    d->id = id;
    d->type = type;
    d->version = version;
    d->name = malloc(strlen(name) + 1);
    strcpy(d->name, name);
    d->state = STATE_HANDSHAKED;
    d->timeout = FELINK_DEFAULT_TIMEOUT;
    d->max_retrans = FELINK_DEFAULT_MAXRET;
    d->tx_packet_delay = -1;
    d->tx_packet_count = 0;
    d->tx_packet_loss = 0;
    d->tea_key = malloc(FELINK_uECC_CURVE_SIZE);
    d->ack_sign = 0;
    pthread_cond_init(&d->ack_cond, NULL);
    pthread_mutex_init(&d->ack_mutex, NULL);

    b->devs = realloc(b->devs, (b->n_devs + 1) * sizeof(struct fl_dev *));
    b->devs[b->n_devs] = d;
    b->n_devs++;

    fl_call_devs_change(b, d, id, DEV_CHANGE_ADD);
    return d;
}

static void fl_dev_remove(
    struct fl_dev *d)
{
    int index;
    struct fl_base *b = d->base;

    for (index = 0; index < b->n_devs; index++)
        if (b->devs[index] == d)
            break;
    if (index == b->n_devs)
        return;

    pthread_cond_destroy(&d->ack_cond);
    pthread_mutex_destroy(&d->ack_mutex);

    fl_call_devs_change(b, d, d->id, DEV_CHANGE_REMOVE);

    free(d->name);
    free(d->tea_key);
    free(d);

    for (int i = index; i < b->n_devs - 1; i++)
        b->devs[i] = b->devs[i + 1];
    b->n_devs--;
    b->devs[b->n_devs] = NULL;
}

static struct fl_dev *fl_base_get_dev_by_id(
    struct fl_base *b,
    uint32_t id)
{
    for (int i = 0; i < b->n_devs; i++)
        if (b->devs[i]->id == id)
            return b->devs[i];
    return NULL;
}

static int fl_base_is_dev_valid(
    struct fl_base *b,
    struct fl_dev *d)
{
    if (d == NULL || b->n_devs == 0 || d->version > FELINK_DEV_VERSION_MAX_COMPATIBILITY || d->version < FELINK_DEV_VERSION_MIN_COMPATIBILITY)
        return 0;
    for (int i = 0; i < b->n_devs; i++)
        if (b->devs[i] == d)
            return 1;
    return 0;
}

static void fl_get_key(
    struct fl_dev *d,
    uint32_t key[4])
{
    uint8_t *key_le = (uint8_t *)key;
    const uint8_t *keys = d->tea_key;
    uint32_t salt = d->salt;
    uint32_t salt_rev = ~salt;
    uint32_t salt_m2 = salt + salt;
    uint32_t salt_rev_m2 = salt_rev + salt_rev;

    for (int i = 0; i < 16; i += 4)
    {
        key_le[i] = keys[salt & 0xF];
        key_le[i + 1] = keys[salt_rev & 0xF];
        key_le[i + 2] = keys[salt_m2 & 0xF];
        key_le[i + 3] = keys[salt_rev_m2 & 0xF];
        salt >>= 4;
        salt_rev >>= 4;
        salt_m2 >>= 4;
        salt_rev_m2 >>= 4;
    }
}

static int fl_transmit_no_ack(
    struct fl_base *b,
    struct fl_msg_t *msg)
{
    if (b->tx_func == NULL)
        return ENODEV;
    int res = b->tx_func(NULL, msg->buf, msg->count, b->tx_func_private_arg);
    fl_msg_delete(msg);
    return res;
}

static int fl_transmit_with_ack(
    struct fl_dev *d,
    struct fl_msg_t *m)
{
    struct fl_base *b = d->base;
    int res = 0;

    if (b->tx_func == NULL)
        return ENODEV;
    struct timeval send_time;
    gettimeofday(&send_time, NULL);
    int tries;
    for (tries = 0; tries < d->max_retrans; tries++)
    {
        res = b->tx_func((struct fl_dev_i *)d, m->buf, m->count, b->tx_func_private_arg);
        if (res)
            break;
        d->tx_packet_count++;

        struct timespec t;
        timespec_get(&t, TIME_UTC);
        fl_timespec_add_ns(&t, d->timeout * 1000000);
        int is_timeout;
        do
        {
            is_timeout = pthread_cond_timedwait(&d->ack_cond, &d->ack_mutex, &t);
        } while (!is_timeout && d->ack_sign != m->sign);

        if (!is_timeout)
            break;
        else
            d->tx_packet_loss++;
    }
    fl_msg_delete(m);

    if (res || tries == d->max_retrans)
    {
        d->state = STATE_PAIRED;
        d->tx_packet_delay = -1;
        fl_call_devs_change(d->base, d, d->id, DEV_CHANGE_CONNECT_TIMEOUT);
        return res ? res : ETIMEDOUT;
    }
    else
    {
        struct timeval ack_time;
        gettimeofday(&ack_time, NULL);
        d->tx_packet_delay = fl_timeval_interval_us(send_time, ack_time) / 1000;
        return 0;
    }
}

static int fl_pcmd_base_search(
    struct fl_base *b)
{
    struct fl_msg_t *msg = fl_msg_create(4);

    msg->buf[0] = FELINK_SIGN;
    msg->buf[1] = 0;
    msg->buf[2] = FELINK_PCMD;
    msg->buf[3] = FELINK_PCMD_BASE_SEARCH;
    msg->buf[1] = fl_chksum8(msg->buf, msg->count);

    return fl_transmit_no_ack(b, msg);
}

static int fl_pcmd_base_id_invalid(
    struct fl_base *b,
    uint32_t id)
{
    struct fl_msg_t *msg = fl_msg_create(12);

    msg->buf[0] = FELINK_SIGN;
    msg->buf[1] = 0;
    msg->buf[2] = FELINK_PCMD;
    msg->buf[3] = FELINK_PCMD_BASE_ID_INVALID;
    fl_wr32(&msg->buf[4], id);
    int res = fl_random(&msg->buf[8], 4);
    if (res)
    {
        fl_msg_delete(msg);
        return res;
    }
    msg->buf[1] = fl_chksum8(msg->buf, msg->count);

    return fl_transmit_no_ack(b, msg);
}

static int fl_pcmd_base_pair(
    struct fl_base *b,
    struct fl_dev *d)
{
    size_t pub_key_size = FELINK_uECC_PUB_KEY_SIZE;
    struct fl_msg_t *msg = fl_msg_create(9 + pub_key_size);

    msg->buf[0] = FELINK_SIGN;
    msg->buf[1] = 0;
    msg->buf[2] = FELINK_PCMD;
    msg->buf[3] = FELINK_PCMD_BASE_PAIR;
    fl_wr32(&msg->buf[4], d->id);
    msg->buf[8] = pub_key_size;
    memcpy(&msg->buf[9], b->pub_key, pub_key_size);
    msg->buf[1] = fl_chksum8(msg->buf, msg->count);

    int res = fl_transmit_no_ack(b, msg);
    if (res)
        return res;

    d->state = STATE_PAIRING;
    fl_call_devs_change(b, d, d->id, DEV_CHANGE_PAIR_START);

    return 0;
}

static int fl_ccmd_base_connect(
    struct fl_base *b,
    struct fl_dev *d)
{
    struct fl_msg_t *msg = fl_msg_create(16);

    msg->buf[0] = FELINK_SIGN;
    msg->buf[1] = 0;
    msg->buf[2] = FELINK_CCMD;
    msg->buf[3] = FELINK_CCMD_BASE_CONNECT;
    fl_wr32(&msg->buf[4], d->id);
    msg->buf[8] = 0;
    int res = fl_random(&msg->buf[9], 3);
    if (res)
    {
        fl_msg_delete(msg);
        return res;
    }
    fl_wr32(&msg->buf[12], d->connect_count);
    msg->buf[8] = fl_chksum8(&msg->buf[8], 8);
    uint32_t salt = fl_rd32(&msg->buf[8]);
    uint32_t key[4];
    d->salt = 0x76543210;
    fl_get_key(d, key);
    res = fl_xxtea_byte_array_encrypt(&msg->buf[8], 8, key);
    if (res)
    {
        fl_msg_delete(msg);
        return res;
    }
    msg->buf[1] = fl_chksum8(msg->buf, msg->count);
    msg->sign = fl_chksum8(&msg->buf[8], 8);

    res = fl_transmit_with_ack(d, msg);
    if (res)
        return res;

    d->connect_count++;
    d->salt = salt;
    d->state = STATE_CONNECTED;
    fl_call_devs_change(b, d, d->id, DEV_CHANGE_CONNECT);

    return 0;
}

static int fl_ccmd_base_unpair(
    struct fl_base *b,
    struct fl_dev *d)
{
    struct fl_msg_t *msg = fl_msg_create(16);

    msg->buf[0] = FELINK_SIGN;
    msg->buf[1] = 0;
    msg->buf[2] = FELINK_CCMD;
    msg->buf[3] = FELINK_CCMD_BASE_UNPAIR;
    fl_wr32(&msg->buf[4], d->id);
    msg->buf[8] = 0;
    int res = fl_random(&msg->buf[9], 3);
    if (res)
    {
        fl_msg_delete(msg);
        return res;
    }
    fl_wr32(&msg->buf[12], d->connect_count);
    msg->buf[8] = fl_chksum8(&msg->buf[8], 8);
    uint32_t key[4];
    d->salt = 0x01234567;
    fl_get_key(d, key);
    res = fl_xxtea_byte_array_encrypt(&msg->buf[8], 8, key);
    if (res)
    {
        fl_msg_delete(msg);
        return res;
    }
    msg->buf[1] = fl_chksum8(msg->buf, msg->count);

    res = fl_transmit_no_ack(b, msg);

    fl_dev_remove(d);

    return res;
}

static int fl_dcmd_data(
    struct fl_base *b,
    struct fl_dev *d,
    const uint8_t *data,
    size_t count,
    size_t padding_align,
    char is_plaintext_transmit)
{
    if (count == 0)
        return ENODATA;
    if (count > 0xFFFF)
        return EMSGSIZE;

    uint32_t l = ((6 + (count > padding_align ? count : padding_align) + 3) / 4) * 4;
    struct fl_msg_t *msg = fl_msg_create(12 + l);

    msg->buf[0] = FELINK_SIGN;
    msg->buf[1] = 0;
    msg->buf[2] = FELINK_DCMD;
    msg->buf[3] = FELINK_DCMD_DATA;
    fl_wr32(&msg->buf[4], d->id);
    fl_wr32(&msg->buf[8], (uint32_t)(is_plaintext_transmit ? -l : l));
    fl_wr16(&msg->buf[12], count - 1);
    msg->buf[14] = 0;
    int res = fl_random(&msg->buf[15], 4);
    if (res)
    {
        fl_msg_delete(msg);
        return res;
    }
    memcpy(&msg->buf[18], data, count);
    msg->buf[14] = fl_chksum8(&msg->buf[12], l);
    uint32_t salt = fl_rd32(&msg->buf[14]);
    if (!is_plaintext_transmit)
    {
        uint32_t key[4];
        fl_get_key(d, key);
        res = fl_xxtea_byte_array_encrypt(&msg->buf[12], l, key);
        if (res)
        {
            fl_msg_delete(msg);
            return res;
        }
    }
    msg->buf[1] = fl_chksum8(msg->buf, msg->count);
    msg->sign = fl_chksum8(&msg->buf[12], l);

    res = fl_transmit_with_ack(d, msg);
    if (!res && !is_plaintext_transmit)
        d->salt = salt;

    return res;
}

static int fl_pcmd_dev_handshake_handler(
    struct fl_base *b,
    const uint8_t *data,
    size_t count)
{
    uint32_t id = fl_rd32(&data[4]);

    struct fl_dev *d = fl_base_get_dev_by_id(b, id);
    if (d != NULL)
        return fl_pcmd_base_id_invalid(b, id);

    uint32_t type = fl_rd32(&data[8]);
    uint16_t version = fl_rd16(&data[12]);
    fl_dev_add(b, id, type, version, (char *)&data[14]);

    return 0;
}

static int fl_pcmd_dev_pair_handler(
    struct fl_base *b,
    const uint8_t *data,
    size_t count)
{
    uint32_t id = fl_rd32(&data[4]);

    struct fl_dev *d = fl_base_get_dev_by_id(b, id);
    if (d != NULL && d->state == STATE_PAIRING)
    {
        uint8_t n = data[8];
        if (n != FELINK_uECC_PUB_KEY_SIZE)
            return EPROTONOSUPPORT;

        uECC_shared_secret(&data[9], b->pri_key, d->tea_key, FELINK_uECC_CURVE);
        fl_random((uint8_t *)&d->connect_count, 3);
        d->state = STATE_PAIRED;
        fl_call_devs_change(b, d, d->id, DEV_CHANGE_PAIR);
    }

    return 0;
}

static int fl_pcmd_dev_id_change_handler(
    struct fl_base *b,
    const uint8_t *data,
    size_t count)
{
    uint32_t nid = fl_rd32(&data[4]);
    uint32_t oid = fl_rd32(&data[8]);

    struct fl_dev *d = fl_base_get_dev_by_id(b, nid);
    if (d != NULL)
        return fl_pcmd_base_id_invalid(b, nid);

    d = fl_base_get_dev_by_id(b, oid);
    if (d == NULL)
    {
        uint16_t type = fl_rd16(&data[12]);
        uint16_t version = fl_rd16(&data[16]);
        fl_dev_add(b, nid, type, version, (char *)&data[18]);
    }
    else
    {
        uint32_t oid = d->id;
        d->id = nid;
        fl_call_devs_change(b, d, oid, DEV_CHANGE_ID_CHANGE);
    }

    return 0;
}

static int fl_pcmd_handler(
    struct fl_base *b,
    const uint8_t *data,
    size_t count)
{
    uint8_t minor_cmd = data[3];
    switch (minor_cmd)
    {
    case FELINK_PCMD_DEV_HANDSHAKE:
        return fl_pcmd_dev_handshake_handler(b, data, count);
    case FELINK_PCMD_DEV_PAIR:
        return fl_pcmd_dev_pair_handler(b, data, count);
    case FELINK_PCMD_DEV_ID_CHANGE:
        return fl_pcmd_dev_id_change_handler(b, data, count);
    case FELINK_PCMD_BASE_SEARCH:
    case FELINK_PCMD_BASE_PAIR:
    case FELINK_PCMD_BASE_ID_INVALID:
        return 0;
    default:
        return EBADMSG;
    }
}

static int fl_ccmd_dev_ack_handler(
    struct fl_base *b,
    const uint8_t *data,
    size_t count)
{
    uint32_t id = fl_rd32(&data[4]);

    struct fl_dev *d = fl_base_get_dev_by_id(b, id);
    if (d != NULL)
    {
        pthread_mutex_lock(&d->ack_mutex);
        d->ack_sign = data[8];
        pthread_mutex_unlock(&d->ack_mutex);
        pthread_cond_signal(&d->ack_cond);
    }

    return 0;
}

static int fl_ccmd_handler(
    struct fl_base *b,
    const uint8_t *data,
    size_t count)
{
    uint8_t minor_cmd = data[3];
    switch (minor_cmd)
    {
    case FELINK_CCMD_DEV_ACK:
        return fl_ccmd_dev_ack_handler(b, data, count);
    case FELINK_CCMD_BASE_CONNECT:
    case FELINK_CCMD_BASE_UNPAIR:
        return 0;
    default:
        return EBADMSG;
    }
}

int fl_receive_handler(
    struct fl_base_i *base,
    const uint8_t *buf,
    size_t count)
{
    struct fl_base *b = (struct fl_base *)base;

    if (buf[0] != FELINK_SIGN)
        return ENODATA;
    if (count < 4)
        return EBADMSG;

    uint8_t minor_cmd = buf[3];
    if (minor_cmd != FELINK_PCMD_BASE_SEARCH)
        if (count < 8)
            return EBADMSG;

    if (fl_chksum8(buf, count) != 0)
        return EBADMSG;

    uint8_t major_cmd = buf[2];
    switch (major_cmd)
    {
    case FELINK_PCMD:
        return fl_pcmd_handler(b, buf, count);
    case FELINK_CCMD:
        return fl_ccmd_handler(b, buf, count);
    case FELINK_DCMD:
        return 0;
    default:
        return EBADMSG;
    }
}

struct fl_dev_i *fl_get_dev_by_id(
    struct fl_base_i *b,
    uint32_t id)
{
    return (struct fl_dev_i *)fl_base_get_dev_by_id((struct fl_base *)b, id);
}

int fl_scan(
    struct fl_base_i *base)
{
    struct fl_base *b = (struct fl_base *)base;

    for (int i = b->n_devs - 1; i >= 0; i--)
        if (b->devs[i]->state == STATE_HANDSHAKED)
            fl_dev_remove(b->devs[i]);

    return fl_pcmd_base_search(b);
}

int fl_pair(
    struct fl_base_i *base,
    const struct fl_dev_i *dev)
{
    struct fl_base *b = (struct fl_base *)base;
    struct fl_dev *d = (struct fl_dev *)dev;

    if (!fl_base_is_dev_valid(b, d))
        return ENOTCONN;
    if (d->state == STATE_PAIRED || d->state == STATE_CONNECTED)
        return EISCONN;

    return fl_pcmd_base_pair(b, d);
}

int fl_connect(
    struct fl_base_i *base,
    const struct fl_dev_i *dev)
{
    struct fl_base *b = (struct fl_base *)base;
    struct fl_dev *d = (struct fl_dev *)dev;

    if (!fl_base_is_dev_valid(b, d) || d->state == STATE_HANDSHAKED || d->state == STATE_PAIRING)
        return ENOTCONN;

    d->tx_packet_count = 0;
    d->tx_packet_loss = 0;

    return fl_ccmd_base_connect(b, d);
}

int fl_unpair(
    struct fl_base_i *base,
    const struct fl_dev_i *dev)
{
    struct fl_base *b = (struct fl_base *)base;
    struct fl_dev *d = (struct fl_dev *)dev;

    if (!fl_base_is_dev_valid(b, d) || d->state == STATE_HANDSHAKED)
        return ENOTCONN;

    return fl_ccmd_base_unpair(b, d);
}

int fl_data(
    struct fl_base_i *base,
    const struct fl_dev_i *dev,
    const uint8_t *data,
    size_t count,
    size_t padding_align,
    int is_plaintext)
{
    struct fl_base *b = (struct fl_base *)base;
    struct fl_dev *d = (struct fl_dev *)dev;

    if (!fl_base_is_dev_valid(b, d) || d->state != STATE_CONNECTED)
        return ENOTCONN;

    int res = fl_dcmd_data(b, d, data, count, padding_align, is_plaintext);
    return res;
}

struct fl_base_i *fl_init(void)
{
    struct fl_base *b = malloc(sizeof(struct fl_base));

    b->devs = malloc(8 * sizeof(struct fl_dev *));
    b->n_devs = 0;
    b->tx_func = NULL;
    b->tx_func_private_arg = NULL;
    b->devs_change_callback = NULL;
    b->devs_change_private_arg = NULL;
    b->pri_key = malloc(FELINK_uECC_PRI_KEY_SIZE);
    b->pub_key = malloc(FELINK_uECC_PUB_KEY_SIZE);
    int res = uECC_make_key(b->pub_key, b->pri_key, FELINK_uECC_CURVE);
    if (res == 0)
    {
        free(b);
        return NULL;
    }

    return (struct fl_base_i *)b;
}

void fl_delete(
    struct fl_base_i *base)
{
    struct fl_base *b = (struct fl_base *)base;

    for (int i = b->n_devs - 1; i >= 0; i--)
        fl_dev_remove(b->devs[i]);
    free(b->devs);
    free(b->pri_key);
    free(b->pub_key);
    free(b);
}

void fl_set_tx_func(
    struct fl_base_i *base,
    fl_tx_func_t func,
    void *private_arg)
{
    struct fl_base *b = (struct fl_base *)base;

    b->tx_func = func;
    b->tx_func_private_arg = private_arg;
}

void fl_set_devs_change_callback(
    struct fl_base_i *base,
    fl_devs_change_callback_t callback,
    void *private_arg)
{
    struct fl_base *b = (struct fl_base *)base;

    b->devs_change_callback = callback;
    b->devs_change_private_arg = private_arg;
}

/*  .sav
    char[]  FELK
    u32     len
    u8      chksum8
    u8      ecc_curve_len
    u8[]    pri_key
    u8[]    pub_key
    u32     n_paired_devs
    paired_devs[]
    {
        u32     id
        u32     type
        u16     version
        u32     connect_count
        u8      tea_key_len
        u8[]    tea_key
        char[]  name
    }
*/
struct fl_base_i *fl_load(
    const uint8_t *sav,
    size_t count)
{
    size_t ecc_curve_len = FELINK_uECC_CURVE_SIZE, pri_key_len = FELINK_uECC_PRI_KEY_SIZE, pub_key_len = FELINK_uECC_PUB_KEY_SIZE;

    const uint8_t *ptr = sav;

    if (strncmp((char *)ptr, "FELK", 4))
        return NULL;
    ptr += 4;

    uint32_t len = fl_rd32(ptr);
    if (len > count)
        return NULL;
    ptr += 4;

    if (fl_chksum8(sav, len) != 0)
        return NULL;
    ptr += 1;

    if (*ptr++ != ecc_curve_len)
        return NULL;

    struct fl_base *b = malloc(sizeof(struct fl_base));
    b->tx_func = NULL;
    b->tx_func_private_arg = NULL;
    b->devs_change_callback = NULL;
    b->devs_change_private_arg = NULL;
    b->pri_key = malloc(pri_key_len);
    memcpy(b->pri_key, ptr, pri_key_len);
    ptr += pri_key_len;
    b->pub_key = malloc(pub_key_len);
    memcpy(b->pub_key, ptr, pub_key_len);
    ptr += pub_key_len;

    uint32_t n_paired_devs = fl_rd32(ptr);
    ptr += 4;
    b->devs = malloc(n_paired_devs * sizeof(struct fl_dev *));
    for (int i = 0; i < n_paired_devs; i++)
    {
        uint32_t id = fl_rd32(ptr);
        ptr += 4;
        uint32_t type = fl_rd32(ptr);
        ptr += 4;
        uint16_t version = fl_rd16(ptr);
        ptr += 2;
        uint32_t connect_count = fl_rd32(ptr);
        ptr += 4;
        if (ecc_curve_len != *ptr)
        {
            ptr += 1 + *ptr;
            ptr += strlen((char *)ptr) + 1;
            continue;
        }

        const uint8_t *tea_key = (uint8_t *)(ptr + 1);
        ptr += 1 + *ptr;
        const char *name = (const char *)ptr;
        ptr += strlen((char *)ptr) + 1;

        struct fl_dev *d = fl_dev_add(b, id, type, version, name);
        memcpy(d->tea_key, tea_key, ecc_curve_len);
        d->connect_count = connect_count;
        d->state = STATE_PAIRED;
    }

    return (struct fl_base_i *)b;
}

size_t fl_save(
    struct fl_base_i *base,
    uint8_t **sav)
{
    struct fl_base *b = (struct fl_base *)base;

    size_t ecc_curve_len = FELINK_uECC_CURVE_SIZE, pri_key_len = FELINK_uECC_PRI_KEY_SIZE, pub_key_len = FELINK_uECC_PUB_KEY_SIZE;
    uint32_t len = 14 + pri_key_len + pub_key_len;

    uint32_t n_paired_devs = 0;
    struct fl_dev *paired_devs[b->n_devs];
    for (int i = 0; i < b->n_devs; i++)
    {
        struct fl_dev *d = b->devs[i];
        if (d->state == STATE_PAIRED || d->state == STATE_CONNECTED)
        {
            paired_devs[n_paired_devs++] = d;
            len += 17 + ecc_curve_len + strlen(d->name) + 1;
        }
    }

    *sav = malloc(len);
    uint8_t *ptr = *sav;

    strncpy((char *)ptr, "FELK", 4);
    ptr += 4;
    fl_wr32(ptr, len);
    ptr += 4;
    *ptr++ = 0;
    *ptr++ = (uint8_t)ecc_curve_len;
    memcpy(ptr, b->pri_key, pri_key_len);
    ptr += pri_key_len;
    memcpy(ptr, b->pub_key, pub_key_len);
    ptr += pub_key_len;
    fl_wr32(ptr, n_paired_devs);
    ptr += 4;
    for (int i = 0; i < n_paired_devs; i++)
    {
        struct fl_dev *d = paired_devs[i];
        fl_wr32(ptr, d->id);
        ptr += 4;
        fl_wr32(ptr, d->type);
        ptr += 4;
        fl_wr16(ptr, d->version);
        ptr += 2;
        fl_wr32(ptr, d->connect_count);
        ptr += 4;
        *ptr++ = (uint8_t)ecc_curve_len;
        memcpy(ptr, d->tea_key, ecc_curve_len);
        ptr += ecc_curve_len;
        strcpy((char *)ptr, d->name);
        ptr += strlen(d->name) + 1;
    }
    (*sav)[8] = fl_chksum8(*sav, len);

    return len;
}
