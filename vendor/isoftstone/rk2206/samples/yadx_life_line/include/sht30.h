#ifndef __SHT30_H__
#define __SHT30_H__

void sht30_config(void);
/* 成功返回 0，CRC/总线失败返回 -1（不改写 dat） */
int sht30_read_data(double *dat);

#endif
