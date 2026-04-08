#ifndef _GALVO_HAL_H_
#define _GALVO_HAL_H_

#include "galvo_ioctl.h"

int galvo_open(void);
int galvo_send_batch(struct galvo_point *points, uint32_t count);
void galvo_close(void);

#endif