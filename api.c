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
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/un.h>
#include "api.h" 
#define UNIX_PATH_MAX 108 


int fd_s; //fd socket 
char sck_name[UNIX_PATH_MAX]; //nome del socket
int connected = 0; //mi ricordo se la connessione è avvenuta o meno

//controllo se a è minore di b 
int compareTime(struct timespec a, struct timespec b){ 
    clock_gettime(CLOCK_REALTIME, &a);

    if(a.tv_sec == b.tv_sec){
        if(a.tv_nsec > b.tv_nsec)
            return -1;
        else
            return 0;
    } 
	else if(a.tv_sec > b.tv_sec)
        return -1;
    else
        return 0;
}

//non devo controllare la connessione sia stata già aperta
//perchè -f lo posso chiamare solo una volta e solo lì invoco openConnection
int openConnection(const char* sockname, int msec, const struct timespec abstime){
	errno = 0;
	
	if((fd_s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1){
		errno = EINVAL;
		perror("ERRORE: socket");
		return -1;
	}
	struct timespec time;
	struct sockaddr_un sa;
	strncpy(sa.sun_path, sockname, UNIX_PATH_MAX);
	sa.sun_family = AF_UNIX;
	int t;
	
	while ((connect(fd_s, (struct sockaddr *) &sa, sizeof(sa) == -1) && (t = compareTime(time, abstime)) != -1) )
	//rimango nei limiti del timeout
			sleep(msec/1000);
	if (t == -1){
		errno = ETIMEDOUT;
		//perror("ERRORE: timeout connessione");
		return -1;
	}
	
	connected = 1;
	#ifdef DEBUG
		char *buf = malloc(5*sizeof(char));
		read(fd_s, buf, 4);
		fprintf(stderr, "%s\n", buf);
	#endif
	strcpy(sck_name, sockname); //memorizzo il nome del socket in una var globale
	
	return 0;
}