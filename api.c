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
#define MSG_SIZE  50 
#define DEBUG
#undef DEBUG
#define O_LOCK 0
#define O_CREATE 1

//****************
// sono quasi tutte uguali queste funzioni, vedi se puoi accorparle con una funz generale
//che viene chiamata opportunamente (magari col tipo di msg come parametro)
//****************


int fd_s = -1; //fd socket 
char sck_name[UNIX_PATH_MAX]; //nome del socket

//int connected = 0; //mi ricordo se la connessione è avvenuta o meno, serve???
//potrei usare direttamente fd_s

int isTimeout(struct timespec, struct timespec);


//non devo controllare che la connessione sia stata già aperta
//perchè -f lo posso chiamare solo una volta e solo lì invoco openConnection
//inoltre la connessione è unica per ogni client 

//credo che probabilmete avrei dovuto farlo fare al server come tutto il resto,
//non credo, perchè ognuno lo deve aprire dal proprio lato 
int openConnection(const char* sockname, int msec, const struct timespec abstime){ 
	errno = 0;
	int t = 0;
	
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
	
	while ( (connect(fd_s, (struct sockaddr *) &sa, sizeof(sa)) == -1) && ((t = isTimeout(time, abstime)) == 0) )
			sleep(msec/1000); //è attesa attiva? 
			
	if (t){
		errno = ETIMEDOUT;
		perror("Errore, Timeout sopraggiunto");
		return -1;
	}
	
	//connected = 1; //serve?
	
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
	
		if(fd_s != -1){ //mi assicuro che non fosse già stato chiuso (se è già stato chiuso ignoro la richiesta 
		    if(write(fd_s, "disconnesso", 12) == -1){//le write dovranno essere riviste (atomicità)
				perror("Errore nella write");
				return -1;
			}
		    if(close(fd_s) == -1){
				perror("Errore nella close");
				return -1;
			}
		}
		
		fd_s = -1; 
	}
	else{
		errno = EINVAL;
		perror("Errore parametro");
		return -1;
	}
	
	return 0;
}

int openFile(const char *pathname, int flags){ //per ora senza OR, ci dovrà essere un controllo sui flags (se sono ammessi)
	
	int len;
	char *msg;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	if(fd_s != -1){ //la connessione è aperta? IL SERVER CONTROLLA SE IL FILE E' GIA' APERTO
	
		len = strlen(pathname);
		msg = malloc(MSG_SIZE*sizeof(char));
		if (flags == O_CREATE){
			strncpy(msg, "openfc", 7); //vedi se dovevi fare una memset prima
			strncat(msg, pathname, len+1); //guarda se strncat funziona così
			write(fd_s, msg, len+7);
		}
		else if(flags == O_LOCK){
			strncpy(msg, "openfl", 7); //vedi se dovevi fare una memset prima
			strncat(msg, pathname, len+1); //guarda se strncat funziona così
			write(fd_s, msg, len+7);
		}
			
		read(fd_s, msg, MSG_SIZE);
		if(strcmp(msg, "Ok") != 0){
			errno = -1;//come lo setto errno in questo caso???
			fprintf(stderr, "Esito dal server: %s\n", msg);
		    free(msg);
			return -1;
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	return 0;
		
}

int closeFile(const char *pathname){ 

    int len;
	char *msg;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	if(fd_s != -1){
		len = strlen(pathname);
		msg = malloc(MSG_SIZE*sizeof(char));
		strncpy(msg, "closef", 7); //vedi se dovevi fare una memset prima
		strncat(msg, pathname, len+1); //credo che il controllo di dimensione lo devi fare su msg, NON su pathname
		write(fd_s, msg, len+7);
		
		read(fd_s, msg, MSG_SIZE);
		if(strcmp(msg, "Ok") != 0){
			errno = -1; //come lo setto errno in questo caso???
			fprintf(stderr, "Err in chiusura file, msg server: %s\n", msg);
		    free(msg);
			return -1;
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}

return 0; 
}

int readFile(const char *pathname, void **buf, size_t *size) {
	
	int len;
	char *msg;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	if(fd_s != -1){ //controllo se sono connesso 
		len = strlen(pathname);
		msg = malloc(MSG_SIZE*sizeof(char));
		strncpy(msg, "readf", 6); //vedi se devi contare il terminatore e se dovevi fare una memset prima
		strncat(msg, pathname, len+1); //guarda se strncat funziona così
		write(fd_s, msg, len+6);
		
		read(fd_s, msg, MSG_SIZE);
		
		if(( *buf = (void *) strstr(msg, "Ok")) == NULL){
			errno = -1; //come lo setto errno in questo caso???
			fprintf(stderr, "buff di lettura NULL, msg del server: %s\n", msg);
		    free(msg);
			return -1;
		}
		else{
			//fprintf(stderr,"1 buff = %s\n", (char *)*buf);
			memmove(*buf, (char *) *buf + 2, strlen((char *) *buf)); //perchè qui mmve e in altri casi direttamente *buf +2?
			//fprintf(stderr,"2 buff = %s\n", (char *)*buf);
			*size = strlen(*buf); //va bene?	
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	
	return 0; }
	
int lockFile(const char *pathname){
	int len;
	char *msg;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	if(fd_s != -1){
		len = strlen(pathname);
		msg = malloc(MSG_SIZE*sizeof(char));
		strncpy(msg, "lckf", 6); //vedi se dovevi fare una memset prima
		strncat(msg, pathname, len+1); //guarda se strncat funziona così
		write(fd_s, msg, len+6);
		
		read(fd_s, msg, MSG_SIZE);
		if(strcmp(msg, "Ok") != 0){
			errno = -1;//come lo setto errno in questo caso???
			fprintf(stderr, "Esito dal server: %s\n", msg);
		    free(msg);
			return -1;
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	return 0;
}

int unlockFile(const char *pathname){
	int len;
	char *msg;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	if(fd_s != -1){
		len = strlen(pathname);
		msg = malloc(MSG_SIZE*sizeof(char));
		strncpy(msg, "unlockf", 8); //vedi se dovevi fare una memset prima
		strncat(msg, pathname, len+1); //guarda se strncat funziona così
		write(fd_s, msg, len+8);
		
		read(fd_s, msg, MSG_SIZE);
		if(strcmp(msg, "Ok") != 0){
			errno = -1;//come lo setto errno in questo caso???
			fprintf(stderr, "Esito dal server: %s\n", msg);
		    free(msg);
			return -1;
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	return 0;
}

int writeFile(const char *pathname, const char* dirname) {
	
	int len;
	char *msg;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	if(fd_s != -1){ //controllo se sono connesso 
		len = strlen(pathname);
		msg = malloc(MSG_SIZE*sizeof(char));
		strncpy(msg, "writef", 7); //vedi se devi contare il terminatore e se dovevi fare una memset prima
		strncat(msg, pathname, len+1); //guarda se strncat funziona così
		write(fd_s, msg, len+6);
		
		read(fd_s, msg, MSG_SIZE);
		
		if((void *) strstr(msg, "Ok") == NULL){
			errno = -1; //come lo setto errno in questo caso???
			fprintf(stderr, "errore writeFile, msg del server: %s\n", msg);
		    free(msg);
			return -1;
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	return 0; 
//
}
		
		
	
	
