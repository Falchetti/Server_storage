#ifndef DEFINES_H_
#define DEFINES_H_

#define UNIX_PATH_MAX 108
#define MAX_SIZE 10000 //taglia massima contenuto file, cmd;path;sz, lunghezza richiesta al server, riga log_file, contenuto file max memorizzato su server, vedi se quest'ultimo lo puoi rendere più flessibile (però ricorda che readF ti dà un buffer di taglia MAX_SIZE, vedi se lo devi riallocare in caso 
#ifdef _POSIX
typedef _sigset_t sigset_t;
#endif

#endif 