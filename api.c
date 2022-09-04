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

int fd_s = -1; //fd socket, controlla che tutti la leggano e nessuno ci scriva (tranne open_conn/close_conn)
char sck_name[UNIX_PATH_MAX]; //nome del socket


//forse ha senso metterle direttamente in api.h
int isTimeout(struct timespec, struct timespec);  
int save_file(const char *dir, char *file, char *buff, int n);
int msg_sender(char *msg, char *cmd, const char *path, int size, char *cnt);
int isNumber(void *el, int *n);
int file_receiver(const char *dirname, int fd);
int comunic_cs(char *cmd, const char *pathname, int *err);

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
	
	char *cmd;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0 
	
	
	if(fd_s != -1){ //la connessione è aperta? (Il server controllerà se il file sia già stato aperto o meno)
		
		switch(flags){
			case O_OPEN: 
			    
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
				return -1; 
		}
		int err, res;
		res = comunic_cs(cmd, pathname, &err);
		errno = err;
		if(res < 0)
			return res; //ATTENZIONE: guarda le specifiche delle api!!!
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	return 0;
		
}

int closeFile(const char *pathname){ //rilascia le lock!! 

	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
						
	if(fd_s != -1){
		int err, res;
		res = comunic_cs("closef", pathname, &err);
		errno = err;
		if(res < 0)
			return res;
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
return 0; 
}

int readFile(const char *pathname, void **buf, size_t *size) {
	
	int n;
	char *msg ;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	
	if(fd_s != -1){ //controllo se sono connesso 

		if((msg = malloc(MAX_SIZE*sizeof(char))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		
		if( msg_sender(msg, "readf", pathname, -1, NULL) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			free(msg);
			return -1;
		}
		
		if(read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			free(msg);
			return -1; 
	    }
		
		if(isNumber(msg, &n) != 0){ //controlla se strstr è ok (not null terminated strings)
			fprintf(stderr, "buff di lettura NULL, msg del server: %s\n", msg); //perchè buff di lettura null?
			errno = -1; //come lo setto errno in questo caso???
		}
		if(n > 0){ //controlla se strstr è ok (not null terminated strings)
			if(read(fd_s, msg, n) == -1){
				perror("read socket lato client");
				free(msg);
				return -1;
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

	return 0; 
}
	
int lockFile(const char *pathname){

	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	if(fd_s != -1){
		int err, res;
		res = comunic_cs("lockf", pathname, &err);
		errno = err;
		if(res < 0)
			return res;
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}

	return 0;
}

int unlockFile(const char *pathname){

	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	if(fd_s != -1){
		int err, res;
		res = comunic_cs("unlockf", pathname, &err);
		errno = err;
		if(res < 0)
			return res;
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	return 0;
}

int writeFile(const char *pathname, const char* dirname) {
	
	char *msg;
    char cnt[MAX_SIZE];   
	FILE *fp;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	struct stat st;
	int size, n;

	
	if(fd_s != -1){ //controllo se sono connesso 
	
		if((msg = malloc(MAX_SIZE*sizeof(char))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		
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
        		free(msg);
				return -1;
			}
		}
	
		fclose(fp);
		
		if(msg_sender(msg, "writef", pathname, size, cnt) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			free(msg);
			return -1;
		}
			

		n = file_receiver(dirname, fd_s); 
		
		if(n == -1){
			free(msg);
			return 0; //vedi come gestirlo 
		}
	   
		if(read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			free(msg);
			return -1;
	    }
		//fprintf(stderr, "read client finale: %s\n", msg);
	
		
		if(strcmp(msg, "Ok") != 0){ 
			fprintf(stderr, "errore writeFile, msg del server: %s\n", msg);
		    errno = -1; //come lo setto errno in questo caso???
			free(msg);
			return -1;
		}	
		
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	free(msg);

	return 0; 
}

int appendToFile(const char *pathname, void *buf, size_t size, const char* dirname) {
	
	char *msg;
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	int k;  
	
	if(fd_s != -1){ //controllo se sono connesso 
		
		if((msg = malloc(MAX_SIZE*sizeof(char))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		
		if(msg_sender(msg, "appendTof", pathname, size, buf) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			free(msg);
			return -1;
		}
		
		k = file_receiver(dirname, fd_s); 
		
		if(k == -1)
			return 0; //vedi come gestirlo 
		
		if(read(fd_s, msg, MSG_SIZE) == -1){
			perror("read socket lato client");
			free(msg);
			return -1;
	    }
		
		if(strcmp(msg, "Ok") != 0){
			fprintf(stderr, "errore writeFile, msg del server: %s\n", msg);
			errno = -1; //come lo setto errno in questo caso???
			free(msg);
			return -1;
		}
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	free(msg);

	return 0; 
}


int readNFile(int n, const char *dirname){  //su client hai funzione che salva file in directory, vedi se spostarla qui 
	
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	int k;
	char str[MAX_SIZE/10+2]; //cambiare
	char *msg;
	
	if(fd_s != -1){ //ci pensa il server ad aprire e chiudere i file in questo caso 
		if((msg = malloc(MAX_SIZE*sizeof(char))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		
		snprintf(str, MAX_SIZE/10+2, "%d", n);	
		if(msg_sender(msg, "readNf", str, -1, NULL) == -1){
			fprintf(stderr, "Errore in invio messaggio al server\n");
			free(msg);
			return -1;
		}
		k = file_receiver(dirname, fd_s);			
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	free(msg);
	return k; //ATTENZIONE: specifiche api 
}


int removeFile(const char *pathname){
	
	errno = 0; //controlla se è ok questa cosa di inizializzare errno a 0
	
	if(fd_s != -1){
		int err, res;
		res = comunic_cs("removef", pathname, &err);
		errno = err;
		if(res < 0)
			return res;
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}

	return 0;
}


int save_file(const char *dir, char *file, char *buff, int size){
	
	char *path_file;
	struct stat st = {0};
	
	if((path_file = malloc(UNIX_PATH_MAX*sizeof(char))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		exit(errno_copy);
	}

	if (stat(dir, &st) == -1) { //se la directory non esiste la creo 
		if(mkdir(dir, S_IRWXU) == -1){ 
			perror("Creazione directory");
			free(path_file);
			return -1;
		}
	}
	else{
		if(!S_ISDIR(st.st_mode)) { //a meno che non ci sia un file omonimo
			fprintf(stderr, "%s non e' una directory\n", dir);
			free(path_file);
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

	snprintf(path_file, UNIX_PATH_MAX, "%s/%s", dir, token2);

	FILE *fp;

	if((fp = fopen(path_file, "wb")) == NULL){
		perror("Apertura file");
		free(path_file);
		return -1;
	}
	if(fwrite(buff, size, 1, fp) == 0 ){
		if (ferror(fp)){    
			perror("Scrittura su  file");
			free(path_file);
		    return -1;
		}
	}
	
	if(fclose(fp) == -1){
		perror("Chiusura file");
		free(path_file);
		return -1;
	}
				
	free(path_file); 
	
	return 0;
}

int comunic_cs(char *cmd, const char *pathname, int *err){
	char *msg;
	
	if((msg = malloc(MAX_SIZE*sizeof(char))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		exit(errno_copy);
	}
	
	if(msg_sender(msg, cmd, pathname, -1, NULL) == -1){
		fprintf(stderr, "Errore in invio messaggio al server\n");
		*err = -1; //come lo setto errno in questo caso???
		free(msg);
		return -1;
	}

	if(read(fd_s, msg, MSG_SIZE) == -1){
		perror("read socket lato client");
		*err = errno;
		free(msg);
		return -1;
	}
			
	if(strncmp(msg, "Ok", 3) != 0){ //gestisci i valori di return 
		if(strstr(msg, "fail") != NULL){ //o fatal, da decidere
			*err = -1;//come lo setto errno in questo caso???
		}
		else if(strncmp(msg, "Err:fileNonEsistente", MSG_SIZE) == 0){
			*err = ENOENT;
		}
		else
			*err = -1; //come lo setto errno in questo caso???
		fprintf(stderr, "Err in %s, msg server: %s\n", cmd, msg);
		
		free(msg);
		return -1;
	}
	free(msg);
	return 0;
}

int msg_sender(char *msg, char *cmd, const char *path, int size, char *cnt){
	
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
		
		
		if(write(fd_s, msg, MSG_SIZE) == -1){ //AGGIUSTA QUESTA COSA DI MSG_SIZE E MAX_SIZE A SECONDA DEL TEMPO CHE HAI A DISPOSIZIONE
			perror("Write del socket");
			return -1;
		}
		if(size > 0){
			if(read(fd_s, msg, MAX_SIZE) == -1){ //AGGIUSTA QUESTA COSA DI MSG_SIZE E MAX_SIZE A SECONDA DEL TEMPO CHE HAI A DISPOSIZIONE
				perror("Write del socket");
				return -1;
			}
			if(strncmp(msg, "Ok", 3) == 0){
				if(write(fd_s, cnt, size) == -1){ //AGGIUSTA QUESTA COSA DI MSG_SIZE E MAX_SIZE A SECONDA DEL TEMPO CHE HAI A DISPOSIZIONE
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
		
		/*if((num_w = write(fd_s, msg, MSG_SIZE)) == -1){ //AGGIUSTA QUESTA COSA DI MSG_SIZE E MAX_SIZE A SECONDA DEL TEMPO CHE HAI A DISPOSIZIONE
			perror("Write del socket");
			return -1;
		}		  
		else {
			while(num_w < strlen(msg) + 1){ //LA PARTE DEL CONTENUTO DEI FILE TROPPO GRANDE E' DA AGGIUSTARE
				msg = msg + MSG_SIZE;
				if((num_w = write(fd_s, msg, MSG_SIZE)) == -1){
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
	char *token_name, token_cnt[MAX_SIZE], *token_sz, *tmpstr; //devo allocare spazio per loro?
	char *msg;
	int k = 0;
	int sz,res;
	
	if((msg = malloc(MAX_SIZE*sizeof(char))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		exit(errno_copy);
	}

	while(!stop){
		
		if(read(fd, msg, MSG_SIZE) == -1){ //path;sz
			perror("read socket lato client");
			free(msg);
			return -1;
		}
		
		if(strstr(msg, "Err:") != NULL){ //controlla se va bene 
			fprintf(stderr, "MESSAGGIO: %s\n", msg);
			free(msg);
			return -1;
		}
	
		token_name = strtok_r(msg, ";", &tmpstr);
		if(strncmp(token_name, "$", UNIX_PATH_MAX) != 0){
			k++;
			token_sz = strtok_r(NULL, ";", &tmpstr);
				
			if((res = isNumber(token_sz, &sz)) == 0 && sz > 0){
				write(fd, "Ok", MSG_SIZE); //gestione
		
				if(read(fd, token_cnt, sz) == -1){ 
					perror("read contenuto file");
					free(msg);
					return -1;
				}
				write(fd, "Ok", MSG_SIZE); 
		
				if(dirname != NULL){
					if(save_file(dirname, token_name, token_cnt, sz) == -1){
						fprintf(stderr, "Errore in savefile\n");
						free(msg);
						return -1;
					}
				}
			}
			else{
				if(res != 0 || sz < 0)
					fprintf(stderr, "Errore file_receiver, sz cnt inviata da server non è un numero o negativa\n");
				write(fd, "Ok", MSG_SIZE); //gestione
				if(read(fd, msg, sz) == -1){ 
					perror("read risposta server");
					free(msg);
					return -1;
				}
				if(dirname != NULL){
					if(save_file(dirname, token_name, NULL, 0) == -1){
						fprintf(stderr, "Errore in savefile\n");
						free(msg);
						return -1;
					}
				}
			}	
		}
		else{
			stop = 1; 
		}
	}
	write(fd, "Ok", MSG_SIZE);
	
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



	
	