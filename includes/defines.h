#ifndef DEFINES_H_
#define DEFINES_H_

#define UNIX_PATH_MAX 108
#define MAX_SIZE 32000 //taglia massima contenuto file 

#ifdef _POSIX
typedef _sigset_t sigset_t;
#endif

#endif /* DEFINES_H_ */