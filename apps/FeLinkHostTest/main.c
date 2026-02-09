#include "FeLinkBase/felink.h"
#include "connection.h"
#include "host.h"
#include "autosave.h"
#include "network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

struct felink_collect
{
    struct fl_base_i **base;
    struct fl_con_i **con;
    struct fl_host_i **host;
    struct fl_autosave_i **autosave;
};

void change_handler(struct fl_host_i *host, struct fl_dev_i *dev, int type, void *private_arg)
{
    struct felink_collect *felink = private_arg;

    switch (type)
    {
    case DEV_CHANGE_REMOVE:
        connection_dev_con_remove(*felink->con, dev);
        autosave_add_change(*felink->autosave, AUTOSAVE_BASE_CHANGE);
        break;
    case DEV_CHANGE_CONNECT:
        connection_dev_con_add(*felink->con, dev);
        autosave_add_change(*felink->autosave, AUTOSAVE_BASE_CHANGE);
        break;
    case DEV_CHANGE_ADD:
    case DEV_CHANGE_ID_CHANGE:
    case DEV_CHANGE_PAIR:
        autosave_add_change(*felink->autosave, AUTOSAVE_BASE_CHANGE);
        break;
    case HOST_CHANGE_USER_ADD:
    case HOST_CHANGE_USER_CHANGE:
    case HOST_CHANGE_USER_REMOVE:
    case HOST_CHANGE_USER_DEV_ADD:
    case HOST_CHANGE_USER_DEV_REMOVE:
        autosave_add_change(*felink->autosave, AUTOSAVE_HOST_CHANGE);
        break;
    case DEV_CHANGE_PAIR_START:
    case DEV_CHANGE_CONNECT_TIMEOUT:
    case HOST_CHANGE_CLIENT_CON:
    case HOST_CHANGE_CLIENT_DISCON:
    default:
        break;
    }
}

int main(int argc, char *argv[])
{
    struct fl_base_i *base = NULL;
    struct fl_con_i *con = NULL;
    struct fl_host_i *host = NULL;
    struct fl_autosave_i *autosave = NULL;
    int res;

    if (network_wifi_connect())
        while (network_provisioning())
            ;

    int felink_sav_fd = open(AUTOSAVE_BASE_FILE, O_RDONLY);
    if (felink_sav_fd > 0)
    {
        off_t sav_size = lseek(felink_sav_fd, 0, SEEK_END);
        lseek(felink_sav_fd, 0, SEEK_SET);
        uint8_t sav_buf[sav_size];
        read(felink_sav_fd, sav_buf, sav_size);
        close(felink_sav_fd);
        base = fl_load(sav_buf, sav_size);
    }
    if (base == NULL)
    {
        perror("FeLink: reload save ERROR, save file open");
        base = fl_init();
        if (base == NULL)
            return 1;
    }

    con = connection_init(base);
    if (con == NULL)
        return 1;

    int host_sav_fd = open(AUTOSAVE_HOST_FILE, O_RDONLY);
    if (host_sav_fd > 0)
    {
        off_t sav_size = lseek(host_sav_fd, 0, SEEK_END);
        lseek(host_sav_fd, 0, SEEK_SET);
        uint8_t sav_buf[sav_size];
        read(host_sav_fd, sav_buf, sav_size);
        close(host_sav_fd);
        host = host_load(base, sav_buf, sav_size);
    }
    if (host == NULL)
    {
        perror("Host: reload save ERROR, save file open");
        host = host_init(base);
        if (host == NULL)
            return 1;
    }

    autosave = autosave_start(base, host, 30);
    if (autosave == NULL)
    {
        printf("Autosave: start ERROR\n");
        return 1;
    }
    struct felink_collect felink = {&base, &con, &host, &autosave};
    host_set_host_change_callback(host, change_handler, &felink);

    res = host_start(host, 11300, "cert/certificate.pem", "cert/privatekey.pem");
    if (res)
    {
        printf("Host: server start ERROR\n");
        return 1;
    }

    res = connection_start_receive(con);
    if (res)
    {
        printf("Connection: start ERROR\n");
        return 1;
    }

    printf("FeLink: base started\n");

    char cmd_buf[256];
    uint32_t dev_id = 0;
    while (1)
    {
        res = 0;
        scanf("%s", cmd_buf);
        struct fl_dev_i *dev = fl_get_dev_by_id(base, dev_id);
        if (strcmp(cmd_buf, "quit") == 0)
        {
            autosave_stop(autosave);
            connection_stop(con);
            host_stop(host);
            host_sav_fd = open(AUTOSAVE_HOST_FILE, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU);
            felink_sav_fd = open(AUTOSAVE_BASE_FILE, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU);
            if (host_sav_fd < 0 || felink_sav_fd < 0)
            {
                perror("FeLink: save ERROR");
                printf("Do you want to force quit? [y/N]: ");
                scanf("%s", cmd_buf);
                if (cmd_buf[0] == 'y')
                    return 1;
                else
                {
                    close(host_sav_fd);
                    close(felink_sav_fd);
                    continue;
                }
            }
            uint8_t *sav_buf;
            size_t sav_size;

            sav_size = host_save(host, &sav_buf);
            write(host_sav_fd, sav_buf, sav_size);
            free(sav_buf);
            close(host_sav_fd);

            sav_size = fl_save(base, &sav_buf);
            write(felink_sav_fd, sav_buf, sav_size);
            free(sav_buf);
            close(felink_sav_fd);

            host_delete(host);
            fl_delete(base);

            return 0;
        }
        else if (strcmp(cmd_buf, "init") == 0)
        {
            unlink(AUTOSAVE_BASE_FILE);
            unlink(AUTOSAVE_HOST_FILE);

            autosave_stop(autosave);
            host_set_host_change_callback(host, NULL, NULL);
            connection_stop(con);
            host_stop(host);
            host_delete(host);
            fl_delete(base);

            base = fl_init();
            con = connection_init(base);
            host = host_init(base);
            if (base == NULL || con == NULL || host == NULL)
                return 1;
            autosave = autosave_start(base, host, 30);
            host_set_host_change_callback(host, change_handler, autosave);
            int res = host_start(host, 11300, "cert/certificate.pem", "cert/privatekey.pem");
            res += connection_start_receive(con);
            if (res || autosave == NULL)
                return 1;
        }
        else if (strcmp(cmd_buf, "info") == 0)
        {
            printf("FeLink: devices info:\n");
            host_dev_change_handler(base, NULL, 0, -1, host);
            printf("Host: users info:\n");
            for (int i = 0; i < host->n_users; i++)
            {
                printf("\t<%s>\t -> Devices: %d, Clients: %d, Is use only: %s\n",
                       host->users[i]->username,
                       host->users[i]->n_available_devs,
                       host->users[i]->n_clients,
                       host->users[i]->is_use_only ? "true" : "false");
                printf("\t\t[ ");
                for (int j = 0; j < host->users[i]->n_available_devs; j++)
                    printf("%08X ", host->users[i]->available_devs[j]->id);
                printf("]\n");
                printf("\t\t[ ");
                for (int j = 0; j < host->users[i]->n_clients; j++)
                    printf("%s:%hu ", inet_ntoa(host->users[i]->clients[j]->addr.sin_addr), ntohs(host->users[i]->clients[j]->addr.sin_port));
                printf("]\n");
            }
        }
        else if (strcmp(cmd_buf, "select") == 0)
        {
            printf("Device ID (HEX): ");
            scanf("%s", cmd_buf);
            dev_id = (uint32_t)strtoul(cmd_buf, NULL, 16);
        }
        else if (strcmp(cmd_buf, "scan") == 0)
        {
            res = fl_scan(base);
        }
        else if (strcmp(cmd_buf, "pair") == 0)
        {
            if (dev == NULL)
                printf("FeLink: Bad device\n");
            res = fl_pair(base, dev);
        }
        else if (strcmp(cmd_buf, "connect") == 0)
        {
            if (dev == NULL)
                printf("FeLink: Bad device\n");
            res = fl_connect(base, dev);
        }
        else if (strcmp(cmd_buf, "unpair") == 0)
        {
            if (dev == NULL)
                printf("FeLink: Bad device\n");
            res = fl_unpair(base, dev);
        }
        else if (strcmp(cmd_buf, "data") == 0)
        {
            if (dev == NULL)
                printf("FeLink: Bad device\n");
            printf("Start send to %08X, enter \"end\" to exit\n", dev_id);
            while (1)
            {
                scanf("%s", cmd_buf);
                dev = fl_get_dev_by_id(base, dev_id);
                if (dev == NULL)
                    printf("FeLink: Bad device\n");
                if (strcmp(cmd_buf, "end") == 0)
                    break;
                res = fl_data(base, dev, (uint8_t *)cmd_buf, strlen(cmd_buf), 0, 0);
                if (res)
                    break;
            }
        }
        if (res)
            printf("FeLink: ERROR : %s\n", strerror(res));
    }
}
