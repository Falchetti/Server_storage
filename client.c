#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>

#include "api.h"

#define DEBUG
#undef DEBUG

#define UNIX_PATH_MAX 108
#define MAX_SIZE 230

//controlla se hai effettivamente risolto problema contenuto binario
//risolvi definitivamente MAX SIZE - MSG SIZE etc (versione più semplice, poi dopo rifinisci)
//fai controlli su contenuto non troppo grande etc 

/*
ogni volta che leggo o scrivo file applico il protocollo:
apro leggo/scrivo chiudo
*/

/*
ho adottato una politica di scrittura dei file nel server secondo la quale
se il file esiste (omonimo) nel server, il contenuto del file da scrivere
viene scritto in append, se non esiste, il file viene creato e scritto con writeFile
*/  

/*
 Nella mia scelta di progetto conta l'ordine dei flag da linea di comando.
Il flag f (socketname) deve essere specificato prima di richiedere i flag w,W,r,R,l,u,c 
(flag che prevedono richieste al server) e non deve essere ripetuto.
I flag d (e D) devono essere specificati prima delle r/R (e w/W) a cui fanno riferimento.
Allo stesso modo per il flag t, solo le richieste a lui successive avranno quello specifico 
tempo di attesa tra due richieste.
*/


#define N_FLAG 13
#define SZ_STRING 1000  //rendi coerente questo se serve con api e server, dovrebbe essere la taglia del contenuto di un file, che deve passare per il canale di comunicazione, quindi < msg_size + taglia comandi

#define MSEC_TENT 1000  //un tentativo al secondo nella openConnection
#define SEC_TIMEOUT 10 //secondi prima del timeout nella openConnection

#define O_OPEN 0     //visto che alcune define si ripetono tra client e api, metterle tutte in api.h è un errore? In un altro file di include? 
#define O_CREATE 1
#define O_LOCK 2
#define O_CREATE_LOCK 3 

//QUANDO CHIUDO IL CLIENT (vari return -1) ricordati di fare pulizia (free varie) , di disconnettere la comunicazione (e i file aperti??)

void lsR(const char *dir, int *err, int *n, int flag, char *D_removed);
int isdot(const char dir[]);
int write_file(char *file, char *D_removed);

int main(int argc, char *argv[]){
	
	int flag, n;
	//int t = 0;
	
	char *d_read = NULL;
	char *D_removed = NULL;
	int t_requests = 0;
	char *socket_name = NULL;  //const? 
	
	int found_r = 0;
	int found_w = 0;
	int found_h = 0;
	int found_p = 0;
	
	char *tmpstr, *token, *buff; //li devo inizializzare a NULL?? (guarda strtok)
	size_t size;
	
	struct timespec ts = {0};

	while(((flag = getopt(argc, argv, "hf:w:W:D:r:R::d:t:l:u:c:p")) != -1) && !found_h ){ //fai controllo su getopt per la questione R arg opzionale
		
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
					fprintf(stderr, "Attenzione: l'opzione f può essere richiesta una sola volta e necessita di un argomento\n"); 
					//stampo questo ignorando gli f successivi al primo
	
				break;
				
			case 'w' : 
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
					
					struct stat st = {0}; //azzero la struct 

					if (stat(dir, &st) == -1){
						perror("directory non esistente"); //non so se va bene come mess d'errore 
						return -1;
					}
					else{
						if(!S_ISDIR(st.st_mode)){
							fprintf(stderr, "%s non e' una directory\n", dir);
							errno = ENOTDIR;
							return -1;
						} 
					}
					
					int err = 0;
					
					if(token == NULL || n == 0){
						n = 1;
					    lsR(dir, &err, &n, 0, D_removed);
					}
					else
						lsR(dir, &err, &n, 1, D_removed);
					
					if(err != 0){
						fprintf(stderr, "errore ricorsione directory\n");
						errno = -1;  //DA CAMBIARE, forse dovrei far galleggiare gli errno interni, ma è complicato 
						return -1;
					}
					
					//CONTROLLA SULLE SLIDE QUESTA PARTE 
					/*
					char *dir_curr[UNIX_PATH_MAX];
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
					fprintf(stderr, "connessione non ancora aperta, socket non specificato\n");
					errno = ECONNREFUSED; //vedi se va bene questo errno 
					return -1;
				} 
				
				break;
				
			
			case 'W' : 
				found_w = 1;
				
				if(socket_name != NULL){ //mi assicuro la connessione sia aperta 
					 
					token = strtok_r(optarg, ",", &tmpstr); 
					while (token) {
						if(write_file(token, D_removed) == -1){
							fprintf(stderr, "Errore  nella scrittura dei file richiesti\n");
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
				found_w = 0; //ogni volta che inserisco una nuova directory azzero found_w, perchè a lei deve necessariamente SEGUIRE il flag w 
				
				break;
				
			case 'r' :  
			
				found_r = 1;
				
				if(socket_name != NULL){
					token = strtok_r(optarg, ",", &tmpstr);
					
					while (token) {
						if(openFile(token, O_LOCK) == -1){ //Mh, forse se il file non esite, dovrei passare al file successivo senza interrompere? 
							perror("Errore in openFile");
							return -1;                               //attenzione che questi return -1 non saltino i cleanup necessari
						}
		
						buff = malloc(SZ_STRING*sizeof(char));
						size = (size_t) SZ_STRING; //forse non è il corretto utilizzo, il punto è salvarci dentro la vera size 
						
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
							if(save_file(d_read, token, buff, size) == -1){
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
				
			case 'R' :  
				if(optarg != NULL){
					//fprintf(stderr, "%s nbb\n", optarg);
					if(isNumber(optarg, &n) != 0){
						fprintf(stderr,"L'argomento di R deve essere un numero\n");
						return -1;
					}/*
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
				}*/
	            }
				else 
					n = 0;
				if((n = readNFile(n, d_read)) == -1){
					perror("Errore in readNFiles");
					return -1;
				} 
				fprintf(stderr, "Letti %d file\n", n);
				
				found_r = 1;
				
				break;
			
			case 'd' :
				d_read = optarg;
				found_r = 0; //ogni volta che inserisco una nuova directory azzero found_r 
				break;
				
			case 't' : 
				//fprintf(stderr, "----------------- t: %s------------------\n", optarg);
				if(isNumber(optarg, &n) == 0 && n >= 0){
					//t = n;
					ts.tv_sec = n / 1000;
					ts.tv_nsec = (n % 1000) * 1000000;
				}
				else{
					fprintf(stderr, "argomento di -t errato\n");
					//t = 0;
				}
				
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
					 
						if(lockFile(token) == -1){
							perror("Errore in lockFile");
							return -1;
						}
						if(removeFile(token) == -1){
							perror("Errore in removeFile");
							return -1;
						}
						
                        //lo cancello quindi non ha senso l'unlock 
						
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
			
			default:  //'?'
		        printf("flag non riconosciuto [-%c], usare -h per richiedere la lista di flag disponibili\n", (char) flag);
		}
		do {
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

int isdot(const char dir[]) {
  int l = strlen(dir);
  
  if ( (l > 0 && dir[l-1] == '.') ) 
	  return 1;
  return 0;
}

void lsR(const char *dir, int *err, int *n, int flag, char *D_removed) {
	
    // controllo ogni volta che il parametro sia una directory
	if(!(*err) && *n > 0){ //fermo la ricorsione prima che i file finiscano nel caso in cui flag = 1 
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
				strncat(filename,file->d_name, UNIX_PATH_MAX-1); //creo nuovo path assoluto del file 
				
				if (stat(filename, &statbuf) == -1) {
					perror("eseguendo la stat");
					fprintf(stderr, "Errore nel file %s\n", filename);
					*err = 1;
					return;
				}
				if(S_ISDIR(statbuf.st_mode)) {
					if ( !isdot(filename) ) //se è una directoty diversa da quella corrente
						lsR(filename, err, n, flag, D_removed); //ricorsione
				}
				else if(S_ISREG(statbuf.st_mode)){
					if(*n <= 0)
						return;
					if(write_file(filename, D_removed) == -1){
						fprintf(stderr, "scrittura file in -w\n");
						*err = 1;
						return;
					}
					
					if(flag) //decremento solo nel caso in cui non devo leggere tutti i file 
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

int write_file(char *file, char *D_removed){

	FILE *fp;
	char buff[MAX_SIZE];
	int size, n;
	struct stat st;
	
	if(openFile(file, O_LOCK) == -1){ //per scrivere apro il file con la lock 
		if(errno == ENOENT){           //se il file non esiste ne creo uno con attiva la lock 
			if(openFile(file, O_CREATE_LOCK) == -1){
				perror("Errore in openFile");
				return -1;
			}
			if(writeFile(file, D_removed) == -1){ 
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
		
		if((fp = fopen(file, "rb")) == NULL){
			perror("errore fopen");
			return -1;
		}
		
		stat(file, &st);
        size = st.st_size;
		
		memset(buff, 0, MAX_SIZE);
		n = fread(buff, 1, size, fp);
		
        if(!n){
			if (ferror(fp)){    
				perror("fread");
				fclose(fp);
        		return -1;
			}
		}
		
		fclose(fp);
		
		if(appendToFile(file, buff, size, D_removed) == -1){ 
			perror("Errore in writeFile");
			return -1;
		}
		
	}
	
	if (closeFile(file) == -1){
		perror("Errore in closeFile");
		return -1;
	}
	
return 0;
}
