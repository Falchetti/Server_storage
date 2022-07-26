#ifndef API_H_
#define API_H_

//#include <sys/types.h>
//#include <sys/socket.h>
#include <unistd.h>


int openConnection(const char* sockname, int msec, const struct timespec abstime);
int closeConnection(const char *sockname);
#endif