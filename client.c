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

/*
ogni volta che leggo o scrivo file applico il protocollo:
apro leggo/scrivo chiudo
*/

/*
ho adottato una politica di scrittura dei file nel server secondo la quale
se il file esiste (omonimo) nel server, il contenuto del file da scrivere
viene scritto in append (appendToFile), 
se non esiste, il file viene creato e scritto con writeFile
*/  

/*
 Nella mia scelta di progetto conta l'ordine dei flag da linea di comando.
Il flag f (socketname) deve essere specificato prima di richiedere i flag w,W,r,R,l,u,c 
(flag che prevedono richieste al server) e non deve essere ripetuto.
I flag d (e D) devono essere specificati PRIMA delle r/R (e w/W) a cui fanno riferimento.
Allo stesso modo per il flag t, solo le richieste a lui successive avranno quello specifico 
tempo di attesa tra due richieste.
*/

//attualmente chiudo il client in maniera brusca solo nei casi in cui 
//open/close connection o malloc
//non vanno a buon fine


#define N_FLAG 13
#define UNIX_PATH_MAX 108
#define MAX_SIZE 10000 //taglia massima contenuto file (usata su read)


#define MSEC_TENT 1000  //un tentativo al secondo nella openConnection
#define SEC_TIMEOUT 10 //secondi prima del timeout nella openConnection

#define O_OPEN 0     //visto che alcune define si ripetono tra client e api, metterle tutte in api.h è un errore? In un altro file di include? 
#define O_CREATE 1
#define O_LOCK 2
#define O_CREATE_LOCK 3 


//e i file aperti??
//ai file aperti forse ci dovrebbe pensare il server a togliere l'identificatore? 
//Ma così dovrebbe scansionare tutti i suoi file ogni volta che si chiude una connessione per togliere quell'identificatore
//tuttavia se li lascio una nuova connessione potrebbe riprendere quell'identificatore e generare errori 
//(ha dei file aperti che in realtà sono di vecchie connessioni) 
//Soluzione: mi assicuro che, a meno di errori, ogni volta che apro un file, lo chiudo! 
//[controllato]
//il problema però rimane per i lock owner..


//ATTENZIONE, che succede ai client che erano bloccati in attesa di una lock di un file quando un client ELIMINA quel file???
//vedere gestione server!!! 


void lsR(const char *dir, int *err, int *n, int flag, char *D_removed, int found_p);
int isdot(const char dir[]);
int write_file(char *file, char *D_removed, int found_p);
void print_p(char *op, char *file[], int n_files, int esito, int rB, int wB);
//in realtà non lo userò mai come vettore, ma ne prenderò sempre uno alla volta (lo lascio così ormai perchè più generale)

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
	
	char *tmpstr = NULL;
	char *token = NULL; 
	
	struct timespec ts = {0};
	
	//fai controllo su getopt per la questione R arg opzionale
	while(((flag = getopt(argc, argv, "hf:w:W:D:r:R:d:t:l:u:c:pb:x:")) != -1) && !found_h){ 
		
		switch(flag){
			
			case 'x' :
					openFile(optarg, O_CREATE_LOCK);
					break;
			
			case 'h' :
				
				found_h = 1; 
				if(found_p)
					print_p("Usage message", NULL, 0, 1, -1, -1);
				break;
				
			case 'f' : //quando ricevo il nome del socket (e solo in quel momento) apro la connessione 
				
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
					
                    // tentativo di connessione, provo per un minuto facendo 1 tentativo al secondo
                    // questa scelta la metto direttamente nelle #define o devo usare un file di config?? 					
					res = openConnection(socket_name, MSEC_TENT, timeout);
					
					if(found_p)
						print_p("Specifica nome del socket", &optarg, 1, res, -1, -1);
					if(res == -1){	
						perror("Errore in openConnection"); 
						exit(errno); //vedi se va bene l'uso di exit
					}
				}
				else // per me il client mantiene la connessione aperta sempre finchè è attivo 
					fprintf(stderr, "Attenzione: l'opzione f può essere richiesta una sola volta e necessita di un argomento\n"); 
					// ignoro f successivi al primo
	
				break;
				
			case 'b' : 
				if(socket_name != NULL){
					char *buff;
					if((buff = malloc(MAX_SIZE*sizeof(char))) == NULL){
							perror("malloc");
							int errno_copy = errno;
							fprintf(stderr,"FATAL ERROR: malloc\n");
							exit(errno_copy);
						} 
					memset(buff, '\0', MAX_SIZE); 
			
					size_t sz;
					if(openFile(optarg, 3) == -1)
						perror("prova openFile");
					if(writeFile(optarg, d_read) == -1)
						perror("prova writeFile");
					if(readFile(optarg, (void **) &buff, &sz) == -1)
						perror("prova readFile");
					if(sz > 0)
						fprintf(stderr, "buff è:%s, sz è: %ld\n", buff, sz);
					if(openFile(optarg, 1) == -1)
						perror("prova openFile");
					if(openFile(optarg, 0) == -1)
						perror("prova openFile");
					if(openFile(optarg, 2) == -1)
						perror("prova openFile");
					if(lockFile(optarg) == -1)
						perror("prova lockFile");
					if(unlockFile(optarg) == -1)
						perror("prova unlockFile");
					if(closeFile(optarg) == -1)
						perror("prova closeFile");
					if(lockFile(optarg) == -1)
						perror("prova lockFile");
				}
				else
					fprintf(stderr, "no socket\n");
				break;
				
			case 'w' : 
				
				found_w = 1;
					
				if(socket_name != NULL){ //mi assicuro la connessione sia aperta (se non lo è termino client)
					 
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
						perror("directory non esistente, impossibile inoltrare richiesta [-w]\n");
						
						if(found_p){
							print_p("Write File", &dir, 1, -1, -1, -1);
						}
						break; //ignoro la richiesta e vado alla successiva 
					}
					else{
						if(!S_ISDIR(st.st_mode)){
							fprintf(stderr, "%s non e' una directory, impossibile inoltrare richiesta [-w]\n", dir);
							if(found_p){
								print_p("Write File", &dir, 1, -1, -1, -1);
							}
							//errno = ENOTDIR; penso sia inutile, tanto verrà sovrascritto, per esempio da closeConnection alla fine
							break; //ignoro la richiesta e vado alla successiva 
						} 
					}
					
					int err = 0;
					
					if(token == NULL || n <= 0){ //valori negativi o non corretti li considero n = 0
						n = 1;
					    lsR(dir, &err, &n, 0, D_removed, found_p);
					}
					else
						lsR(dir, &err, &n, 1, D_removed, found_p);
					
					if(err != 0){ 
						fprintf(stderr, "errore ricorsione directory, errore nell'inoltrare richiesta [-w]\n");
						if(found_p)
							print_p("Write File", &dir, 1, -1, -1, -1);	
						break; //ignoro la richiesta e vado alla successiva
					}

					//non devo cambiare directory di lavoro per come ho scritto lsR
					//però in generale importante che i path passati da riga di comando siano ASSOLUTI
					
				}
				
				else{
					fprintf(stderr, "connessione non ancora aperta, socket non specificato\n");
					errno = ENOTCONN; 
					return -1;
				} 
				
				break;
				
			
			case 'W' : 
				found_w = 1;
				
				if(socket_name != NULL){ 
					 
					token = strtok_r(optarg, ",", &tmpstr); 
					while (token) {
						if(write_file(token, D_removed, found_p) == -1)
							fprintf(stderr, "Errore nella scrittura del file richiesto: %s [-W]\n", token); //ignoro la richiesta e vado alla successiva
						token = strtok_r(NULL, ",", &tmpstr);	
					}
				}
				else{
					fprintf(stderr, "connessione non ancora aperta, socket non specificato\n");
					errno = ENOTCONN; 
					return -1;
				} 
				
				break;
				
			case 'D' :
			
				D_removed = optarg;
				if(found_p)
					print_p("Specifica cartella file espulsi", &optarg, 1, 1, -1, -1);
				
				found_w = 0; //ogni volta che inserisco una nuova directory azzero found_w, perchè a lei deve necessariamente SEGUIRE il flag w 
				
				break;
				
			case 'r' :  
			
				found_r = 1;
				
				if(socket_name != NULL){
					token = strtok_r(optarg, ",", &tmpstr);
					
					while (token) {
						if(openFile(token, O_LOCK) == -1){ 
							perror("Errore in openFile [-r]");
							fprintf(stderr, "impossibile aprire file: %s\n", token);
							if(found_p)
								print_p("Read file", &token, 1, -1, -1, -1);
							token = strtok_r(NULL, ",", &tmpstr);
							continue;   //ignoro la richiesta e vado alla successiva                         
						}
						char *buff;
						size_t size;
						
						if((buff = malloc(MAX_SIZE*sizeof(char))) == NULL){
							perror("malloc");
							int errno_copy = errno;
							fprintf(stderr,"FATAL ERROR: malloc\n");
							closeFile(token);
							exit(errno_copy);
						} 
						memset(buff, '\0', MAX_SIZE); 
						
						res = readFile(token,  (void *) &buff, &size);
						errno_copy = errno;
						
						if(found_p)
							print_p("Read file", &token, 1, res, size, -1);
						
						if(res == -1){
							errno = errno_copy;
							perror("Errore in readFile [-r]");
							fprintf(stderr, "impossibile leggere file: %s\n", token);
							free(buff);
							closeFile(token);
							token = strtok_r(NULL, ",", &tmpstr);
							continue; //ignoro la richiesta e vado alla successiva
						} 
						
						#ifdef DEBUG
							if(buff[0] == '\0')
								fprintf(stderr, "File letto: vuoto\n");
							else
								fprintf(stderr, "File letto: %s\n", (char *) buff);
						#endif
						
     					if(d_read != NULL){
							if(save_file(d_read, token, buff, size) == -1)
								fprintf(stderr, "si è verificato un errore nel salvataggio di: %s [-r]\n", token);
						}
						if (closeFile(token) == -1)
							perror("Errore in closeFile [-r]");
						
						token = strtok_r(NULL, ",", &tmpstr);
						free(buff); 				
					}
				}
				
				else{
					fprintf(stderr, "connessione non ancora aperta, socket non specificato\n");
					errno = ENOTCONN; 
					return -1;
				} 
				
				break;
				
			case 'R' :  //!!!!!!non riesco a farlo funzionare con argomento opzionale (problema bash credo, non posso usare la forma :R::, guarda sulla virtual machine se va )
				
				found_r = 1;
				int all = 0;
				
				if(socket_name != NULL){
					if(optarg != NULL){
						if(isNumber(optarg, &n) != 0){
							fprintf(stderr,"L'argomento di R deve essere un numero [-R] \n");
							break; //ignoro la richiesta e vado alla successiva
						}	
					}
					else{
						all = 1;
						n = 0;
					}
					if(n <= 0) 
						all = 1;
					
					res = readNFile(n, d_read);
					if(res == -1)
						perror("Errore in readNFiles");  
					
					if(found_p){
						if(res != -1){
							if(all) //non posso sapere lato client quali file sono stati letti e con quale size (struttura api ReadNFiles)
								fprintf(stderr, "Tipo di operazione: Lettura tutti i file, File effettivamente letti: %d, Esito: SUCCESS\n", res);
							else
								fprintf(stderr, "Tipo di operazione: Lettura %d file, File effettivamente letti: %d, Esito: SUCCESS\n", n, res);
						}
						else {
							if(all)
								fprintf(stderr, "Tipo di operazione: Lettura tutti i file, Esito: FAIL\n"); //ignoro la richiesta e vado alla successiva
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
			
			case 'd' :
				d_read = optarg;
				
				if(found_p)
						print_p("Specifica cartella file letti", &optarg, 1, 0, -1, -1);
				
				found_r = 0; //ogni volta che inserisco una nuova directory azzero found_r 
				break;
				
			case 't' : //optarg in millisecondi 
				if(isNumber(optarg, &n) == 0 && n >= 0){
					res = 1;
					ts.tv_sec = n / 1000;  
					ts.tv_nsec = (n % 1000) * 1000000;
				}
				else{
					res = -1;
					fprintf(stderr, "argomento di -t errato [-t] \n"); //ignoro la richiesta e vado alla successiva
				}
				if(found_p)
						print_p("Specifica tempo di attesa tra due richieste", NULL, 0, res, -1, -1);
				
				break;
				
			case 'l' :  
			    if(socket_name != NULL){
					
					token = strtok_r(optarg, ",", &tmpstr);
					while (token) { //ho scelto che per fare la lock non devo aprire il file 
						res = lockFile(token);
						errno_copy = errno;
						if(found_p)
							print_p("Lock file", &token, 1, res, -1, -1);
						if(res == -1){
							errno = errno_copy;
							perror("Errore in lockFile [-l]"); //ignoro la richiesta e vado alla successiva
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
				
			case 'u' : 
				if(socket_name != NULL){
					
					token = strtok_r(optarg, ",", &tmpstr);
					while (token) { //ho scelto che per fare la unlock non devo aprire il file 
					
						res = unlockFile(token);
						if(found_p)
							print_p("Unock file", &token, 1, res, -1, -1);
						if(res == -1){
							errno = errno_copy;
							perror("Errore in unlockFile [-u]"); //ignoro la richiesta e vado alla successiva
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
			
			case 'c' : 
				if(socket_name != NULL){
					
					token = strtok_r(optarg, ",", &tmpstr);
					while (token) { //per rimuovere file non devo aprirlo, solo fare la lock
					 
						if(lockFile(token) == -1){
							perror("Errore in lockFile [-c]");
							if(found_p)
								print_p("Remove file", &token, 1, -1, -1, -1);
							token = strtok_r(NULL, ",", &tmpstr);
							continue; //ignoro la richiesta e vado alla successiva
						}
						res = removeFile(token);
						
						if(res == -1){
							perror("Errore in removeFile"); 
							if(unlockFile(token) == -1)
								perror("Errore in unlockFile [-c]");
						}
						if(found_p)
							print_p("Remove file", &token, 1, res, -1, -1);
						
                        //è stato cancellato, quindi non ha senso l'unlock 
						token = strtok_r(NULL, ",", &tmpstr);	
					}
				}
				else{
					fprintf(stderr, "connessione non ancora aperta, socket non specificato\n");
					errno = ENOTCONN; 
					return -1;
				} 
				
				break;
				
			case 'p' :
				
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

void print_p(char *op, char *file[], int n_files, int esito, int rB, int wB){
	if(op != NULL){
		printf("\nTipo di operazione: %s, ", op);
		if(file != NULL && n_files > 0){ //in realtà per come la uso, n_files sarà MAX 1 
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
			

int isdot(const char dir[]) {
  int l = strlen(dir);
  
  if ( (l > 0 && dir[l-1] == '.') ) 
	  return 1;
  return 0;
}


//modifica della funzione vista a lezione 
void lsR(const char *dir, int *err, int *n, int flag, char *D_removed, int found_p) {
	
    
	if(!(*err) && *n > 0){ //fermo la ricorsione prima che i file finiscano nel caso in cui flag = 1  (devo leggere tot specifico di file)
		
		struct stat st = {0};
		
		// controllo ogni volta che il parametro sia una directory
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
						lsR(filename, err, n, flag, D_removed, found_p); //ricorsione
				}
				else if(S_ISREG(statbuf.st_mode)){
					if(*n <= 0)
						return;
					if(write_file(filename, D_removed, found_p) == -1){
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

int write_file(char *file, char *D_removed, int found_p){

	FILE *fp;
	char *buff;
	int size, n, res;
	struct stat st;
	int errno_copy;

	errno = 0;
	
	if(openFile(file, O_LOCK) == -1){   //per scrivere apro il file con la lock 
		if(errno == ENOENT){           //se il file non esiste ne creo uno con attiva la lock e scrivo
			if(openFile(file, O_CREATE_LOCK) == -1){
				perror("Errore in openFile CL (write_file)");
				if(found_p){
					print_p("Write File", &file, 1, -1, -1, -1);
				}
				return -1;
			}
			
			res = writeFile(file, D_removed);
			errno_copy = errno;
			
			if(found_p){
				if((fp = fopen(file, "rb")) == NULL){
					perror("errore fopen");
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
				perror("Errore in writeFile");
				closeFile(file);
				return -1;
			}	
		}
		else {
			perror("Errore in openFile L (write_file)");
			if(found_p)
				print_p("Write File", &file, 1, -1, -1, -1);
			return -1;                      
		}
	}
	else {   //se esite scrivo in append 
		
		if((fp = fopen(file, "rb")) == NULL){
			perror("errore fopen");
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
		n = fread(buff, 1, size, fp); //metto in buff il contenuto del file 
		
        if(!n){
			if (ferror(fp)){    
				perror("fread");
				fclose(fp);
				if(found_p){
					print_p("Write File", &file, 1, -1, -1, -1);
				}
				closeFile(file);
				//devo fare l'unlock? potrei fare che quando chiudo un file lo unlockko (se ho la lock)
				free(buff);
        		return -1;
			}
		}
		
		if(fclose(fp) == -1){
			perror("fclose write_file");
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
			perror("Errore in appendToFile");
			closeFile(file);
			return -1;
		}
	}
	if(closeFile(file) == -1)
		perror("Errore in closeFile [write_file]");
	
	return 0;
}
