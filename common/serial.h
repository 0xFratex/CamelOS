// common/serial.h
#ifndef SERIAL_H
#define SERIAL_H

#include "ports.h"

int init_serial();
int is_transmit_empty();
void write_serial(char a);
void s_printf(const char* str);

void serial_write_string(const char* str);

#endif
