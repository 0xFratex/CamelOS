#ifndef _ERRNO_H
#define _ERRNO_H

// Minimal errno for MuJS compatibility

#define EDOM        1
#define ERANGE      34
#define EINVAL      22
#define ENOENT      2
#define ENOMEM      12

extern int errno;
char* strerror(int errnum);

#endif
