#ifndef _FELINK_CONNECTION_
#define _FELINK_CONNECTION_

#include "FeLinkBase/felink.h"

#include <stdint.h>
#include <errno.h>

struct fl_con_i
{
    const struct fl_dev_con *const dev_cons;
    const int n_dev_cons;
};

void connection_dev_con_add(struct fl_con_i *con, struct fl_dev_i *dev);
void connection_dev_con_remove(struct fl_con_i *con, struct fl_dev_i *dev);
int connection_tx_func(struct fl_dev_i *dev, uint8_t *buf, size_t count, void *con);
struct fl_con_i *connection_init(struct fl_base_i *base);
int connection_start_receive(struct fl_con_i *con);
void connection_stop(struct fl_con_i *con);

#endif