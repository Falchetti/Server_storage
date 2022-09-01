#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <dirent.h>

#include "api.h" 

#define UNIX_PATH_MAX 108 //lunghezza massima path
#define MSG_SIZE  230 //path + spazio comandi 
#define MAX_SIZE 230 //path, comandi, contenuto file
#define CNT_SIZE 100

#define DEBUG
#undef DEBUG

#define O_OPEN 0
#define O_CREATE 1
#define O_LOCK 2
#define O_CREATE_LOCK 3 

//forse i define li devi mettere in api.h, leggi commento su client.c  

//****************
// sono quasi tutte uguali queste funzioni, vedi se puoi accorparle con una funz generale (es: f_comunic)
//che viene chiamata opportunamente (magari col tipo di msg come parametro)
//il protocollo è: msg_sender - read - strcmp o strstr [in realtà questo strncmp varia a seconda del comando]
//****************

//gestire problema max_size e msg_size 
//usare scelta coerente per strcmp , strcat e strcpy

int fd_s = -1; //fd socket 
char sck_name[UNIX_PATH_MAX]; //nome del socket


//forse ha senso metterle direttamente in api.h
int isTimeout(struct timespec, struct timespec);  
int save_file(const char *dir, char *file, char *buff, int n);
int msg_sender(char *msg, int fd, char *cmd, const char *path, int size, char *cnt);
int isNumber(void *el, int *n);
int file_receiver(const char *dirname, int fd);

//non devo controllare che la connessione sia stata già aperta
//perchè -f lo posso chiamare solo una volta e solo lì invoco openConnection
//inoltre la connessione è unica per ogni client 
int openConnection(const char* sockname, int msec, const struct timespec abstime){ 


	int t = 0;
	errno = 0;
	
	if((fd_s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1){ //errno lo setta socket
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
		errno = ETIMEDOUT; //forse dovrei lasciare l'errno settato da connect 
		perror("Errore, timeout sopraggiunto");
		return -1;
	}			
	
	
	strncpy(sck_name, sockname, UNIX_PATH_MAX); //memorizzo il nome del socket in una var globale
    if (sck_name[UNIX_PATH_MAX] != '\0'){
		errno = ENAMETOOLONG;
		return -1;
	}

	
	return 0;
}

//controllo se l'orario a è maggiore di b 
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
	//ho scelto di lasciare strcmp perchè usare la versione con la n non aumentava la sicurezza 
	//https://stackoverflow.com/questions/30190460/advantages-of-strncmp-over-strcmp 
	//so per certo che sck_name è null-terminated (me ne sono assicurata in open connection)
	//non ho la certezza di sockname però 
	if(strcmp(sockname, sck_name) == 0){ 	
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

int openFile(const char *pathname, int flags){ 
	
	int errore = 0;
	char *msg, *cmd;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0 
	
	
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
				errore = 1; //credo di aver usato questa variabile per non dover fare la free ogni volta (non so quanto sia meglio sinceramente)
		}
		
		if(!errore && msg_sender(msg, fd_s, cmd, pathname, -1, NULL) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
			errno = -1;//come lo setto errno in questo caso???
		}		
			
		if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			errore = 1;
	    }
		
		if(!errore && strncmp(msg, "Ok", 3) != 0){
			if(strncmp(msg, "Err:fileNonEsistente", MSG_SIZE) == 0) 
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
		
		if(!errore && msg_sender(msg, fd_s, "closef", pathname, -1, NULL) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errno = -1; //come lo setto errno in questo caso???
			errore = 1;
		}

	
	
		if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			errore = 1; //ok?
	    }
				
		if(!errore && strncmp(msg, "Ok", 3) != 0){
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
	
	int n;
	int errore = 0;
	char *msg ;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	
	if(fd_s != -1){ //controllo se sono connesso 
	
		msg = malloc(MAX_SIZE*sizeof(char));
		
		if(!errore && msg_sender(msg, fd_s, "readf", pathname, -1, NULL) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
		}
		
		if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			errore = 1; 
	    }
		
		if(!errore && isNumber(msg, &n) != 0){ //controlla se strstr è ok (not null terminated strings)
			fprintf(stderr, "buff di lettura NULL, msg del server: %s\n", msg); //perchè buff di lettura null?
			errno = -1; //come lo setto errno in questo caso???
		}
		if(!errore && n > 0){ //controlla se strstr è ok (not null terminated strings)
			if(!errore && read(fd_s, msg, n) == -1){
			perror("read socket lato client");
			errore = 1; 
	        }    
			memcpy(*buf, msg, n);
			*size = n;
		}
		else{
			*buf = NULL;
			*size = 0;
		}
		
		/*if(!errore){// qui mi sa che non ho gestito se il cnt è + grande di MSG_SIZE 
			strcpy((char *) *buf, msg + 2); 
			//memmove(*buf, (char *) *buf + 2, strlen((char *) *buf)); //perchè qui mmve e in altri casi direttamente *buf +2?
			*size = strlen(*buf); //va bene?	
		}*/
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
		
		if(!errore && msg_sender(msg, fd_s, "lockf", pathname, -1, NULL) == -1){
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
		
		if(!errore && msg_sender(msg, fd_s, "unlockf", pathname, -1, NULL) == -1){
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
	char *msg;
    char cnt[MAX_SIZE];   
	FILE *fp;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	struct stat st;
	int size, n, k;

	
	if(fd_s != -1){ //controllo se sono connesso 
	
		msg = malloc(MAX_SIZE*sizeof(char));
		
		if((fp = fopen(pathname, "rb")) == NULL){ //Se il file non esiste chiaramente si esce con errore
			perror("errore nella fopen"); 
            free(msg);
            return -1;
        }			
		
		stat(pathname, &st);
        size = st.st_size;
		
		memset(cnt, 0, MAX_SIZE);
		n = fread(cnt, 1, size, fp);
		
        if(!n){
			if (ferror(fp)){    
				perror("fread");
        		errore = 1;
			}
		}
	
	
		fclose(fp);
		
		if(!errore && msg_sender(msg, fd_s, "writef", pathname, size, cnt) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
		}
			

		k = file_receiver(dirname, fd_s); 
		fprintf(stderr, "k = %d\n", k);
		if(k == -1)
			return 0; //vedi come gestirlo 
	   
		if(!errore && read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			errore = 1; //ok?
	    }
		//fprintf(stderr, "read client finale: %s\n", msg);
	
		
		if(!errore &&  strcmp(msg, "Ok") != 0){ //QUI SARA' DA AGGIUNGERE LA PARTE DEI FILE ESPULSI 
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

int appendToFile(const char *pathname, void *buf, size_t size, const char* dirname) {
	
	int errore = 0;
	char *msg;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	int k;  
	
	if(fd_s != -1){ //controllo se sono connesso 
		
		msg = malloc(MAX_SIZE*sizeof(char));
		
		if(!errore && msg_sender(msg, fd_s, "appendTof", pathname, size, buf) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
		}
		
		k = file_receiver(dirname, fd_s); 
		
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


int readNFile(int n, const char *dirname){  //su client hai funzione che salva file in directory, vedi se spostarla qui 
	
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	int k;
	char str[MAX_SIZE/10+2]; //cambiare
	char *msg = malloc(MAX_SIZE*sizeof(char));
	int errore = 0;
	
	if(fd_s != -1){ //ci pensa il server ad aprire e chiudere i file in questo caso 
		snprintf(str, MAX_SIZE/10+2, "%d", n);	
		if(!errore && msg_sender(msg, fd_s, "readNf", str, -1, NULL) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			errore = 1;
		}
		k = file_receiver(dirname, fd_s);			
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	

	if(errore)
		return -1;
	
	free(msg);
	return k;
}


int removeFile(const char *pathname){
	
	int errore = 0;
	char *msg ;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	
	if(fd_s != -1){
		
		msg = malloc(MAX_SIZE*sizeof(char));
		
		if(!errore && msg_sender(msg, fd_s, "removef", pathname, -1, NULL) == -1){
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


int save_file(const char *dir, char *file, char *buff, int size){
	
	char *path_file = malloc(UNIX_PATH_MAX*sizeof(char));
	struct stat st = {0};

	if (stat(dir, &st) == -1) { //se la directory non esiste la creo 
		if(mkdir(dir, S_IRWXU) == -1){ 
			perror("Creazione directory");
			return -1;
		}
	}
	else{
		if(!S_ISDIR(st.st_mode)) { //a meno che non ci sia un file omonimo
			fprintf(stderr, "%s non e' una directory\n", dir);
			return -1;
		} 
	}
	//	VEDI COME AGGIUSTARE QUESTA COSA DEI PATH
	char *tmpstr, *token, *token2;
	token = strtok_r(file, "/", &tmpstr);
	while(token != NULL){
		token2 = token;
		token = strtok_r(NULL, "/", &tmpstr);
	}
	//
	snprintf(path_file, UNIX_PATH_MAX, "%s/%s", dir, token2);

	FILE *fp;

	if((fp = fopen(path_file, "wb")) == NULL){
		perror("Apertura file");
		return -1;
	}
	if(fwrite(buff, size, 1, fp) == 0 ){
		if (ferror(fp)){    
			perror("Scrittura su  file");
		    return -1;
		}
	}
	
	if(fclose(fp) == -1){
		perror("Chiusura file");
		return -1;
	}
				
	free(path_file); 
	
	return 0;
}

int msg_sender(char *msg, int fd, char *cmd, const char *path, int size, char *cnt){
	
	//int num_w = 0;
	char str[MAX_SIZE/10+2]; //mah, controlla se va bene 
	
	
	
	strncpy(msg, cmd, MAX_SIZE - 1); //so la taglia max dei cmd no null-terminated problem
	strcat(msg, ";"); //come sopra (non voglio appesantire il codice)
	
	if(path != NULL){
		strncat(msg, path, MAX_SIZE - strlen(msg) -1); //strncat aggiunge sempre \0 in fondo
		strncat(msg, ";", MAX_SIZE - strlen(msg) -1);
		if(size == -1)
			snprintf(str, MAX_SIZE/10+2, "%d", -1);
		else
			snprintf(str, MAX_SIZE/10+2, "%d", size);
		strncat(msg, str, MAX_SIZE - strlen(msg) -1);
		
		
		if(write(fd, msg, MSG_SIZE) == -1){ //AGGIUSTA QUESTA COSA DI MSG_SIZE E MAX_SIZE A SECONDA DEL TEMPO CHE HAI A DISPOSIZIONE
			perror("Write del socket");
			return -1;
		}
		if(size > 0){
			if(read(fd, msg, MAX_SIZE) == -1){ //AGGIUSTA QUESTA COSA DI MSG_SIZE E MAX_SIZE A SECONDA DEL TEMPO CHE HAI A DISPOSIZIONE
				perror("Write del socket");
				return -1;
			}
			if(strncmp(msg, "Ok", 3) == 0){
				if(write(fd, cnt, size) == -1){ //AGGIUSTA QUESTA COSA DI MSG_SIZE E MAX_SIZE A SECONDA DEL TEMPO CHE HAI A DISPOSIZIONE
					perror("Write del socket");
					return -1;
				}
			}
			else{
				fprintf(stderr, "Errore ricezione messaggio dal server\n");
				return -1;
			}
		}	
		
		
		//strncat(msg, ";$", MAX_SIZE - strlen(msg) -1); //carattere finale, serve??
		
		/*if((num_w = write(fd, msg, MSG_SIZE)) == -1){ //AGGIUSTA QUESTA COSA DI MSG_SIZE E MAX_SIZE A SECONDA DEL TEMPO CHE HAI A DISPOSIZIONE
			perror("Write del socket");
			return -1;
		}		  
		else {
			while(num_w < strlen(msg) + 1){ //LA PARTE DEL CONTENUTO DEI FILE TROPPO GRANDE E' DA AGGIUSTARE
				msg = msg + MSG_SIZE;
				if((num_w = write(fd, msg, MSG_SIZE)) == -1){
					perror("Write del socket2");
					return -1;
				}
			}
		}*/
		
		
	}
	else{
		fprintf(stderr, "path NULL in msg_sender\n");
		return -1;
	}
	
	return 0;
}


int file_receiver(const char *dirname, int fd){

	int stop = 0;
	int errore = 0;
	char *token_name, token_cnt[MAX_SIZE], *token_sz, *tmpstr; //devo allocare spazio per loro?
	char *msg = malloc(MAX_SIZE*sizeof(char));
	int k = 0;
	int sz,res;

	while(!errore && !stop){
		
		if(!errore && read(fd, msg, MSG_SIZE) == -1){ //path;sz
				perror("read socket lato client");
				errore = 1;  //guarda se va bene questa gestione dell'errore 
		}
		
		if(strstr(msg, "Err:") != NULL){ //controlla se va bene 
			fprintf(stderr, "MESSAGGIO: %s\n", msg);
			return -1;
		}
		//fprintf(stderr, "read path;sz o $ client: %s\n", msg);

		token_name = strtok_r(msg, ";", &tmpstr);
		if(strncmp(token_name, "$", UNIX_PATH_MAX) != 0 && !errore){
			k++;
			token_sz = strtok_r(NULL, ";", &tmpstr);
				
			if((res = isNumber(token_sz, &sz)) == 0 && sz > 0){
				write(fd, "Ok", MSG_SIZE); //gestione
				//fprintf(stderr, "write client dopo path;sz\n");
		
				if(!errore && read(fd, token_cnt, sz) == -1){ 
					perror("read contenuto file");
					errore = 1;  //guarda se va bene questa gestione dell'errore 
				}
				//fprintf(stderr, "read client del contenuto\n");
				write(fd, "Ok", MSG_SIZE); //gestione
				//fprintf(stderr, "write client dopo cnt: ok\n");
		
				if(dirname != NULL){
					if(save_file(dirname, token_name, token_cnt, sz) == -1){
						fprintf(stderr, "Errore in savefile\n");
						errore = 1;
					}
				}
			}
			else{
				if(res != 0 || sz < 0)
					fprintf(stderr, "Errore file_receiver, sz cnt inviata da server non è un numero o negativa\n");
				write(fd, "Ok", MSG_SIZE); //gestione
			//	fprintf(stderr, "write client quando sz = 0: Ok\n");
				if(!errore && read(fd, msg, sz) == -1){ 
					perror("read risposta server");
					errore = 1;  //guarda se va bene questa gestione dell'errore 
				}
			//	fprintf(stderr, "read client quando sz = 0: \n", msg);
				if(dirname != NULL){
					if(save_file(dirname, token_name, NULL, 0) == -1){
						fprintf(stderr, "Errore in savefile\n");
						errore = 1;
					}
				}
			}
			
			
		}
		else{
			stop = 1; 
		}
	}
	write(fd, "Ok", MSG_SIZE);
	//fprintf(stderr, "write client finale\n");
	free(msg);
	
	return k;
}
			
		

int isNumber(void *el, int *n){
	char *e = NULL;
	errno = 0;
	*n = strtol(el, &e, 10);
	if(errno == ERANGE) return 2;
	if(e != NULL && *e == (char) 0)
		return 0; 
	return 1;
}



	
	