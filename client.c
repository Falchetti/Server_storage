#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "api.h"

#define DEBUG
//#undef DEBUG

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

//int isNumber(void *);

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
						
     					/*if(d_read != NULL){
							//DIR * mydir;
							if((dp = opendir("d_read")) == NULL){
								perror("Apertura directory");
								return -1;
							}
							
							if(close(dp) == -1{
								perror("Chiusura directory");
								return -1;
							}
							
							creafile(token, buff); */
						
						free(buff); 
						
						
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
				
			case 'R' :  //per ora senza dirname 
				if(optarg != NULL){
					if(isNumber(optarg, &n) != 0){
						fprintf(stderr,"L'argomento di R deve essere un numero\n");
						return -1;
					}
					else if(n > 0){
						if(readNFile(n, NULL) == -1){
							perror("Errore in readFile");
							return -1;
						} 
					}
					else{ 
						if(readNFile(0, NULL) == -1){
							perror("Errore in readFile");
							return -1;
						} 
					}
				}
				else{
					if(readNFile(0, NULL) == -1){
						perror("Errore in readFile");
						return -1;
					} 
				}
					
				found_r = 1;
				
				
				break;
			
			case 'd' :
				d_read = optarg;
				found_r = 0; //ogni volta che inserisco una nuova directory azzero found_r 
				//ricordati se serve di fare la free di buff, o comunque di capire come liberarlo
				
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
			fprintf(stderr, "HHHHHHH\n");
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