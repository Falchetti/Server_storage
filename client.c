#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "./api.h"

#define DEBUG
#undef DEBUG

/*Nella mia scelta di progetto conta l'ordine dei flag.
Il flag f (socketname) deve essere specificato prima di richiedere i flag w,W,r,R,l,u,c 
(flag che prevedono richieste al server) e non deve essere ripetuto
I flag d (e D) devono essere specificati prima delle r/R (e w/W) a cui fanno riferimento
idem per il flag t, solo le richieste a lui successive avranno quello specifico tempo di attesa tra due richieste*/


#define N_FLAG 13
#define SZ_STRING 1000
#define MSEC_TENT 1000  //un tentativo al secondo nella openConnection
#define SEC_TIMEOUT 10 //secondi prima del timeout nella openConnection


//int isNumber(void *);

int main(int argc, char *argv[]){
	
	int flag;
	char *d_read = NULL;
	char *D_removed = NULL;
	int t_requests = 0;
	char *socket_name = NULL; 
	char *files = NULL; //potrei usare piuttosto il nome arg e metterla ovunque, anche su w e R
	int found_r = 0;
	int found_w = 0;
	int found_h = 0;
	int found_p = 0;

	
	while(((flag = getopt(argc, argv, "hf:w:W:D:r:R:d:t:l:u:c:p")) != -1) && !found_h ){
		
		#ifdef DEBUG 
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
				
			case 'f' : 
				
				if(socket_name == NULL){
					socket_name = optarg;
					
					struct timespec timeout;
					clock_gettime(CLOCK_REALTIME,&timeout);
					timeout.tv_sec += SEC_TIMEOUT;
					
                    // tentativo di connessione, provo per un minuto facendo 1 tentativo al secondo 
					if(openConnection(socket_name, MSEC_TENT, timeout) == -1){
						perror("Errore in openConnection"); 
						exit(errno); //vedi se va bene l'uso di exit
					}
				}
				else
					printf("Attenzione: l'opzione f può essere richiesta una sola volta e necessita di un argomento\n"); //stampo questo e ignoro gli f successivi al primo
	
				break;
				
			case 'w' : 
				//dir_etc = optarg;
				//ricordati della parte n 
				found_w = 1;
				
				break;
			
			case 'W' : 
				files = optarg;
				found_w = 1;
				
				break;
				
			case 'D' :
			
				D_removed = optarg;
				found_w = 0; //ogni volta che inserisco una nuova directory azzero found_w
				
				
				break;
				
			case 'r' :
			
			    files = optarg;
				found_r = 1;
				if(socket_name != NULL){
					char *tmpstr;
					char *token = strtok_r(optarg, ",", &tmpstr);
					while (token) {
						if(openFile(token, 0) == -1){
							perror("Errore in openFile");
							return -1;                               //attenzione che questi return -1 non saltino i cleanup necessari
						}
						char *buff = malloc(SZ_STRING*sizeof(char));
						size_t size = (size_t) SZ_STRING;
						if(readFile(token, (void *) &buff, &size) == -1){
							perror("Errore in readFile");
							return -1;
						}
						fprintf(stderr, "file letto: %s\n", buff);
						if(d_read == NULL)
							free(buff);
						if (closeFile(token) == -1){
							perror("Errore in closeFile");
							return -1;
						}
						/*assicurati di non poter fare una read se il file
						non è aperto! 
						if(readFile(token, (void *) &buff, &size) == -1){
							perror("Errore in readFile");
							return -1;
						}
						fprintf(stderr, "file letto: %s\n", buff); //NO, NON DOVREBBE FUNZIONARE!
				        */
						token = strtok_r(NULL, " ", &tmpstr);
						
					}
				}
				
				else{
					fprintf(stderr, "connessione non acora aperta, socket non specificato\n");
					return -1;
				} 
				
				break;
				
			case 'R' :
				//devi fare tutta la parte di isNum etc 
				//n_files = optarg;
				found_r = 1;
				
				break;
			
			case 'd' :
				d_read = optarg;
				found_r = 0; //ogni volta che inserisco una nuova directory azzero found_r 
				
				break;
				
			case 't' :
				
				break;
				
			case 'l' :  //controlla bene come funzionano -l e -u (devo aprire il file?)
			    if(socket_name != NULL){
					char *tmpstr;
					char *token = strtok_r(optarg, ",", &tmpstr);
					while (token) {
						/*if(openFile(token, 0) == -1){ //posso aprire un file più volte?? Serve un controllo?
							perror("Errore in openFile");
							return -1;                              
						}*/
						if(lockFile(token) == -1){
							perror("Errore in lockFile");
							return -1;
						}
						/*if (closeFile(token) == -1){
							perror("Errore in closeFile");
							return -1;
						}*/
						token = strtok_r(NULL, " ", &tmpstr);
						
					}
				}
				
				else{ 
					fprintf(stderr, "connessione non acora aperta, socket non specificato\n");
					return -1;
				} 
				
				break;
				
			case 'u' : //controlla bene come funzionano -l e -u (devo aprire il file?)
			if(socket_name != NULL){
					char *tmpstr;
					char *token = strtok_r(optarg, ",", &tmpstr);
					while (token) {
						/*if(openFile(token, 0) == -1){ //posso aprire un file più volte?? Serve un controllo?
							perror("Errore in openFile");
							return -1;                             
						}*/
						if(unlockFile(token) == -1){
							perror("Errore in unlockFile");
							return -1;
						}
						/*if (closeFile(token) == -1){
							perror("Errore in closeFile");
							return -1;
						}*/
						token = strtok_r(NULL, " ", &tmpstr);
						
					}
				}
				
				else{
					fprintf(stderr, "connessione non acora aperta, socket non specificato\n");
					return -1;
				} 
				
				break;
			
			case 'c' : 
				
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
		printf("flag h: Le opzioni accettate sono hf:w:W:D:r:R:d:t:l:u:c:p\n");

    if(closeConnection(socket_name) == -1){
		perror("Errore in closeConnection");
		exit(errno);
	}
	
	return 0;
	
}

/*int isNumber(void *el){
	char *e = NULL;
	errno = 0;
	strtol(el, &e, 10);
	if(errno == ERANGE) return 2;
	if(e != NULL && *e == (char) 0)
		return 0; 
	return 1;
}*/