#include "connection.h"
#include "FeLinkBase/felink.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#define NRF24_FELINK_CMD_ADDR "0x666C636D64"

#define NRF24_PAYLOAD_WIDTH 32
#if NRF24_PAYLOAD_WIDTH < 2
#error NRF24_PAYLOAD_WIDTH should be bigger than 2
#endif
#define _MARCO_TO_STR(marco) #marco
#define MARCO_TO_STR(marco) _MARCO_TO_STR(marco)

#define MY_MIN(a, b) (a) < (b) ? (a) : (b)

#define C_PRINT_ERR (1 << 0)
#define C_PRINT_MSG (1 << 1)
#define C_PRINT_INFO (1 << 2)
#define C_PRINT_DISPLAY (C_PRINT_ERR | C_PRINT_MSG | C_PRINT_INFO)
#define con_printf(type, format, ...) \
    if (C_PRINT_DISPLAY & (type))     \
    printf(format, ##__VA_ARGS__)

struct fl_dev_con
{
    struct fl_dev_i *dev;
    char addr[13];
};

struct fl_con
{
    struct fl_dev_con *dev_cons;
    int n_dev_cons;

    int cmd_fd;
    int data_fd;
    struct fl_base_i *base;
    pthread_t receive_thread;
};

static int sysfs_puts(const char *f, const char *s)
{
    int len = strlen(s) + 1;
    int fd;
    fd = open(f, O_WRONLY);
    ssize_t res = write(fd, s, len);
    close(fd);
    return res != len;
}
static int nrf24_config(void)
{
    int res = 0;
    res += sysfs_puts("/sys/class/nrf24/nrf0/address_width", "5");
    res += sysfs_puts("/sys/class/nrf24/nrf0/channel", "113");
    res += sysfs_puts("/sys/class/nrf24/nrf0/crc", "16");
    res += sysfs_puts("/sys/class/nrf24/nrf0/data_rate", "2048");
    res += sysfs_puts("/sys/class/nrf24/nrf0/retr_count", "15");
    res += sysfs_puts("/sys/class/nrf24/nrf0/retr_delay", "1000");
    res += sysfs_puts("/sys/class/nrf24/nrf0/rf_power", "0");
    res += sysfs_puts("/sys/class/nrf24/nrf0.0/address", NRF24_FELINK_CMD_ADDR);
    res += sysfs_puts("/sys/class/nrf24/nrf0.1/address", "0x0000000000");
    res += sysfs_puts("/sys/class/nrf24/nrf0.0/plw", MARCO_TO_STR(NRF24_PAYLOAD_WIDTH));
    res += sysfs_puts("/sys/class/nrf24/nrf0.1/plw", MARCO_TO_STR(NRF24_PAYLOAD_WIDTH));
    res += sysfs_puts("/sys/class/nrf24/nrf0.0/ack", "1");
    res += sysfs_puts("/sys/class/nrf24/nrf0.1/ack", "1");
    res += sysfs_puts("/sys/class/nrf24/nrf0.2/ack", "0");
    res += sysfs_puts("/sys/class/nrf24/nrf0.3/ack", "0");
    res += sysfs_puts("/sys/class/nrf24/nrf0.4/ack", "0");
    res += sysfs_puts("/sys/class/nrf24/nrf0.5/ack", "0");
    if (res)
        return EIO;
    return 0;
}

#define NRF24_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CON_NRF24_BUF_SIZE 128
#define CON_NRF24_FELINK_BLOCK_SIZE (NRF24_PAYLOAD_WIDTH - 1)
static void *nrf24_rx_thread(void *args)
{
    struct fl_con *con = args;
    struct fl_base_i *b = con->base;

    uint8_t buf[CON_NRF24_BUF_SIZE];
    uint8_t buf_max_block_index = CON_NRF24_BUF_SIZE / CON_NRF24_FELINK_BLOCK_SIZE;
    uint8_t max_block_index = 0;
    uint8_t rx_buf[NRF24_PAYLOAD_WIDTH];
    while (1)
    {
        ssize_t count = read(con->cmd_fd, rx_buf, NRF24_PAYLOAD_WIDTH);
        if (count < 0)
            continue;

        uint8_t block_index = rx_buf[0];
        if (block_index > buf_max_block_index)
            continue;
        else
            memcpy(
                &buf[block_index * CON_NRF24_FELINK_BLOCK_SIZE],
                &rx_buf[1],
                block_index == buf_max_block_index ? CON_NRF24_BUF_SIZE - block_index * CON_NRF24_FELINK_BLOCK_SIZE : CON_NRF24_FELINK_BLOCK_SIZE);

        if (block_index > max_block_index)
            max_block_index = block_index;

        if (block_index == 0)
        {
            size_t len = NRF24_MIN((max_block_index + 1) * CON_NRF24_FELINK_BLOCK_SIZE, CON_NRF24_BUF_SIZE);
            con_printf(C_PRINT_MSG, "R: ");
            for (int i = 0; i < len; i++)
                con_printf(C_PRINT_MSG, "%02hhX ", buf[i]);
            con_printf(C_PRINT_MSG, "\n");

            int res = fl_receive_handler(b, buf, len);
            if (res)
                con_printf(C_PRINT_MSG, "FeLink ERROR: %s\n", strerror(res));

            max_block_index = 0;
        }
    }
    pthread_exit(NULL);
}

static int nrf24_tx_func(struct fl_con *con, uint8_t *buf, size_t count, int pipe)
{
    uint8_t tx_buf[32 + (count / (NRF24_PAYLOAD_WIDTH - 1) * NRF24_PAYLOAD_WIDTH)]; // 确保缓存空间足够
    uint8_t block_index = count / CON_NRF24_FELINK_BLOCK_SIZE;
    const uint8_t *ptr = buf + count - count % CON_NRF24_FELINK_BLOCK_SIZE;

    uint8_t *tbptr = tx_buf;
    memset(tbptr, 0, NRF24_PAYLOAD_WIDTH);
    tbptr[0] = block_index;
    memcpy(&tbptr[1], ptr, count % CON_NRF24_FELINK_BLOCK_SIZE);
    tbptr += NRF24_PAYLOAD_WIDTH;
    ptr -= CON_NRF24_FELINK_BLOCK_SIZE;
    while (block_index--)
    {
        tbptr[0] = block_index;
        memcpy(&tbptr[1], ptr, CON_NRF24_FELINK_BLOCK_SIZE);
        tbptr += NRF24_PAYLOAD_WIDTH;
        ptr -= CON_NRF24_FELINK_BLOCK_SIZE;
    }

    int fd = pipe == 0 ? con->cmd_fd : con->data_fd;
    ssize_t n = write(fd, tx_buf, (size_t)(tbptr - tx_buf));
    if (n < (size_t)(tbptr - tx_buf))
        return errno;

    con_printf(C_PRINT_MSG, "T: ");
    for (int i = 0; i < count; i++)
        con_printf(C_PRINT_MSG, "%02hhX ", buf[i]);
    con_printf(C_PRINT_MSG, "\n");

    return 0;
}

void connection_dev_con_add(struct fl_con_i *con, struct fl_dev_i *dev)
{
    struct fl_con *c = (struct fl_con *)con;

    for (int i = 0; i < c->n_dev_cons; i++)
    {
        if (c->dev_cons[i].dev == dev)
        {
            char *addr = c->dev_cons[i].addr;
            sprintf(addr, "0x666C%06X", dev->salt & 0x00FFFFFF);
            return;
        }
    }

    c->dev_cons = realloc(c->dev_cons, (c->n_dev_cons + 1) * sizeof(struct fl_dev_con));
    c->dev_cons[c->n_dev_cons].dev = dev;

    char *addr = c->dev_cons[c->n_dev_cons].addr;
    sprintf(addr, "0x666C%06X", dev->salt & 0x00FFFFFF);
    c->n_dev_cons++;
}

void connection_dev_con_remove(struct fl_con_i *con, struct fl_dev_i *dev)
{
    struct fl_con *c = (struct fl_con *)con;
    int index;

    for (index = 0; index < c->n_dev_cons; index++)
        if (c->dev_cons[index].dev == dev)
            break;
    if (index == c->n_dev_cons)
        return;

    for (int i = index; i < c->n_dev_cons - 1; i++)
        c->dev_cons[i] = c->dev_cons[i + 1];
    c->n_dev_cons--;
}

static const char *connection_get_dev_con_addr(struct fl_con *c, struct fl_dev_i *dev)
{
    if (dev != NULL)
        for (int i = 0; i < c->n_dev_cons; i++)
            if (c->dev_cons[i].dev == dev)
                return c->dev_cons[i].addr;
    return NULL;
}

int connection_tx_func(struct fl_dev_i *dev, uint8_t *buf, size_t count, void *con)
{
    struct fl_con *c = con;
    static char old_addr[16];

    const char *addr = connection_get_dev_con_addr(c, dev);
    if (addr != NULL && strcmp(addr, old_addr) != 0)
    {
        if (sysfs_puts("/sys/class/nrf24/nrf0.1/address", addr))
            return EIO;
        strcpy(old_addr, addr);
    }

    return nrf24_tx_func(c, buf, count, addr != NULL);
}

struct fl_con_i *connection_init(struct fl_base_i *base)
{
    struct fl_con *c = malloc(sizeof(struct fl_con));

    c->base = base;
    if (nrf24_config())
        return NULL;
    c->cmd_fd = open("/dev/nrf0.0", O_RDWR);
    if (c->cmd_fd < 0)
    {
        perror("nrf24 open");
        free(c);
        return NULL;
    }
    c->data_fd = open("/dev/nrf0.1", O_WRONLY);
    if (c->data_fd < 0)
    {
        perror("nrf24 open");
        free(c);
        return NULL;
    }
    c->dev_cons = malloc(8 * sizeof(struct fl_dev_con));
    c->n_dev_cons = 0;

    fl_set_tx_func(base, connection_tx_func, c);

    return (struct fl_con_i *)c;
}

int connection_start_receive(struct fl_con_i *con)
{
    struct fl_con *c = (struct fl_con *)con;

    return pthread_create(&c->receive_thread, NULL, nrf24_rx_thread, c);
}

void connection_stop(struct fl_con_i *con)
{
    struct fl_con *c = (struct fl_con *)con;

    fl_set_tx_func(c->base, NULL, NULL);

    pthread_cancel(c->receive_thread);
    pthread_join(c->receive_thread, NULL);
    close(c->cmd_fd);
    free(c);
}
