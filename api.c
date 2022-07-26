/*
int openConnection(const char* sockname, int msec, const struct timespec abstime)

Viene aperta una connessione AF_UNIX al socket file sockname. 
Se il server non accetta immediatamente la richiesta di connessione, 
la connessione da parte del client viene ripetuta dopo ‘msec’ millisecondi 
e fino allo scadere del tempo assoluto ‘abstime’ specificato come terzo argomento. 

Ritorna 0 in caso di successo, -1 in caso di fallimento, errno viene settato opportunamente.
NB: devi settare errno*/

#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/un.h>
#include "api.h" 
#define UNIX_PATH_MAX 108 //lunghezza massima path 
#define DEBUG


int fd_s; //fd socket 
char sck_name[UNIX_PATH_MAX]; //nome del socket
int connected = 0; //mi ricordo se la connessione è avvenuta o meno, serve???
//potrei usare direttamente fd_s

int isTimeout(struct timespec, struct timespec);


//non devo controllare che la connessione sia stata già aperta
//perchè -f lo posso chiamare solo una volta e solo lì invoco openConnection
//inoltre la connessione è unica per ogni client 

int openConnection(const char* sockname, int msec, const struct timespec abstime){
	errno = 0;
	int i = 0;
	
	if((fd_s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1){
		errno = EINVAL; //va bene questo tipo di errore? 
		perror("Errore in socket");
		return -1;
	}
	
	struct timespec time;
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(struct sockaddr_un)); //lo azzero 
	strncpy(sa.sun_path, sockname, UNIX_PATH_MAX); 
	sa.sun_family = AF_UNIX;
	int t = 0;
	
	while (((i = connect(fd_s, (struct sockaddr *) &sa, sizeof(sa))) == -1) && ((t = isTimeout(time, abstime)) == 0) ){
			sleep(msec/1000); //è attesa attiva? 
	}
		
	if (t){
		errno = ETIMEDOUT;
		perror("Errore, Timeout sopraggiunto");
		return -1;
	}
	
	connected = 1;
	
	#ifdef DEBUG
		char *buf = malloc(6*sizeof(char));
		read(fd_s, buf, 5);
		fprintf(stderr, "Contenuto canale di comunicazione: %s\n", buf);
	    free(buf);
	#endif
	
	strcpy(sck_name, sockname); //memorizzo il nome del socket in una var globale
	
	return 0;
}

//controllo se a è maggiore di b 
int isTimeout(struct timespec a, struct timespec b){ 
    clock_gettime(CLOCK_REALTIME, &a);
	
	if(a.tv_sec == b.tv_sec){
        if(a.tv_nsec > b.tv_nsec)
            return 1;
        else
            return 0;
    } 
	else if(a.tv_sec > b.tv_sec)
        return 1;
    else
        return 0;
}

//per evitare race conditions attenzione a non chiudere 
//file descriptor se sono usati da altri threads nello stesso processo
int closeConnection(const char *sockname){
	errno = 0;
	if(strcmp(sockname, sck_name) == 0){ //controlla se va bene strcmp
		if(fd_s != -1){ //mi assicuro che non fosse già stato chiuso 
		    if(write(fd_s, "disconnected", 12) == -1){//le write dovranno essere riviste (atomicità)
				perror("Errore nella write");
				return -1;
			}
		    if(close(fd_s) == -1){
				perror("Errore nella close");
				return -1;
			}
		}
		else 
			fd_s = -1; 
		return 0;
	}
	else{
		errno = EINVAL;
		perror("Errore parametro");
		return -1;
	}
}
	
	
