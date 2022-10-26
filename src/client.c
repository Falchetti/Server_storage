#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>

#include <api.h> 
#include <util.h>
#include <defines.h>

#define ERR_SZ 1024  //taglia buffer strerror_r 
#define MSEC_TENT 1000  //un tentativo al secondo nella openConnection
#define SEC_TIMEOUT 10 //secondi prima del timeout nella openConnection


//funzioni di supporto 
char *abs_path(char *r_path);
void lsR(const char *dir, int *err, int *n, int flag_all, char *D_removed, int found_p);
int isdot(const char dir[]);
int write_file(char *file, char *D_removed, int found_p);
void print_p(char *op, char *file[], int n_files, int esito, int rB, int wB);

	
int main(int argc, char *argv[]){
	
	int flag, n, res;
	int errno_copy;
	
	char *d_read = NULL;
	char *D_removed = NULL;
	char *socket_name = NULL;  
	
	int found_r = 0;
	int found_w = 0;
	int found_h = 0;
	int found_p = 0;
	
	struct timespec ts = {0};
	
	char *tmpstr = NULL;
	char *token = NULL; 
	char err_buff[ERR_SZ];
	
	memset(err_buff, 0, ERR_SZ);
		
	while(((flag = getopt(argc, argv, "hf:w:W:D:r:R:d:t:l:u:c:p")) != -1) && !found_h){ 
		
		switch(flag){
			
			case 'h' : //richiesta messaggio d'uso
				
				found_h = 1; 
				if(found_p)
					print_p("Usage message", NULL, 0, 1, -1, -1);
				break;
				
			case 'f' : //richiesta apertura connessione 
				//quando riceve il nome del socket (e solo in quel momento) apre la connessione
				
				if(socket_name == NULL){
					socket_name = optarg;
					if(strlen(socket_name) >= UNIX_PATH_MAX){
						errno = ENAMETOOLONG;
						perror("inserire path socket più breve");
						return -1;
					}
					struct timespec timeout;
					clock_gettime(CLOCK_REALTIME,&timeout);
					timeout.tv_sec += SEC_TIMEOUT;
					
                    // tentativo di connessione, prova per un minuto facendo 1 tentativo al secondo
                    res = openConnection(socket_name, MSEC_TENT, timeout);
					errno_copy = errno;
					
					if(found_p)
						print_p("Specifica nome del socket", &optarg, 1, res, -1, -1);
					if(res == -1){	
						errno = errno_copy;
						perror("Errore in openConnection"); 
						exit(errno); 
					}
				}
				else 
					fprintf(stderr, "Attenzione: l'opzione f può essere richiesta una sola volta e necessita di un argomento\n"); 
	
				break;
				
			case 'w' : //richiesta scrittura file in una directory 
				
				found_w = 1;
					
				if(socket_name != NULL){ //si assicura la connessione sia aperta (se non lo è, termina)
					 
					token = strtok_r(optarg, ",", &tmpstr); 
					char *dir = token;
					token = strtok_r(NULL, ",", &tmpstr); 
					if(token != NULL){
						if(isNumber(token, &n)!= 0){ 
							fprintf(stderr,"Il secondo argomento di w non è un numero\n");
							token = NULL;
						}
					}
					//controlli sul parametro directory (da dove prendere i file da scrivere)
					struct stat st = {0}; 

					if (stat(dir, &st) == -1){
						perror("directory non esistente, impossibile inoltrare richiesta [-w]\n");
						
						if(found_p)
							print_p("Write File", &dir, 1, -1, -1, -1);
						break; //ignora la richiesta e va alla successiva 
					}
					else{
						if(!S_ISDIR(st.st_mode)){
							fprintf(stderr, "%s non e' una directory, impossibile inoltrare richiesta [-w]\n", dir);
							if(found_p)
								print_p("Write File", &dir, 1, -1, -1, -1);
							break; 
						} 
					}
					int err = 0;
					//controlli sul parametro n (numero di file da scrivere)
					//in caso di valori negativi o non corretti si usa n = 0, scrittura di tutti i file 
					if(token == NULL || n <= 0){ 
						n = 1;
					    lsR(dir, &err, &n, 1, D_removed, found_p);
					}
					else
						lsR(dir, &err, &n, 0, D_removed, found_p);
					
					if(err != 0){ 
						if(found_p)
							print_p("Write File", &dir, 1, -1, -1, -1);	
						break; 
					}
				}
				else{
					fprintf(stderr, "connessione non ancora aperta, socket non specificato\n");
					errno = ENOTCONN; 
					return -1;
				} 
				break;
				
			
			case 'W' : //richiesta scrittura file
				found_w = 1;
				
				if(socket_name != NULL){ 
					 
					token = strtok_r(optarg, ",", &tmpstr); 
					while (token) {
						token = abs_path(token);
						if(token != NULL){
							write_file(token, D_removed, found_p); 
							free(token);
						}
						token = strtok_r(NULL, ",", &tmpstr);	
					}
				}
				else{
					fprintf(stderr, "connessione non ancora aperta, socket non specificato\n");
					errno = ENOTCONN; 
					return -1;
				} 
				
				break;
				
			case 'D' : //specifica cartella dove inserire file espulsi 
			
				D_removed = optarg;
				if(found_p)
					print_p("Specifica cartella file espulsi", &optarg, 1, 1, -1, -1);
				
				found_w = 0; //azzera questa variabile, in quanto a -D deve SEGUIRE un'operazione di tipo -w/-W
				break;
				
			case 'r' :  //richiesta di lettura file 
			
				found_r = 1;
				
				if(socket_name != NULL){
					token = strtok_r(optarg, ",", &tmpstr);
					
					while (token) {
						token = abs_path(token);
						if(token != NULL){
							//richiesta di apertura del file da leggere 						
							if(openFile(token, O_LOCK) == -1){ 
								strerror_r(errno, err_buff, ERR_SZ);
								fprintf(stderr, "Errore in openFile [-r], impossibile leggere file %s: %s\n", token, err_buff);
								if(found_p)
									print_p("Read file", &token, 1, -1, -1, -1);
								free(token);
								token = strtok_r(NULL, ",", &tmpstr);
								continue;                           
							}
							char *buff;
							size_t size;
							
							if((buff = malloc(MAX_SIZE*sizeof(char))) == NULL){
								perror("malloc");
								int errno_copy = errno;
								fprintf(stderr,"FATAL ERROR: malloc\n");
								closeFile(token);
								free(token);
								exit(errno_copy);
							} 
							memset(buff, '\0', MAX_SIZE); 
							
							//richiesta lettura file 
							res = readFile(token, (void *) &buff, &size);
							errno_copy = errno;
							
							if(found_p)
								print_p("Read file", &token, 1, res, size, -1);
							
							if(res == -1){
								errno = errno_copy;
								strerror_r(errno, err_buff, ERR_SZ);
								fprintf(stderr, "Errore in readFile [-r], impossibile leggere file %s: %s\n", token, err_buff);
								free(buff);
								closeFile(token);
								free(token);
								token = strtok_r(NULL, ",", &tmpstr);
								continue; 
							}
							char *tmp_token;
							if((tmp_token = malloc((strlen(token)+1)*sizeof(char))) == NULL){
								errno = errno_copy;
								strerror_r(errno, err_buff, ERR_SZ);
								fprintf(stderr, "Errore in readFile [-r], impossibile leggere file %s: %s\n", token, err_buff);
								free(buff);
								closeFile(token);
								free(token);
								token = strtok_r(NULL, ",", &tmpstr);
								continue; 
							}
							memcpy(tmp_token, token, strlen(token) + 1);
							//se la cartella d_read è stata specificata precedentemente con il flag -d i fle letti vengono salvati
							if(d_read != NULL){
								if(save_file(d_read, tmp_token, buff, size) == -1)
									fprintf(stderr, "si è verificato un errore nel salvataggio di: %s [-r]\n", token);
							}
							//richiesta di chiusura del file 
							if (closeFile(token) == -1){
								strerror_r(errno, err_buff, ERR_SZ);
								fprintf(stderr, "Errore in closeFile [-r], impossibile chiudere file %s: %s\n", token, err_buff);
							}
							free(token);
							free(buff); 
						}	
						token = strtok_r(NULL, ",", &tmpstr);				
					}
				}
				
				else{
					fprintf(stderr, "connessione non ancora aperta, socket non specificato\n");
					errno = ENOTCONN; 
					return -1;
				} 
				
				break;
				
			case 'R' :  //richiesta lettura di n file 
				
				found_r = 1;
				int all = 0;
				
				if(socket_name != NULL){
					if(optarg != NULL){
						if(isNumber(optarg, &n) != 0){
							fprintf(stderr,"L'argomento di R deve essere un numero [-R] \n");
							break; 
						}	
					}
					else{ 
						all = 1;
						n = 0;
					}
					//nel caso in cui il parametro sia negativo o zero, vengono letti tutti i file dello storage 
					if(n <= 0)
						all = 1;
					
					//richiesta lettura n file, restituisce file essettivamente letti 
					res = readNFile(n, d_read);
					if(res == -1){
						strerror_r(errno, err_buff, ERR_SZ);
						fprintf(stderr, "Errore in readNFile [-R]: %s\n", err_buff);
					}
					
					if(found_p){
						if(res != -1){
							if(all) 
								fprintf(stderr, "Tipo di operazione: Lettura tutti i file, File effettivamente letti: %d, Esito: SUCCESS\n", res);
							else
								fprintf(stderr, "Tipo di operazione: Lettura %d file, File effettivamente letti: %d, Esito: SUCCESS\n", n, res);
						}
						else {
							if(all)
								fprintf(stderr, "Tipo di operazione: Lettura tutti i file, Esito: FAIL\n"); 
							else 
								fprintf(stderr, "Tipo di operazione: Lettura %d file, Esito: FAIL\n", n);
						}
					}
				}
				else{
					fprintf(stderr, "connessione non ancora aperta, socket non specificato\n");
					errno = ENOTCONN; 
					return -1;
				} 

				break;
			
			case 'd' :  //specifica cartella dove inserire file letti 
				d_read = optarg;
				
				if(found_p)
						print_p("Specifica cartella file letti", &optarg, 1, 0, -1, -1);
				
				found_r = 0; //azzera questa variabile, in quanto a -d deve SEGUIRE un'operazione di tipo -r/-R
				break;
				
			case 't' : //specifica tempo che deve interrompere tra due richieste allo storage (in millisecondi)
				if(isNumber(optarg, &n) == 0 && n >= 0){
					res = 1;
					ts.tv_sec = n / 1000;  
					ts.tv_nsec = (n % 1000) * 1000000;
				}
				else{
					res = -1;
					fprintf(stderr, "argomento di -t errato [-t] \n"); 
				}
				if(found_p)
						print_p("Specifica tempo di attesa tra due richieste", NULL, 0, res, -1, -1);
				
				break;
				
			case 'l' :  //richiesta lock su file
			    if(socket_name != NULL){
					
					token = strtok_r(optarg, ",", &tmpstr);
					while (token) { 
						token = abs_path(token);
						if(token != NULL){
							res = lockFile(token);
							errno_copy = errno;
							if(found_p)
								print_p("Lock file", &token, 1, res, -1, -1);
							if(res == -1){
								errno = errno_copy;
								strerror_r(errno, err_buff, ERR_SZ);
								fprintf(stderr, "Errore in lockFile [-l], impossibile fare lock su file %s: %s\n", token, err_buff);
							}
							free(token);
						}
						token = strtok_r(NULL, ",", &tmpstr);	
					}
				}
				else{ 
					fprintf(stderr, "connessione non ancora aperta, socket non specificato\n");
					errno = ENOTCONN; 
					return -1;
				} 
				
				break;
				
			case 'u' : //richiesta unlock su file
				if(socket_name != NULL){
					
					token = strtok_r(optarg, ",", &tmpstr);
					while (token) { 
						token = abs_path(token);
						if(token != NULL){
							res = unlockFile(token);
							if(found_p)
								print_p("Unlock file", &token, 1, res, -1, -1);
							if(res == -1){
								errno = errno_copy;
								strerror_r(errno, err_buff, ERR_SZ);
								fprintf(stderr, "Errore in unlockFile [-u], impossibile fare unlock su file %s: %s\n", token, err_buff);
							}
							free(token);
						}
						token = strtok_r(NULL, ",", &tmpstr);	
					}
				}
				else{
					fprintf(stderr, "connessione non ancora aperta, socket non specificato\n");
					errno = ENOTCONN; 
					return -1;
				} 
				
				break;
			
			case 'c' : //richiesta rimozione di file
				if(socket_name != NULL){
					
					token = strtok_r(optarg, ",", &tmpstr);
					while (token) { 
						token = abs_path(token);
						if(token != NULL){
							//richiesta lock su file
							if(lockFile(token) == -1){
								strerror_r(errno, err_buff, ERR_SZ);
								fprintf(stderr, "Errore in lockFile [-c], impossibile rimuovere file %s: %s\n", token, err_buff);
								if(found_p)
									print_p("Remove file", &token, 1, -1, -1, -1);
								free(token);
								token = strtok_r(NULL, ",", &tmpstr);
								continue; 
							}
							//richiesta rimozione file
							res = removeFile(token);
							
							if(res == -1){
								strerror_r(errno, err_buff, ERR_SZ);
								fprintf(stderr, "Errore in removeFile [-c], impossibile rimuovere il file %s: %s\n", token, err_buff);
								if(unlockFile(token) == -1)
									fprintf(stderr, "Errore in unlockFile [-c], impossibile chiudere il file %s: %s\n", token, err_buff);	
							}
							if(found_p)
								print_p("Remove file", &token, 1, res, -1, -1);
							
							//se il file viene rimosso correttamente non deve chiaramente richiederne l'unlock
							
							free(token);
						}
						token = strtok_r(NULL, ",", &tmpstr);	
					}
				}
				else{
					fprintf(stderr, "connessione non ancora aperta, socket non specificato\n");
					errno = ENOTCONN; 
					return -1;
				} 
				
				break;
				
			case 'p' : //specifica del flag di stampa 
				
				if(!found_p)
					found_p = 1;
				else
					printf("L'opzione p può essere richiesta una sola volta\n");
				
				break;
			
			default:  
		        printf("flag non riconosciuto [-%c], usare -h per richiedere la lista di flag disponibili\n", (char) flag);
		}
		errno = 0;
		do { //per evitare di non aspettare a sufficienza se arriva un'interruzione 
			n = nanosleep(&ts, &ts);
		} while (n && errno == EINTR);
		
	}
	
	if(!found_r && d_read != NULL)
		printf("il flag d deve essere seguito dal flag r o R\n");
	if(!found_w && D_removed != NULL)
		printf("il flag D deve essere seguito dal flag w o W\n");
	
	if(found_h)
		printf("Le opzioni accettate sono:\n -h, -f filename, -w dirname[,n=0], -W file1[,file2],\n -D dirname, -r file1[,file2], -R [n = 0], -d dirname,\n -t time, -l file1[,file2], -u file1[,file2], -c file1[,file2], -p\n");

    if(closeConnection(socket_name) == -1){
		perror("Errore in closeConnection");
		exit(errno);
	}

	return 0;
	
}


/* ------------------- funzioni di supporto -------------------- */

/** Trasforma un path relativo di un file in uno assoluto
 * 
 *   \param path relativo
 *
 *   \retval NULL in caso di errore 
 *   \retval path assoluto in caso di successo 
 */
char *abs_path(char *r_path){
	char init[UNIX_PATH_MAX];
	char *a_path;
	char *r_path_dir = NULL;
	char *tmp;
	int len;
	
	if((tmp = strrchr(r_path, '/')) == NULL){
		r_path_dir = ".";
	}
	else{
		len = strlen(r_path) - strlen(tmp);
		if(len > 0){
			if((r_path_dir = malloc(len+1*sizeof(char))) == NULL){
				perror("malloc");
				fprintf(stderr,"FATAL ERROR: malloc\n");
				return NULL;
			}
			memcpy(r_path_dir, r_path, len);
			r_path_dir[len] = '\0';
		}
		else if(len == 0)
			r_path_dir = ".";
		else{
			fprintf(stderr, "Errore in abs_path\n");
			return NULL;
		}
	}
	if(getcwd(init, UNIX_PATH_MAX) == NULL){
		perror("getcwd");
		if(r_path_dir != NULL) 
			free(r_path_dir);
		return NULL;
	}
	if(chdir(r_path_dir) == -1){
		perror("chdir");
		if(r_path_dir != NULL) 
			free(r_path_dir);
		return NULL;
	}
	if(r_path_dir != NULL) 
		free(r_path_dir);
	
	if((a_path = malloc(UNIX_PATH_MAX*sizeof(char))) == NULL){
		perror("malloc");
		fprintf(stderr,"FATAL ERROR: malloc\n");
		return NULL;
	}
	if(getcwd(a_path, UNIX_PATH_MAX) == NULL){
		perror("getcwd");
		chdir(init);
		return NULL;
	}
	if(chdir(init) == -1){
		perror("chdir");
		return NULL;
	}
	strncat(a_path, tmp, UNIX_PATH_MAX - strlen(a_path));
	
	return a_path;
}

/** Stampa informazione associate ad un'operazione svolta 
 *
 *   \param nome dell'operazione
 *   \param array di file di riferimento 
 *   \param numero di file nell'array 
 *   \param esito operazione (numero positivo se successo)
 *   \param numero di bytes letti (-1 se nessuno)
 *   \param numero di bytes scritti (-1 se nessuno)
 * 
 */
void print_p(char *op, char *file[], int n_files, int esito, int rB, int wB){
	if(op != NULL){
		printf("\nTipo di operazione: %s, ", op);
		//in realtà a questa funzione verrà passata sempre un unico file (n_files al max 1)
		//è stata lasciata così per mantenerne la generalità 
		if(file != NULL && n_files > 0){ 
			printf("Files di riferimento: ");
			for(int i = 0; i < n_files; i++){
				if(file[i] != NULL)
					printf("%s, ", file[i]);
			}
		}
		if(rB >= 0)
			printf("Bytes letti: %d, ", rB);
		if(wB >= 0)
			printf("Bytes scritti: %d, ", wB);
		if(esito >= 0)
			printf("Esito: SUCCESS\n\n");
		else
			printf("Esito: FAIL\n\n");
			
		fflush(stdout);
	}
	else
		fprintf(stderr, "Errore nell'invio dell'operazione alla funzione di stampa\n");
	
	return;
}

/** Controlla se il parametro è la directory . o .. 
 *  (funzione presentata a lezione)
 *
 *   \param directory
 *
 *   \retval 1 se è la directory . o ..  
 *   \retval 0 se non lo è
 */		
int isdot(const char dir[]) {
  int l = strlen(dir);
  
  if ( (l > 0 && dir[l-1] == '.') ) 
	  return 1;
  return 0;
}

/** Richiede di scrivere tutti i file presenti nella cartella e nelle sue sottocartelle
 *  fermandosi una volta raggiunti n file se il flag_all è 0
 *  (modifica della funzione presentata a lezione)
 *
 *   \param directory
 *   \param variabile dove salvare codice d'errore  
 *   \param numero di file da scrivere (se 0 o maggiore del numero di files presenti nella directory, tutti)
 *   \param flag che indica se scriverne n o tutti 
 *   \param directory dove inserire i file espulsi dal server
 *   \param flag che indica se bisogna o meno stampare i dettagli dell'operazione
 *
 */	
void lsR(const char *dir, int *err, int *n, int flag_all, char *D_removed, int found_p) {
	
	if(!(*err) && *n > 0){ 
		
		struct stat st = {0};
		
		// controlla ogni volta che il parametro sia una directory
		if (stat(dir, &st) == -1){
			perror("directory non esistente");
			*err = 1;
			return;
		}
		else{
			if(!S_ISDIR(st.st_mode)){
				fprintf(stderr, "%s non e' una directory\n", dir);
				*err = 1;
				return;
			} 
		}

		DIR * dp;
		
		if ((dp = opendir(dir)) == NULL) {
			perror("opendir");
			fprintf(stderr, "Errore aprendo la directory %s\n", dir);
			*err = 1;
			return;
		}
		else {
			struct dirent *file;

			while((errno = 0, file = readdir(dp)) != NULL) {
				struct stat statbuf;
				
				char filename[UNIX_PATH_MAX]; 
				
				int len1 = strlen(dir);
				int len2 = strlen(file->d_name);
				
				if ((len1 + len2 + 2) > UNIX_PATH_MAX) {
					fprintf(stderr, "ERRORE: UNIX_PATH_MAX troppo piccolo\n");
					*err = 1;
					return;
				}	    
				strncpy(filename,dir, UNIX_PATH_MAX-1);
				strncat(filename,"/", UNIX_PATH_MAX-1);
				strncat(filename,file->d_name, UNIX_PATH_MAX-1); //crea nuovo path del file 
				
				if (stat(filename, &statbuf) == -1) {
					perror("eseguendo la stat");
					fprintf(stderr, "Errore nel file %s\n", filename);
					*err = 1;
					return;
				}
				if(S_ISDIR(statbuf.st_mode)) {
					if ( !isdot(filename) ) //se è una directoty diversa da quella corrente
						lsR(filename, err, n, flag_all, D_removed, found_p); //ricorsione
				}
				else if(S_ISREG(statbuf.st_mode)){
					if(*n <= 0)
						return;
					char *tmp = abs_path(filename);
					if(tmp != NULL){
						if(write_file(tmp, D_removed, found_p) == -1){
							fprintf(stderr, "scrittura file in -w\n");
							*err = 1;
							free(tmp);
							return;
						}
						free(tmp);
					}
					if(!flag_all) //decrementa solo nel caso in cui non si devono leggere tutti i file 
						*n = *n - 1;
				}
		    }
			if (errno != 0) 
				perror("readdir");
			closedir(dp);
		}
	}
	return;
}

/** Richiede la scrittura del file passato come parametro.
 *  Se il file esiste nello storage lo apre con lock e scrive tramite writeFile.
 *  Se il file non esiste lo crea e apre con lock e scrive tramite appendToFile.
 *  Infine richiede la chiusura del file aperto e salva i potenziali file espulsi dal server.
 *
 *   \param file da scrivere 
 *   \param directory dove salvare i file espulsi dal server
 *   \param flag che indica se bisogna o meno stampare i dettagli dell'operazione
 *
 *   \retval 0 in caso di successo
 *   \retval -1 in caso di errore (setta errno opportunamente)
 *
 */	
int write_file(char *file, char *D_removed, int found_p){

	FILE *fp;
	char *buff;
	int size, n, res;
	struct stat st;
	int errno_copy;
	char err_buff[ERR_SZ];

	errno = 0;
	
	if(openFile(file, O_LOCK) == -1){   //per scrivere tenta di aprire il file con la lock 
		if(errno == ENOENT){           //se il file non esiste ne crea uno con attiva la lock e richiede la scrittura
			if(openFile(file, O_CREATE_LOCK) == -1){
				strerror_r(errno, err_buff, ERR_SZ);
				fprintf(stderr, "Errore in openFileCL [-w/W], impossibile scrivere file %s: %s\n", file, err_buff);
		        if(found_p)
					print_p("Write File", &file, 1, -1, -1, -1);
				return -1;
			}
			
			res = writeFile(file, D_removed);
			errno_copy = errno;
			
			if(found_p){ 
				if((fp = fopen(file, "rb")) == NULL){
					strerror_r(errno, err_buff, ERR_SZ);
					fprintf(stderr, "Errore in fopen [-w/W], impossibile aprire file %s: %s\n", file, err_buff);
					print_p("Write File", &file, 1, -1, -1, -1);
					closeFile(file);
					return -1;
				}	
				stat(file, &st);
				print_p("Write File", &file, 1, res, -1, st.st_size);
				if(fclose(fp) == -1){
					perror("fclose write_file");
					closeFile(file);
					return -1;
				}
			}
			errno = errno_copy;
			if(res == -1){ 
				strerror_r(errno, err_buff, ERR_SZ);
				fprintf(stderr, "Errore in writeFile [-w/W], impossibile scrivere file %s: %s\n", file, err_buff);
				closeFile(file);
				return -1;
			}	
		}
		else {
			strerror_r(errno, err_buff, ERR_SZ);
			fprintf(stderr, "Errore in openFileL [-w/W], impossibile aprire file %s: %s\n", file, err_buff);
			if(found_p)
				print_p("Write File", &file, 1, -1, -1, -1);
			return -1;                      
		}
	}
	else {   //se esite richiede la scrittura in append 
		
		if((fp = fopen(file, "rb")) == NULL){
			strerror_r(errno, err_buff, ERR_SZ);
			fprintf(stderr, "Errore in fopen [-w/W], impossibile aprire file %s: %s\n", file, err_buff);
			if(found_p)
				print_p("Append to File", &file, 1, -1, -1, -1);
			closeFile(file);
			return -1;
		}
		
		stat(file, &st);
        size = st.st_size;
		
		if((buff = malloc(size*sizeof(char))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		
		memset(buff, 0, size);
		n = fread(buff, 1, size, fp); //inserisce in buff il contenuto del file 
		
        if(!n){
			if (ferror(fp)){    
				strerror_r(errno, err_buff, ERR_SZ);
				fprintf(stderr, "Errore in fread [-w/W], impossibile leggere file %s: %s\n", file, err_buff);
				fclose(fp);
				if(found_p)
					print_p("Write File", &file, 1, -1, -1, -1);
				closeFile(file);
				free(buff);
        		return -1;
			}
		}
		if(fclose(fp) == -1){
			strerror_r(errno, err_buff, ERR_SZ);
			fprintf(stderr, "Errore in fclose [-w/W], impossibile chiudere file %s: %s\n", file, err_buff);
			closeFile(file);
			free(buff);
			return -1;
		}
		
		res = appendToFile(file, buff, size, D_removed);
		errno_copy = errno;
		
		free(buff);
		
		if(found_p)
			print_p("Append To File", &file, 1, res, -1, size);
		if(res == -1){
			errno = errno_copy;
			strerror_r(errno, err_buff, ERR_SZ);
			fprintf(stderr, "Errore in appendToFile [-w/W], impossibile scrivere file %s: %s\n", file, err_buff);
			closeFile(file);
			return -1;
		}
	}
	if(closeFile(file) == -1){
		strerror_r(errno, err_buff, ERR_SZ);
		fprintf(stderr, "Errore in closeFile [-w/W], impossibile chiudere file %s: %s\n", file, err_buff);
    }
	
	return 0;
}
