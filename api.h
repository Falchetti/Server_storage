#ifndef API_H_
#define API_H_

//#include <sys/types.h>
//#include <sys/socket.h>
#include <unistd.h>


int openConnection(const char* sockname, int msec, const struct timespec abstime);
int closeConnection(const char *sockname);
int openFile(const char *sockname, int flags);
int closeFile(const char *pathname);
int readFile(const char *pathname, void **buf, size_t *size);
int lockFile(const char *pathname);
int unlockFile(const char *pathname);
int writeFile(const char *pathname, const char *dirname);
int appendToFile(const char *pathname, void *buf, size_t size, const char *dirname);
int removeFile(const char *pathname);
int readNFile(int N, const char *dirname);

#endif