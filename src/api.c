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

#include <api.h>
#include <conn.h>
#include <defines.h> 
 
#define DEBUG
#undef DEBUG

/**
 * @file api.c
 * @brief File di implementazione dell'interfaccia per la comunicazione con lo storage  
 */


//variabili globali 
int fd_s = -1; //fd socket
char sck_name[UNIX_PATH_MAX]; //nome del socket

/* ------------------- funzioni di supporto -------------------- */
int isTimeout(struct timespec);  
int comunic_cs(char *cmd, const char *pathname, int *err);
int msg_sender(char *msg, char *cmd, const char *path, int size, char *cnt);
int file_receiver(const char *dirname);
int error_handler(char *msg, int sz);

/* ------------------- interfaccia ------------------ */

int openConnection(const char* sockname, int msec, const struct timespec abstime){ 

	int t = 0;
	
	if(strlen(sockname) >= UNIX_PATH_MAX){
		errno = ENAMETOOLONG;
		return -1;
	}
	
	if((fd_s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1){ 
		perror("Errore in socket [openConnection]");
		return -1;
	}
	
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(struct sockaddr_un)); 
	strncpy(sa.sun_path, sockname, UNIX_PATH_MAX); 
	sa.sun_family = AF_UNIX;
	
	//tentativi di connessione 
	while ( (connect(fd_s, (struct sockaddr *) &sa, sizeof(sa)) == -1) && ((t = isTimeout(abstime)) == 0) )
		sleep(msec/1000); 
			
	if (t){
		errno = ETIMEDOUT; 
		perror("Errore, timeout sopraggiunto [openConnection]");
		return -1;
	}		
	
	//memorizza il nome del socket in una var globale
	strncpy(sck_name, sockname, UNIX_PATH_MAX); 
	
	return 0;
}

int closeConnection(const char *sockname){

	if(strncmp(sockname, sck_name, UNIX_PATH_MAX) == 0){ 	
		if(fd_s != -1){ 
			char *mess = "disconnesso";
		    int n = strlen(mess) + 1; 
			
			//invio richiesta al server 
			if(writen(fd_s, &n, sizeof(int)) == -1){
				perror("Errore nella write [closeConnection]");
				return -1;
			}
			if(writen(fd_s, "disconnesso", n) == -1){
				perror("Errore nella write");
				return -1;
			}
			//chiusura descrittore 
		    if(close(fd_s) == -1){
				perror("Errore nella close [closeConnection]");
				return -1;
			}
		}
		//segnala che il descrittore è chiuso
		fd_s = -1; 
	}
	else{
		errno = EINVAL;
		perror("Errore parametro [closeConnection]");
		return -1;
	}
	return 0;
}

int openFile(const char *pathname, int flags){ 
	
	char *cmd;
	int err = 0;
	int res;
	
	if(fd_s != -1){ //controlla che la connessione sia aperta
		
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
		
		//comunicazione con il server 
		res = comunic_cs(cmd, pathname, &err);
		errno = err; 
		if(res < 0)
			return -1; 
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	return 0;	
}

int closeFile(const char *pathname){ 
						
	if(fd_s != -1){
		int err = 0;
		//comunicazione con il server
		int res = comunic_cs("closef", pathname, &err);
		errno = err;
		if(res < 0)
			return -1;
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
return 0; 
}

int lockFile(const char *pathname){ 
	
	if(fd_s != -1){
		int err = 0;
		//comunicazione con il server
		int res = comunic_cs("lockf", pathname, &err);
		errno = err;
		if(res < 0)
			return -1;
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}

	return 0;
}

int unlockFile(const char *pathname){
	
	if(fd_s != -1){
		int err = 0;
		//comunicazione con il server
		int res = comunic_cs("unlockf", pathname, &err);
		errno = err;
		if(res < 0)
			return -1;
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}
	
	return 0;
}

int removeFile(const char *pathname){ 
	
	if(fd_s != -1){
		int err = 0;
		//comunicazione con il server
		int res = comunic_cs("removef", pathname, &err);
		errno = err;
		if(res < 0)
			return -1;
	}
	else{
		errno = ENOTCONN;
		perror("Connessione chiusa");
		return -1;
	}

	return 0;
}

int readFile(const char *pathname, void **buf, size_t *size) {
	
	int sz_msg;
	char *msg ;
	
	if(fd_s != -1){ 
		//invio richiesta al server 
		if((msg = malloc(MAX_SIZE*sizeof(char))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		if(msg_sender(msg, "readf", pathname, -1, NULL) == -1){
			perror("Errore in invio messaggio al server"); 
			free(msg);
			return -1;
		}			
		
		free(msg);
		
		//lettura contenuto file ricevuto dal server (se sz_cnt positiva)
		if(readn(fd_s, &sz_msg, sizeof(int)) == -1){
			perror("read socket lato client");
			return -1; 
	    }
		if(sz_msg > 0){
			if((msg = malloc(sz_msg*sizeof(char))) == NULL){
				perror("malloc");
				int errno_copy = errno;
				fprintf(stderr,"FATAL ERROR: malloc\n");
				exit(errno_copy);
			}
			if(readn(fd_s, msg, sz_msg) == -1){
				perror("read socket lato client");
				free(msg);
				return -1; 
			}
			//controlla se è stato ricevuto messaggio d'errore
			if(strncmp(msg, "Err:", 4) == 0){ 
				#ifdef DEBUG
					fprintf(stderr, "Err in readF, msg server: %s\n", msg);
				#endif
				errno = error_handler(msg, sz_msg);
				free(msg);
				return -1;
			}
			//salvataggio contenuto file 
			memcpy(*buf, msg, sz_msg);
			*size = sz_msg;
			free(msg);
		}
		else{
			*buf = NULL;
			*size = 0;
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
	
	char *msg;
    char *cnt;   
	FILE *fp; 
	struct stat st;
	int size, n, msg_sz;
	int errno_copy;

	if(fd_s != -1){ 
	
		if((msg = malloc(MAX_SIZE*sizeof(char))) == NULL){
			perror("malloc");
			errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		//apertura file da scrivere
		//se il file non esiste si esce con errore
		if((fp = fopen(pathname, "rb")) == NULL){ 
			perror("fopen writeFile"); 
            free(msg);
            return -1;
        }			
		
		stat(pathname, &st);
        size = st.st_size;
		//salvataggio contenuto file 
		if((cnt = malloc(size*sizeof(char))) == NULL){
			perror("malloc");
			errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			free(msg);
			fclose(fp); 
			exit(errno_copy);
		}
		
		memset(cnt, 0, size);
		n = fread(cnt, 1, size, fp);
		
        if(!n){
			if (ferror(fp)){  
				errno_copy = errno;
				perror("fread writeFile");
        		free(msg);
				free(cnt);
				fclose(fp); 
				errno = errno_copy;
				return -1;
			}
		}
	    //chiusura file 
		if(fclose(fp)){
			perror("fclose writeFile");
			free(msg);
			free(cnt);
			return -1;
		}
		//invio richiesta al server 
		if(msg_sender(msg, "writef", pathname, size, cnt) == -1){
			perror("Errore in invio messaggio al server");
			free(msg);
			free(cnt);
			return -1;
		}
		free(cnt);
		//recezione file espulsi 	
		n = file_receiver(dirname); 
		
		if(n < 0){
			free(msg);
			return -1;
		}
		//recezione messaggio di esito 
		if(readn(fd_s, &msg_sz, sizeof(int)) == -1){ 
			perror("read socket lato client");
			free(msg);
			return -1;
	    }
		if(readn(fd_s, msg, msg_sz) == -1){ 
			perror("read socket lato client");
			free(msg);
			return -1;
	    }
		//controlla se è stato ricevuto un messaggio d'errore 
		if(strncmp(msg, "Ok", msg_sz) != 0){ 
			#ifdef DEBUG
				fprintf(stderr, "errore writeFile, msg del server: %s\n", msg);
			#endif
			errno = error_handler(msg, msg_sz);
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
	int k, msg_sz;  
	
	if(fd_s != -1){ 
		
		if((msg = malloc(MAX_SIZE*sizeof(char))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		//invio richiesta al server 
		if(msg_sender(msg, "appendTof", pathname, size, buf) == -1){
			perror("Errore in invio messaggio al server");
			free(msg);
			return -1;
		}
		//ricezione file espulsi dal server 
		k = file_receiver(dirname); 

		
		if(k < 0){
			free(msg);
			return -1; 
		}
		//ricezione messaggio di esito 
		if(readn(fd_s, &msg_sz, sizeof(int)) == -1){ 
			perror("read socket lato client");
			free(msg);
			return -1;
	    }
		if(readn(fd_s, msg, msg_sz) == -1){
			perror("read socket lato client");
			free(msg);
			return -1;
	    }
		//controlla se si tratta di un messaggio d'errore 
		if(strncmp(msg, "Ok", msg_sz) != 0){ 
			#ifdef DEBUG
				fprintf(stderr, "errore appendToFile, msg del server: %s\n", msg);
			#endif
			errno = error_handler(msg, msg_sz);
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

int readNFile(int n, const char *dirname){  
 
	int k, msg_sz;
	char *msg;
	
	if(fd_s != -1){ 
		if((msg = malloc(MAX_SIZE*sizeof(char))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		//trasforma numero di file da leggere in stringa 
		char *str;
		if((str = malloc(MAX_SIZE*sizeof(char))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			free(msg);
			exit(errno_copy);
		}
		memset(str, 0, MAX_SIZE); 
		
		int buf_sz = snprintf(NULL, 0, "%d", n);
		snprintf(str, buf_sz + 1, "%d", n);
		
		//invio richiesta al server
		if(msg_sender(msg, "readNf", str, -1, NULL) == -1){ 
			perror("Errore in invio messaggio al server");
			free(msg);
			free(str);
			return -1;
		}
		free(str);
		//ricezione file letti 
		k = file_receiver(dirname);	
		
		if(k < 0){
			free(msg);
			return -1;
		}
		//ricezione messaggio di esito
		if(readn(fd_s, &msg_sz, sizeof(int)) == -1){ 
			perror("read socket lato client");
			free(msg);
			return -1;
	    }
		if(readn(fd_s, msg, msg_sz) == -1){ 
			perror("read socket lato client");
			free(msg);
			return -1;
	    }
		//controlla se è un messaggio d'errore 
		if(strncmp(msg, "Ok", msg_sz) != 0){ 
			#ifdef DEBUG
				fprintf(stderr, "errore readNFile, msg del server: %s\n", msg);
			#endif
			errno = error_handler(msg, msg_sz);
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
	
	//restituisce il numero di file letti
	return k;  
}


/* ------------------- funzioni di supporto -------------------- */

/** Controlla se l'orario specificato nel parametro è successivo a quello attuale
 *   \param orario da confrontare 
 *  
 *   \retval 1 in caso di successo (true)
 *   \retval 0 in caso di fallimento (false)
 */
int isTimeout(struct timespec b){ 
	struct timespec a;
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

/** Trasforma il messaggio ricevuto in un codice d'errore 
 *   \param messaggio da tradurre 
 *   \param taglia del messaggio  
 *  
 *   \retval codice d'errore in caso di successo
 *   \retval -1 in caso di errore
 */
int error_handler(char *msg, int sz){
	if(strncmp(msg, "Err:FileEsistente", sz) == 0){
		return EEXIST;
	}
	else if(strncmp(msg, "Err:CreazioneFile", sz) == 0){
		return EAGAIN;
	}
	else if(strncmp(msg, "Err:RimpiazzamentoFile", sz) == 0){
		return EPERM;
	}
	else if(strncmp(msg, "Err:RimozioneFile", sz) == 0){
		return ENOENT;
	}
	else if(strncmp(msg, "Err:FileTroppoGrande", sz) == 0){
		return EFBIG;
	}
	else if(strncmp(msg, "Err:InserimentoFile", sz) == 0){
		return ENOMEM;
	}	
	else if(strncmp(msg, "Err:AperturaFile", sz) == 0){
		return EAGAIN;
	}
	else if(strncmp(msg, "Err:FileNonEsistente", sz) == 0){
		return ENOENT;
	}
	else if(strncmp(msg, "Err:FileNonAperto", sz) == 0){
		return EBADF;
	}
	else if(strncmp(msg, "Err:FileLocked", sz) == 0){
		return ENOLCK;
	}
	else if(strncmp(msg, "Err:FileNoLocked", sz) == 0){
		return ENOLCK;
	}
	else if(strncmp(msg, "Err:InvalidLastOperation", sz) == 0){
		return EPERM;
	}
	else if(strncmp(msg, "Err:Malloc", sz) == 0){
		return ENOMEM;
	}
	else if(strncmp(msg, "Err:OperazioneNonSupportata", sz) == 0){
		return ENOTSUP;
	}
	else if(strncmp(msg, "Err:PathnameTroppoGrande", sz) == 0){
		return ENAMETOOLONG;
	}
	else if(strncmp(msg, "Err:PushReplQueue", sz) == 0){
		return ENOMEM;
	}
	else if(strncmp(msg, "Err:ParametroErrato", sz) == 0){
		return EINVAL;
	}
	
	return -1;
}

/** Salva il file nella directory specificata 
 *   \param directory dove salvare il file
 *   \param path del file da salvare 
 *   \param contenuto del file
 *   \param taglia del contenuto del file 
 *  
 *   \retval 0 in caso di successo 
 *   \retval -1 in caso di errore (errno settato opportunamente) 
 */
int save_file(const char *dir, char *file, char *buff, int size){

	char *path_file;
	struct stat st = {0};
	char *tmpstr = NULL;
	char *token, *token2;
	
	if((path_file = malloc(UNIX_PATH_MAX*sizeof(char))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		exit(errno_copy);
	}

	if (stat(dir, &st) == -1) { //se la directory non esiste la crea
		if(mkdir(dir, S_IRWXU) == -1){ 
			perror("Creazione directory");
			free(path_file);
			return -1;
		}
	}
	else{
		if(!S_ISDIR(st.st_mode)) { //a meno che non ci sia un file omonimo
			#ifdef DEBUG
				fprintf(stderr, "%s non e' una directory\n", dir);
			#endif
			free(path_file); 
			errno = ENOTDIR;
			return -1;
		} 
	}
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

/** Funzione che si occupa della comunicazione con il server 
 *  nel caso in cui non sia previsto che il server invii file al client
 *  o che il client invii il contenuto di un file al server  
 *
 *   \param tipo di operazione da richiedere al server 
 *   \param path del file di riferimento
 *   \param variabile dove salvare codice d'errore
 *  
 *   \retval 0 in caso di successo 
 *   \retval -1 in caso di errore (errno settato opportunamente) 
 */
int comunic_cs(char *cmd, const char *pathname, int *err){ 
	char *msg;
	int n;

	if((msg = malloc(MAX_SIZE*sizeof(char))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		exit(errno_copy);
	}
	memset(msg, '\0', MAX_SIZE);
	
	//invio richiesta al server nel formato cmd;pathname;sz_cnt 
	//in questo caso si invia sz_cnt = -1 perchè non ci si aspetta di inviare
	//il contenuto del file al server in questo tipo di comunicazione 
	if(msg_sender(msg, cmd, pathname, -1, NULL) == -1){
		*err = errno;  
		#ifdef DEBUG
			fprintf(stderr, "Errore in invio messaggio al server\n");
		#endif
		free(msg);
		return -1;
	}
	//lettura risposta dal server 
	if(readn(fd_s, &n, sizeof(int)) == -1){  
		perror("read socket lato client");
		*err = errno;
		free(msg);
		return -1;
	}
	if(readn(fd_s, msg, n) == -1){ 
		perror("read socket lato client");
		*err = errno;
		free(msg);
		return -1;
	}
	//analisi risposta
    //se contiene messaggio d'errore viene decodificato e salvato nella variabile err
	if(strncmp(msg, "Ok", n) != 0){ 
		#ifdef DEBUG
			fprintf(stderr, "Errore in %s, msg server: %s\n", cmd, msg);
		#endif
		*err = error_handler(msg, n);
		free(msg);
		return -1;
	}
	free(msg);
	return 0;
}

/** Funzione per l'invio di richieste al server. 
 *  Il primo messaggio è nel formato tipo_di_operazione;file_di_riferimento;size_file.
 *  Se la taglia è >= 0 si invia un secondo messaggio con all'interno il contenuto 
 *
 *   \param variabile dove allocare il messaggio 
 *   \param tipo di richiesta da rivolgere al server 
 *   \param pathname del file di riferimento 
 *   \param taglia del contenuto del file (-1 se non significativa)
 *   \param contenuto del file (null se non significativo)
 *  
 *   \retval 0 in caso di successo 
 *   \retval -1 in caso di errore (errno settato opportunamente) 
 *
 */
int msg_sender(char *msg, char *cmd, const char *path, int size, char *cnt){
	char *str;	
	int n;
	
	if(path != NULL){
		//creazione primo messaggio di richiesta 
		strncpy(msg, cmd, MAX_SIZE - 1); 
		strncat(msg, ";", MAX_SIZE - strlen(msg) -1); 

		strncat(msg, path, MAX_SIZE - strlen(msg) -1); 
		strncat(msg, ";", MAX_SIZE - strlen(msg) -1);
		
		if((str = malloc(MAX_SIZE*sizeof(char))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		memset(str, 0, MAX_SIZE);
	
		int bufsz = snprintf(NULL, 0, "%d", size);
		snprintf(str, bufsz+1, "%d", size);
		strncat(msg, str, MAX_SIZE - strlen(msg) -1);

		n = strlen(msg) + 1;
		//invio primo messaggio
		if(writen(fd_s, &n, sizeof(int)) == -1){ //size msg
			perror("Errore nella write");
			free(str);
			return -1;
		}
		if(writen(fd_s, msg, n) == -1){ //cmd;path;sz_cnt
			perror("Write del socket");
			free(str);
			return -1;
		}
		free(str);
		
		if(size > 0){ 
			//invio secondo messaggio 
			if(writen(fd_s, cnt, size) == -1){ //cnt 
				perror("Write del socket");
				return -1;
			}	
		}
	}
	else{
		errno = EINVAL;
		return -1;
	}
	
	return 0;
}

/** Funzione per la ricezione e il salvataggio di file dal server 
 *  Si leggono mano a mano path, taglia_contenuto e, 
 *  se quest'ultima è maggiore di zero, il contenuto del file
 *  Si interrompe in caso di messaggio d'errore "Err:" o 
 *  carattere speciale di terminazione "$"
 *
 *   \param directory dove salvare i file ricevuti
 *  
 *   \retval numero di file letti in caso di successo 
 *   \retval -1 in caso di errore (errno settato opportunamente) 
 *
 */
int file_receiver(const char *dirname){

	int stop = 0;
	char *path, *cnt = NULL;
	int k = 0;
	int sz_msg, found;
	

	while(!stop){
		found = 0;
		//lettura path 
		if(readn(fd_s, &sz_msg, sizeof(int)) == -1){ 
			perror("read socket lato client");
			return -1;
		}
		if((path = malloc(sz_msg*sizeof(char))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		if(readn(fd_s, path, sz_msg) == -1){ //path o $ o Err 
			perror("read socket lato client");
			free(path);
			return -1;
		}
		if(strncmp(path, "Err:", 4) == 0){ 
			#ifdef DEBUG
				fprintf(stderr, "Errore in file_receiver, msg server: %s\n", path);
			#endif
			errno = error_handler(path, sz_msg);
			free(path);
			return -1;
		}
		if(strncmp(path, "$", UNIX_PATH_MAX) != 0){
			k++;
			//lettura cnt_sz
			if(readn(fd_s, &sz_msg, sizeof(int)) == -1){ 
				perror("read socket lato client");
				free(path);
				return -1;
			}
			if(sz_msg > 0){
				found = 1;
				//lettura contenuto 
				if((cnt = malloc(sz_msg*sizeof(char))) == NULL){
					perror("malloc");
					int errno_copy = errno;
					fprintf(stderr,"FATAL ERROR: malloc\n");
					free(path);
					exit(errno_copy);
				}
				if(readn(fd_s, cnt, sz_msg) == -1){ //cnt
					perror("read socket lato client");
					free(path);
					free(cnt);
					return -1;
				}
			}		
			//salvataggio file 
			if(dirname != NULL){
				if(save_file(dirname, path, cnt, sz_msg) == -1){ 
					perror("Errore in savefile");
					free(path);
					if(found)
						free(cnt);
					return -1;
				}
			}
			if(found)
				free(cnt);
		}
		else
			stop = 1; 
		free(path);
	}
	return k;
}
