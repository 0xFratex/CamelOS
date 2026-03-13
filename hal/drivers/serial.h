#ifndef SERIAL_H
#define SERIAL_H

#include "../common/ports.h"

int init_serial();
int is_transmit_empty();
void write_serial(char a);
void s_printf(const char* str);

void serial_write_string(const char* str);

#endif
