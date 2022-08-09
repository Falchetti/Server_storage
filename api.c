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
#include <sys/stat.h>
#include <dirent.h>

#define UNIX_PATH_MAX 108 //lunghezza massima path
#define MSG_SIZE  130 //path + spazio comandi 
#define MAX_SIZE 230 //path, comandi, contenuto file
#define CNT_SIZE 100

#define DEBUG
#undef DEBUG

#define O_OPEN 0
#define O_CREATE 1
#define O_LOCK 2
#define O_CREATE_LOCK 3 

//forse i define li devi mettere in api.h 

//****************
// sono quasi tutte uguali queste funzioni, vedi se puoi accorparle con una funz generale
//che viene chiamata opportunamente (magari col tipo di msg come parametro)
//****************


int fd_s = -1; //fd socket 
char sck_name[UNIX_PATH_MAX]; //nome del socket

//int connected = 0; //mi ricordo se la connessione è avvenuta o meno, serve???
//potrei usare direttamente fd_s

int isTimeout(struct timespec, struct timespec);

int save_file(const char *dir, char *file, char *buff){
	
	char *path_file = malloc(UNIX_PATH_MAX*sizeof(char));
	struct stat st = {0};
//
	if (stat(dir, &st) == -1) {
		if(mkdir(dir, S_IRWXU) == -1){
			perror("Creazione directory");
			return -1;
		}
	}
	else{
		if(!S_ISDIR(st.st_mode)) {
			fprintf(stderr, "%s non e' una directory\n", dir);
			return -1;
		} 
	}
	
	snprintf(path_file, UNIX_PATH_MAX, "%s/%s", dir, file);

	FILE *fp;
	if((fp = fopen(path_file, "w")) == NULL){
		perror("Apertura file");
		return -1;
	}
	if(fprintf(fp, "%s", buff) < 0){
		perror("Scrittura su  file");
		return -1;
	}
	
	if(fclose(fp) == -1){
		perror("Chiusura file");
		return -1;
	}
				
	free(path_file); 
	
	return 0;
}

int msg_sender(char *msg, int fd, char *cmd, const char *path, char *cnt){
	
	int num_w = 0;
	
	strcpy(msg, cmd); //so la taglia max dei cmd no buff overflow
	strcat(msg, ";"); //come sopra
	if(path != NULL)
		strncat(msg, path, MAX_SIZE - strlen(msg) -1); //strncat aggiunge sempre \0 in fondo
	
	if(cnt != NULL){
		strncat(msg, ";", MAX_SIZE - strlen(msg) -1);
		strncat(msg, cnt, MAX_SIZE - strlen(msg) -1);
	}
	strncat(msg, ";$", MAX_SIZE - strlen(msg) -1); //carattere finale 
	
	fprintf(stderr, "PRIMA: %s, t = %d\n", msg, t);
	sleep(t); //è vero che ogni volta che scrivo al server gli sto facendo una richiesta
	fprintf(stderr, "DOPO: %s\n", msg);
	
	if((num_w = write(fd, msg, MSG_SIZE)) == -1){
		perror("Write del socket");
		return -1;
	}		  
	else {
		while(num_w < strlen(msg) + 1){
			msg = msg + MSG_SIZE;
			if((num_w = write(fd, msg, MSG_SIZE)) == -1){
				perror("Write del socket2");
				return -1;
			}
		}
	}
	
	
	return 0;
}


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
			//
	if (t){
		errno = ETIMEDOUT;
		perror("Errore, Timeout sopraggiunto");
		return -1;
	}			
	
	//connected = 1; //serve?
	
	#ifdef DEBUG
		char *buf = malloc(6*sizeof(char));
		if(read(fd_s, buf, 5) == -1){
			perror("read socket lato client");
			return -1; //ok?
	    }
		fprintf(stderr, "Contenuto canale di comunicazione: %s\n", buf);
	    free(buf);
	#endif
	
	strcpy(sck_name, sockname); //memorizzo il nome del socket in una var globale
	
	return 0;
}
//bla v j
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

//ASSICURATI CON TUTTE QUESTE STAMPE CHE ERRNO NON SIA SOVRASCRITTO
//FREE NON SETTA ERRNO, MA PERROR SI' MI SA 

int openFile(const char *pathname, int flags){ //per ora senza OR, ci dovrà essere un controllo sui flags (se sono ammessi)
	
	int errore = 0;
	char *msg, *cmd;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	//int len; 
	
	
	if(fd_s != -1){ //la connessione è aperta? IL SERVER CONTROLLA SE IL FILE E' GIA' APERTO
	
		//len = strlen(pathname);
		msg = malloc(MAX_SIZE*sizeof(char));
		
		switch(flags){
			case O_OPEN: //attenzione a strstr 
			    
				cmd = "openfo";
				break;
				
			case O_CREATE:
				
				cmd = "openfc";
				break;
				
			case O_LOCK:
			
				cmd = "openfl";
				break;
				
			case O_CREATE_LOCK:
				
				cmd = "openfcl";
				break;
				
			default: 
				
				fprintf(stderr, "flag errato\n");
				errno = EINVAL;
				errore = 1;
		}
		
		if(!errore && msg_sender(msg, fd_s, cmd, pathname, NULL) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
		}		
			
		if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			errore = 1;
	    }
		
		if(!errore && strcmp(msg, "Ok") != 0){
			if(strcmp(msg, "Err:fileNonEsistente") == 0)
				errno = ENOENT;
			else{
				fprintf(stderr, "Esito dal server: %s\n", msg);
				errno = -1;//come lo setto errno in questo caso???
			}
			errore = 1;
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}

    free(msg);
	
	if(errore)
		return -1;
	
	return 0;
		
}

int closeFile(const char *pathname){ //rilascia le lock!! 

    int errore = 0;
	char *msg ;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	if(fd_s != -1){
		
		msg = malloc(MAX_SIZE*sizeof(char));
		
		if(!errore && msg_sender(msg, fd_s, "closef", pathname, NULL) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
		}
	
		if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			errore = 1; //ok?
	    }
		if(!errore && strcmp(msg, "Ok") != 0){
			fprintf(stderr, "Err in chiusura file, msg server: %s\n", msg);
		    errno = -1; //come lo setto errno in questo caso???
			errore = 1;
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	free(msg);
	if(errore)
		return -1;
	

return 0; 
}

int readFile(const char *pathname, void **buf, size_t *size) {
	
	
	int errore = 0;
	char *msg ;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	
	if(fd_s != -1){ //controllo se sono connesso 
	
		msg = malloc(MAX_SIZE*sizeof(char));
		
		if(!errore && msg_sender(msg, fd_s, "readf", pathname, NULL) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
		}
		
		if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			errore = 1; 
	    }
		
		if(!errore && strstr(msg, "Ok") == NULL){
			fprintf(stderr, "buff di lettura NULL, msg del server: %s\n", msg); //perchè buff di lettura null?
			errno = -1; //come lo setto errno in questo caso???
		}
		
		if(!errore){// qui mi sa che non ho gestito se il cnt è + grande di MSG_SIZE 
			strcpy((char *) *buf, msg + 2); 
			//memmove(*buf, (char *) *buf + 2, strlen((char *) *buf)); //perchè qui mmve e in altri casi direttamente *buf +2?
			*size = strlen(*buf); //va bene?	
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	free(msg);

	if(errore)
		return -1;

	return 0; 
}
	
int lockFile(const char *pathname){
	
	int errore = 0;
	char *msg ;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	
	if(fd_s != -1){
		
		msg = malloc(MAX_SIZE*sizeof(char));
		
		if(!errore && msg_sender(msg, fd_s, "lockf", pathname, NULL) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
		}
		
		if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			errore = 1;
	    }
		
		if(!errore && strcmp(msg, "Ok") != 0){
			fprintf(stderr, "Esito dal server: %s\n", msg);
			errno = -1;//come lo setto errno in questo caso???
			errore = 1;
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	free(msg);

	if(errore)
		return -1;

	return 0;
}

int unlockFile(const char *pathname){
	
	int errore = 0;
	char *msg;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	if(fd_s != -1){
		
		msg = malloc(MAX_SIZE*sizeof(char));
		
		if(!errore && msg_sender(msg, fd_s, "unlockf", pathname, NULL) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
		}
		
		if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			errore = 1;
		}
		if(!errore && strcmp(msg, "Ok") != 0){
			fprintf(stderr, "Esito dal server: %s\n", msg);
			errno = -1;//come lo setto errno in questo caso???
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	free(msg);

	if(errore)
		return -1;
	
	return 0;
}

int writeFile(const char *pathname, const char* dirname) {
	
	int errore = 0;
	char *msg, *aux, *aux2; //vedi se devi allocare spazio per aux  
	FILE *fp;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
		
	if(fd_s != -1){ //controllo se sono connesso 
	
		msg = malloc(MAX_SIZE*sizeof(char));
		
		if((fp = fopen(pathname, "r")) == NULL){ //HO PROBLEMI SUI FILE CHE NON ESISTONO, è ok?
			perror("errore nella fopen"); //devo controllare se va a buon fine SI
            free(msg);
            return -1;
        }			
		
		aux = malloc(MSG_SIZE*sizeof(char));
		errno = 0; //ok?
   
        aux2 = aux;
        while (!feof(fp)){
			fgets(aux2, MSG_SIZE, fp);
			aux2 = aux2 + strlen(aux2);
		}
		
		if(errno != 0){//va bene questo controllo? va bene come ho aggiornato il file?
			perror("fgets fallita\n");
			return -1;
		}
	
		fclose(fp);
		
		if(!errore && msg_sender(msg, fd_s, "writef", pathname, aux) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
		}
	 
		if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			errore = 1; //ok?
	    }
		
		if(!errore &&  strcmp(msg, "Ok") != 0){
			fprintf(stderr, "errore writeFile, msg del server: %s\n", msg);
		    errno = -1; //come lo setto errno in questo caso???
			errore = 1;
		}		
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	free(aux);
	free(msg);
	
	if(errore){
		return -1;
	}

	return 0; 
}

int appendToFile(const char *pathname, void *buf, size_t size, const char* dirname) {
	
	int errore = 0;
	char *msg;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	if(fd_s != -1){ //controllo se sono connesso 
		
		msg = malloc(MAX_SIZE*sizeof(char));
		
		if(!errore && msg_sender(msg, fd_s, "appendTof", pathname, buf) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
		}
		
		if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			errore = 1;
	    }
		
		if(!errore && strcmp(msg, "Ok") != 0){
			fprintf(stderr, "errore writeFile, msg del server: %s\n", msg);
			errno = -1; //come lo setto errno in questo caso???
			errore = 1;
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	free(msg);
	
	if(errore){
		return -1;
	}
	
	return 0; 
}

int removeFile(const char *pathname){
	
	int errore = 0;
	char *msg ;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	
	if(fd_s != -1){
		
		msg = malloc(MAX_SIZE*sizeof(char));
		
		if(!errore && msg_sender(msg, fd_s, "removef", pathname, NULL) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
		}
		
		if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			errore = 1;
	    }
		
		if(!errore && strcmp(msg, "Ok") != 0){
			if(strstr(msg, "fail") == NULL){
				errno = -1;//come lo setto errno in questo caso???
				errore = 1;
			}
			fprintf(stderr, "Esito dal server: %s\n", msg);
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	free(msg);

	if(errore)
		return -1;

	return 0;
}


int readNFile(int n, const char *dirname){  //su client hai funzione che salva file in directory, vedi se spostarla qui 
	int errore = 0;
	char *msg, *tmpstr;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	char *token_name, *token_cnt, *token_flag; //devo allocare spazio per loro?
	
	
	if(fd_s != -1){ //ci pensa il server ad aprire e chiudere i file in questo caso 
		
		msg = malloc(MAX_SIZE*sizeof(char));
		
		if(n <= 0){
			if(!errore && msg_sender(msg, fd_s, "readAllf", NULL, NULL) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
			} 
			
			if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
				perror("read socket lato client");
				errore = 1;  //guarda se va bene questa gestione dell'errore 
		    }
		
			token_flag = strtok_r(msg, ";", &tmpstr);
			
			while(!errore && token_flag != NULL && strcmp(token_flag, "Ok") == 0){
				
				token_name = strtok_r(NULL, ";", &tmpstr);
				token_cnt = strtok_r(NULL, ";", &tmpstr); //dovrei controllare lo spazio sia sufficiente per il contenuto 
				if(dirname != NULL){
					if(save_file(dirname, token_name, token_cnt) == -1){
						//serve una stampa?
						errore = 1;
					}
				}
				if(!errore)
					write(fd_s, "Ok", 3);
				if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
					perror("read socket lato client");
					errore = 1;
				}
				token_flag = strtok_r(msg, ";", &tmpstr);
			}	
		}
		else{
			while(!errore && n > 0){ //ATTENZIONE, IN QUESTO MODO POSSO RILEGGERE PIù VOLTE LO STESSO FILE 
				if(msg_sender(msg, fd_s, "readAf", NULL, NULL) == -1){
					fprintf(stderr, "Errore in invio messaggio al server\n");
					errore = 1;
				}
				n--;
				if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
					perror("read socket lato client");
					errore = 1;
				}
				if(!errore && strcmp(strtok_r(msg, ";", &tmpstr), "Ok") == 0){
					token_name = strtok_r(NULL, ";", &tmpstr);
					token_cnt = strtok_r(NULL, ";", &tmpstr); //dovrei controllare lo spazio sia sufficiente per il contenuto 
					
					if(dirname != NULL){
						if(save_file(dirname, token_name, token_cnt) == -1){
							errore = 1;
						}
					}
				}
			}	
		}

	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	free(msg);

	if(errore)
		return -1;

	return 0;
}



	
	