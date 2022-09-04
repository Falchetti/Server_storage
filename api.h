#ifndef API_H_
#define API_H_

//#include <sys/types.h>
//#include <sys/socket.h>
#include <unistd.h>

//int t; //controlla se va bene 


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


int isTimeout(struct timespec, struct timespec);  
int save_file(const char *dir, char *file, char *buff, int n);
int msg_sender(char *msg, char *cmd, const char *path, int size, char *cnt);
int isNumber(void *el, int *n);




#endif