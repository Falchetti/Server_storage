#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>


#include "api.h"

#define UNIX_PATH_MAX 108
#define MAX_SIZE 230

#define DEBUG
#undef DEBUG

/*Nella mia scelta di progetto conta l'ordine dei flag.
Il flag f (socketname) deve essere specificato prima di richiedere i flag w,W,r,R,l,u,c 
(flag che prevedono richieste al server) e non deve essere ripetuto
I flag d (e D) devono essere specificati prima delle r/R (e w/W) a cui fanno riferimento
idem per il flag t, solo le richieste a lui successive avranno quello specifico tempo di attesa tra due richieste
*/


#define N_FLAG 13
#define SZ_STRING 1000  //rendi coerente questo se serve con api e server, dovrebbe essere la taglia del contenuto di un file, che deve passare per il canale di comunicazione, quindi < msg_size + taglia comandi

#define MSEC_TENT 1000  //un tentativo al secondo nella openConnection
#define SEC_TIMEOUT 10 //secondi prima del timeout nella openConnection

#define O_OPEN 0
#define O_CREATE 1
#define O_LOCK 2
#define O_CREATE_LOCK 3 

//QUANDO CHIUDO IL CLIENT (vari return -1) ricordati di fare pulizia, di disconnettere la comunicazione (e i file aperti??)

int isNumber(void *, int *n);
//int save_file(const char *dir, char *file, char *buff);
void lsR(const char *dir, int *err, int *n, int flag);
int isdot(const char dir[]);
int write_file(char *file);

int main(int argc, char *argv[]){
	
	int flag, n;
	char *d_read = NULL;
	char *D_removed = NULL;
	int t_requests = 0;
	char *socket_name = NULL; 
	//char *files = NULL; //potrei usare piuttosto il nome arg e metterla ovunque, anche su w e R
	int found_r = 0;
	int found_w = 0;
	int found_h = 0;
	int found_p = 0;
	char *tmpstr, *token, *buff; //li devo inizializzare a NULL?? (guarda strtok)
	size_t size;
	FILE *fp;
	char *aux;

	
	while(((flag = getopt(argc, argv, "hf:w:W:D:r:R::d:t:l:u:c:p")) != -1) && !found_h ){
		
		#ifdef DEBUG2
			printf("t_requests = %d, found_r = %d, found_w = %d, found_p = %d\n", t_requests, found_r, found_w, found_p);
			if( d_read != NULL)   
				printf("d_read = %s, ", d_read);
			if( D_removed != NULL)   
				printf("D_removed = %s, ", D_removed);
			if( files != NULL)   
				printf("files = %s, ", files);
			if( socket_name != NULL)   
				printf("socket_name = %s ", socket_name); 
			printf("\n\n");
		#endif
				
		switch(flag){
			
			case 'h' :
				
				found_h = 1; //assicurati che così termini subito, guarda se anche in fondo la gestione (cleanup) va bene
				break;
				
			case 'f' : //quando ricevo il nome del socket (e solo in quel momento) apro la connessione 
				
				if(socket_name == NULL){
					socket_name = optarg;
					
					struct timespec timeout;
					clock_gettime(CLOCK_REALTIME,&timeout);
					timeout.tv_sec += SEC_TIMEOUT;
					
                    // tentativo di connessione, provo per un minuto facendo 1 tentativo al secondo
                    // questa scelta la metto direttamente nelle #define o devo usare un file di config?? 					
					if(openConnection(socket_name, MSEC_TENT, timeout) == -1){
						perror("Errore in openConnection"); 
						exit(errno); //vedi se va bene l'uso di exit
					}
				}
				else //per me il client mantiene la connessione aperta sempre finchè è attivo 
					printf("Attenzione: l'opzione f può essere richiesta una sola volta e necessita di un argomento\n"); 
					//stampo questo e ignoro gli f successivi al primo
	
				break;
				
			case 'w' : 
				//dir_etc = optarg;
				//ricordati della parte n 
				found_w = 1;
					
				if(socket_name != NULL){ //mi assicuro la connessione sia aperta 
					 
					token = strtok_r(optarg, ",", &tmpstr); 
					char *dir = token;
					token = strtok_r(NULL, ",", &tmpstr); 
					if(token != NULL){
						if(isNumber(token, &n)!= 0){ 
							fprintf(stderr,"Il secondo argomento di w non è un numero\n");
							token = NULL;
						}
					}
					
					struct stat st = {0};

					if (stat(dir, &st) == -1){
						perror("directory non esistente");
						return -1;
					}
					else{
						if(!S_ISDIR(st.st_mode)){
							fprintf(stderr, "%s non e' una directory\n", dir);
							return -1;
						} 
					}
					
					int err = 0;
					fprintf(stderr, "directory: %s\n", dir);
					if(token == NULL || n == 0){
						n = 1;
					    lsR(dir, &err, &n, 0);
					}
					else
						lsR(dir, &err, &n, 1);
					
					
					
					if(err != 0){
						fprintf(stderr, "errore ricorsione directory\n");
						return -1;
					}
					/*char *dir_curr[UNIX_PATH_MAX];
					if(getcwd(dir_curr, UNIX_PATH_MAX) == NULL){
						perror("getcwd");
						return -1;
					}
					if(chdir(dir) == -1){
						perror("chdir");
						return -1;
					}	
					}
					if(chdir(dir_curr == -1){
						perror("chdir");
						return -1;
					}*/				
					
				}
				
				else{
					fprintf(stderr, "connessione non acora aperta, socket non specificato\n");
					return -1;
				} 
				
				break;
				
			
			case 'W' : 
				//files = optarg;
				found_w = 1;
				
				if(socket_name != NULL){ //mi assicuro la connessione sia aperta 
					 
					token = strtok_r(optarg, ",", &tmpstr); 
					while (token) {
						
						if(openFile(token, O_LOCK) == -1){ //per scrivere apro il file con la lock 
							if(errno == ENOENT){           //se il file non esiste ne creo uno con attiva la lock 
								if(openFile(token, O_CREATE_LOCK) == -1){
									perror("Errore in openFile");
							        return -1;
								}
								
								if(writeFile(token, NULL) == -1){ //probabilmente con questo flag dovrei usare l'appendToFile non la writeFile (per via delle specifiche della openFile)
									perror("Errore in writeFile");
									return -1;
								}
									
							}
							else {
								perror("Errore in openFile");
							    return -1;                      //attenzione che questi return -1 non saltino i cleanup necessari
						    }
						}
						else {   //se esite scrivo in append 
							buff = malloc(SZ_STRING*sizeof(char));
							
							if((fp = fopen(token, "r")) == NULL){
								perror("errore fopen");
								return -1;
							}
							errno = 0;
							aux = buff;
							
							while (!feof(fp)){
								fgets(aux, SZ_STRING, fp);;
								aux = aux + strlen(aux);
							}
							
							if(errno != 0){//va bene questo controllo? va bene come ho aggiornato il file?
								perror("fscanf fallita\n");
								return -1;
							}
			
							fclose(fp);
							
							if(appendToFile(token, buff, strlen(buff), NULL) == -1){ //probabilmente con questo flag dovrei usare l'appendToFile non la writeFile (per via delle specifiche della openFile)
								perror("Errore in writeFile");
								return -1;
							}
							
							free(buff); //NB: così se c'è un errore (return -1) buff non viene deallocato
						}
						
						if (closeFile(token) == -1){
							perror("Errore in closeFile");
							return -1;
						}
						
						token = strtok_r(NULL, ",", &tmpstr);
						
					}
				}
				
				else{
					fprintf(stderr, "connessione non acora aperta, socket non specificato\n");
					return -1;
				} 
				
				break;
				
			case 'D' :
			
				D_removed = optarg;
				found_w = 0; //ogni volta che inserisco una nuova directory azzero found_w, perchè a lei deve necessariamente seguire il flag w 
				
				break;
				
			case 'r' :  
			
				found_r = 1;
				
				if(socket_name != NULL){
					token = strtok_r(optarg, ",", &tmpstr);
					
					while (token) {
						if(openFile(token, O_LOCK) == -1){
							perror("Errore in openFile");
							return -1;                               //attenzione che questi return -1 non saltino i cleanup necessari
						}
		
						buff = malloc(SZ_STRING*sizeof(char));
						size = (size_t) SZ_STRING;
						
						if(readFile(token,  (void *) &buff, &size) == -1){
							perror("Errore in readFile");
							return -1;
						} 
						
						#ifdef DEBUG
							if(buff[0] == '\0')
								fprintf(stderr, "file letto: VUOTO\n");
							else
								fprintf(stderr, "FILE LETTO: %s\n", (char *) buff);
						#endif
						
     					if(d_read != NULL)
							if(save_file(d_read, token, buff) == -1){
								//serve una stampa?
								return -1;
							}
						
						if (closeFile(token) == -1){
							perror("Errore in closeFile");
							return -1;
						}
						
						token = strtok_r(NULL, ",", &tmpstr);
						free(buff); //ha senso dentro al while?
						
						
					}
				}
				
				else{
					fprintf(stderr, "connessione non acora aperta, socket non specificato\n");
					return -1;
				} 
				
				break;
				
			case 'R' :  //per ora senza dirname, ATTUALMENTE NON FUNZIONA L'ARGOMENTO OPZIONALE
				if(optarg != NULL){
					fprintf(stderr, "%s nbb\n", optarg);
					if(isNumber(optarg, &n) != 0){
						fprintf(stderr,"L'argomento di R deve essere un numero\n");
						return -1;
					}
					else if(n > 0){
						if(readNFile(n, d_read) == -1){
							perror("Errore in readFile");
							return -1;
						} 
					}
					else if(n == 0) { 
						if(readNFile(0, d_read) == -1){
							perror("Errore in readFile");
							return -1;
						} 
					}
					else{
						perror("l'argomento di R deve essere un numero positivo");
						return -1;
					}
				}
				else{
					if(readNFile(2, d_read) == -1){ //METTI 0 POI
						perror("Errore in readFile");
						return -1;
					} 
				}
				
				found_r = 1;
				
				break;
			
			case 'd' :
				d_read = optarg;
				found_r = 0; //ogni volta che inserisco una nuova directory azzero found_r 
				break;
				
			case 't' :
				
				break;
				
			case 'l' :  
			    if(socket_name != NULL){
					
					token = strtok_r(optarg, ",", &tmpstr);
					while (token) { //ho scelto che per fare la lock non devo aprire il file 
						
						if(lockFile(token) == -1){
							perror("Errore in lockFile");
							return -1;
						}
					
						token = strtok_r(NULL, ",", &tmpstr);	
					}
				}
				else{ 
					fprintf(stderr, "connessione non acora aperta, socket non specificato\n");
					return -1;
				} 
				
				break;
				
			case 'u' : 
				if(socket_name != NULL){
					
					token = strtok_r(optarg, ",", &tmpstr);
					while (token) { //ho scelto che per fare la lock non devo aprire il file 
					
						if(unlockFile(token) == -1){
							perror("Errore in unlockFile");
							return -1;
						}
						
						token = strtok_r(NULL, ",", &tmpstr);	
					}
				}
				
				else{
					fprintf(stderr, "connessione non acora aperta, socket non specificato\n");
					return -1;
				} 
				
				break;
			
			case 'c' : 
				if(socket_name != NULL){
					
					token = strtok_r(optarg, ",", &tmpstr);
					while (token) { //ho scelto che per fare la lock non devo aprire il file 
					
						if(removeFile(token) == -1){
							perror("Errore in unlockFile");
							return -1;
						}
						
						token = strtok_r(NULL, ",", &tmpstr);	
					}
				}
				
				else{
					fprintf(stderr, "connessione non acora aperta, socket non specificato\n");
					return -1;
				} 
				
				break;
				
			case 'p' :
				
				if(!found_p){
					found_p = 1;
				}
				
				else
					printf("L'opzione p può essere richiesta una sola volta\n");
				
				break;
				
			case(-1):
				break;
			
			default:  //'?'
		        printf("flag non riconosciuto [-%c], usare -h per richiedere la lista di flag disponibili\n", (char) flag);
		}
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

int isNumber(void *el, int *n){
	char *e = NULL;
	errno = 0;
	*n = strtol(el, &e, 10);
	if(errno == ERANGE) return 2;
	if(e != NULL && *e == (char) 0)
		return 0; 
	return 1;
}
/*
int save_file(const char *dir, char *file, char *buff){
	
	char *path_file = malloc(UNIX_PATH_MAX*sizeof(char));
	struct stat st = {0};

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
}*/

int isdot(const char dir[]) {
  int l = strlen(dir);
  
  if ( (l>0 && dir[l-1] == '.') ) return 1;
  return 0;
}

void lsR(const char *dir, int *err, int *n, int flag) {
    // controllo che il parametro sia una directory

		
	if(!(*err) && *n > 0){
		struct stat st = {0};
		
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
		fprintf(stdout, "-----------------------\n");
		fprintf(stdout, "Directory %s:\n", dir);
		
		if ((dp = opendir(dir)) == NULL) {
			perror("opendir");
			fprintf(stderr, "Errore aprendo la directory %s\n", dir);
			*err = 1;
			return;
		}
		else {
		struct dirent *file;
		
		while((errno=0, file = readdir(dp)) != NULL) {
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
			strncat(filename,file->d_name, UNIX_PATH_MAX-1);
			
			if (stat(filename, &statbuf) == -1) {
				perror("eseguendo la stat");
				fprintf(stderr, "Errore nel file %s\n", filename);
				*err = 1;
				return;
			}
			if(S_ISDIR(statbuf.st_mode)) {
				if ( !isdot(filename) ) 
					lsR(filename, err, n, flag);
			}
			else if(S_ISREG(statbuf.st_mode)){
				if(*n <= 0)
					return;
				if(write_file(filename) == -1){
					fprintf(stderr, "scrittura file in -w\n");
					*err = 1;
					return;
				}
				fprintf(stderr, "proviamo %d\n", *n);
				if(flag)
					*n = *n - 1;
			}
		}
		if (errno != 0) perror("readdir");
		closedir(dp);
		fprintf(stdout, "-----------------------\n");
		}
	}
	return;
}

int write_file(char *file){
	char *buff, *aux;
	FILE *fp;
	//***********COPIATO DA -W
	if(openFile(file, O_LOCK) == -1){ //per scrivere apro il file con la lock 
		if(errno == ENOENT){           //se il file non esiste ne creo uno con attiva la lock 
			if(openFile(file, O_CREATE_LOCK) == -1){
				perror("Errore in openFile");
				return -1;
			}
			if(writeFile(file, NULL) == -1){ //probabilmente con questo flag dovrei usare l'appendToFile non la writeFile (per via delle specifiche della openFile)
				perror("Errore in writeFile");
				return -1;
			}
				
		}
		else {
			perror("Errore in openFile");
			return -1;                      //attenzione che questi return -1 non saltino i cleanup necessari
		}
	}
	else {   //se esite scrivo in append 
		buff = malloc(SZ_STRING*sizeof(char));
		
		if((fp = fopen(file, "r")) == NULL){
			perror("errore fopen");
			return -1;
		}
		errno = 0;
		aux = buff;
		
		while (!feof(fp)){
			fgets(aux, SZ_STRING, fp);;
			aux = aux + strlen(aux);
		}
		
		if(errno != 0){//va bene questo controllo? va bene come ho aggiornato il file?
			perror("fscanf fallita\n");
			return -1;
		}

		fclose(fp);
		
		if(appendToFile(file, buff, strlen(buff), NULL) == -1){ //probabilmente con questo flag dovrei usare l'appendToFile non la writeFile (per via delle specifiche della openFile)
			perror("Errore in writeFile");
			return -1;
		}
		
		free(buff); //NB: così se c'è un errore (return -1) buff non viene deallocato
	}
	
	if (closeFile(file) == -1){
		perror("Errore in closeFile");
		return -1;
	}
	//***** FINE copia
return 0;
}
