#ifndef STORAGE_F_H
#define STORAGE_F_H

#include "list.h"
#include "defines.h"

typedef struct file_info{ //struttura dei valori nell'hashtable (storage)
	int lock_owner;
	open_node *open_owners;
	int lst_op; //controllo per la writeFile
	char cnt[MAX_SIZE]; //con char * ho maggiore flessibilità, ma più "difficile" gestione cnt (allocare/deallocare memoria, realloc buff di readF) (scrivilo sulla relazione e gestiscilo (tento di scrivere/appendere file troppo grande)
	int cnt_sz;  
	pthread_cond_t cond;
} file_info;

typedef struct file_repl{ //entry della coda per i rimpiazzamenti 
	file_info *pnt;
	char *path;
	struct file_repl *next;
} file_repl;

int openFC(char *path, int fd);
int openFL(char *path, int fd);
int openFO(char *path, int fd);
int openFCL(char *path, int fd);
int closeF(char *path, int fd);
int readF(char *path, int fd);
int writeF(char *path, char *cnt, int sz, int fd);
int lockF(char *path, int fd);
int unlockF(char *path, int fd);
int appendToF(char *path, char *cnt, int sz, int fd);
int removeF(char *path, int fd);
int readNF(int fd, int n);

file_info *init_file(int lock_owner, int fd, int lst_op, char *cnt, int sz);
int st_repl(int dim, int fd, char *path, file_repl **list, int isCreate);
int file_sender(file_repl *list, int fd);

int clean_storage (int fd);
void print_files();
#endif