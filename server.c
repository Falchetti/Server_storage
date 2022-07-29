#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include "icl_hash.h"

#define DEBUG
//#undef DEBUG
#define UNIX_PATH_MAX 108
#define MSG_SIZE  50
#define SOCKNAME "/home/giulia/Server_storage/mysock" //in realtà lo leggo dal file di configurazione credo 
#define NBUCKETS  256 // n. di buckets nella tabella hash 
#define MAX_REQ   20 //n. max di richieste in coda 
#define N 18

typedef struct file_info{ 
	int lock_owner;
	//open_list *open_owners; (prima semplificazione, da scambiare poi con quella sotto)
	int open_owner;
	int lst_op; //controllo per la writeFile
	char *cnt;
} file_info;

/*typedef struct open_list{ //lista di chi ha fatto la open su un dato file 
	int fd_client;
	struct open_list *next;
} open_list;*/

file_info *init_file(int lock_flag, int client, int lst_op, char *contenuto){
	file_info *tmp = malloc(sizeof(file_info)); 
	tmp->lock_owner = lock_flag;
	
	/*open_list list;
	list->fd_client = client;
	list->next = NULL;
	tmp->open_owners = list;*/  
	
	tmp->open_owner = client;
	tmp->lst_op = lst_op;
	tmp->cnt = contenuto;
	
	return tmp;
}
				

int main(int argc, char *argv[]){
	
	int  fd_skt, fd_c; //socket passivo che accetta, socket per la comunicazione
	char *msg; 
		
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(struct sockaddr_un));
	strncpy(sa.sun_path, SOCKNAME, UNIX_PATH_MAX);
	sa.sun_family = AF_UNIX;
	
	icl_hash_t *hash_table;
	
	if((hash_table = icl_hash_create(NBUCKETS, hash_pjw, string_compare)) == NULL){
		fprintf(stderr, "Errore nella creazione della hash table\n");
		return -1;
	}
	
	if((fd_skt = socket(AF_UNIX, SOCK_STREAM, 0)) == -1){
		perror("Errore nella socket");
		return -1;
	}
	
	if(bind(fd_skt, (struct sockaddr *) &sa, sizeof(sa)) == -1){
		perror("Errore nella bind");
		return -1;
	} 

	listen(fd_skt, MAX_REQ); //fai gestire errore
	
	fd_c = accept(fd_skt, NULL, 0); //fai gestire errore, crea un nuovo socket per la comunicazione e ne restituisce il file descriptor

	char *aux = malloc(MSG_SIZE*sizeof(char));
	char *stringa;
	
	int k;
	icl_entry_t *entry;
	char *key, *value;
	file_info *tmp;
	FILE *fp;
	file_info *data;
	
	
    for(int i = 0; i < N ; i++){
		msg = malloc(MSG_SIZE*sizeof(char)); //controlla la malloc 
		
		read(fd_c, msg, MSG_SIZE); //fai gestione errore 
		fprintf(stderr, "\n*********Contenuto canale di comunicazione: %s*******\n\n", msg);
		stringa = malloc(MSG_SIZE*sizeof(char)); //potrei usare direttamente msg 
		
		if((stringa = strstr(msg, "openfc")) != NULL){ //creo file nello storage, per ora solo O_CREATE (non faccio il controllo se è già aperto perchè per ora lo sto creando)
			strcpy(aux, stringa + 6);
			if(icl_hash_find(hash_table, aux) != NULL){ 
				fprintf(stderr, "Impossibile ricreare file già esistente\n");
				write(fd_c, "Err:fileEsistente", 18); 
				i = N;
			}
			else{
				data = init_file(-1, fd_c, 1, "init"); //qui forse c'è 0 per lst_op, non ho capito 
				
				if(icl_hash_insert(hash_table, aux, data) == NULL){
					fprintf(stderr, "Errore nell'inseriemento del file nello storage\n");
					write(fd_c, "Err:inserimento", 16);
					i = N;
				}
				else {	
					write(fd_c, "Ok", 3); //sovrascrive? 
					
				    #ifdef DEBUG
                    fprintf(stderr,"---INSERIMENTO INIZIALE openFile\n");
						if (hash_table) {
							file_info *aux;
							for (k = 0; k < hash_table->nbuckets; k++)  {
								for (entry = hash_table->buckets[k]; entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL); entry = entry->next){
									aux = icl_hash_find(hash_table, key);
									printf("k = %d, chiave: %s, lock_owner: %d, open_owner: %d, last_op: %d \n",k, key, aux->lock_owner, aux->open_owner, aux->lst_op);
									if(aux->cnt != NULL)
										printf("contenuto: %s\n", aux->cnt);
								}
							}
						}
					#endif	
				}
				free(data);
				
			}
		}
		else if((stringa = strstr(msg, "openfl")) != NULL){ //creo file nello storage, per ora solo O_CREATE (non faccio il controllo se è già aperto perchè per ora lo sto creando)
			strcpy(aux, stringa + 6);
		//HO CIRCA COPIATO IL CODICE DEL SERVER SUL MESSAGGIO LCKF, POI IMPACCHETTA TUTTO IN DELLE FUNZIONI 		
			if((tmp = icl_hash_find(hash_table, aux)) == NULL){ 
				fprintf(stderr, "Impossibile fare lock su file non esistente\n");
				write(fd_c, "Err:fileNonEsistente", 21); 
				i = N;
			} 
			else{ //se sta cercando di aprire un file che già aperto, lo ignoro per ora
			    while((tmp->lock_owner != -1 && tmp->lock_owner != fd_c)){
					sleep(1); //attesa attiva??
				}
				tmp->lock_owner = fd_c;
				tmp->open_owner = fd_c;
				
				//aggiungiusta lst_op, non so se 0 o 1
				tmp->lst_op = 1;
				
				write(fd_c, "Ok", 3);	
				
				#ifdef DEBUG
				fprintf(stderr,"---LOCK openFile\n");
					if (hash_table) {
						file_info *aux;
						for (k = 0; k < hash_table->nbuckets; k++)  {
							for (entry = hash_table->buckets[k]; entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL); entry = entry->next){
								aux = icl_hash_find(hash_table, key);
								printf("k = %d, chiave: %s, lock_owner: %d, open_owner: %d, last_op: %d \n",k, key, aux->lock_owner, aux->open_owner, aux->lst_op);
								if(aux->cnt != NULL)
									printf("contenuto: %s\n", aux->cnt);
							}
						}
					}
				#endif
			}
							
		}
		else if(strstr(msg, "disconnesso") != NULL){ //da chiudere i file e aggiornarli
			close(fd_c); //non so se lo fa in automatico
			i = N;
		}
		else if((stringa = strstr(msg, "closef")) != NULL){ //rilascio lock 
			strcpy(aux, stringa + 6);
			fprintf(stderr,"---CHIUSURA FILE %s\n", aux);
			if((tmp = icl_hash_find(hash_table, aux)) == NULL){ 
				fprintf(stderr, "Impossibile chiudere file non esistente\n");
				write(fd_c, "Err:fileNonEsistente", 21); 
				i = N;
			}
			else if(tmp->open_owner != fd_c){
				fprintf(stderr, "Impossibile chiudere file che non si è prima aperto\n");
				write(fd_c, "Err:fileNonAperto", 18); 
				i = N;
			}
			else{
				tmp->open_owner = -1;
				if(tmp->lock_owner == fd_c)
					tmp->lock_owner = -1;
				tmp->lst_op = 0;
				write(fd_c, "Ok", 3);
			}
			
		}
		else if((stringa = strstr(msg, "readf")) != NULL){
	        strcpy(aux, stringa + 5);
			fprintf(stderr, "---READ FILE %s\n", aux);
			if((tmp = icl_hash_find(hash_table, aux)) == NULL){ 
				fprintf(stderr, "Impossibile leggere file non esistente\n");
				write(fd_c, "Err:fileNonEsistente", 21); 
				i = N;
			}
			else if(tmp->open_owner != fd_c){
				fprintf(stderr, "Impossibile leggere file non aperto\n");
				write(fd_c, "Err:fileNonAperto", 18); 
				i = N;
			}
			else if(tmp->lock_owner != fd_c && tmp->lock_owner != -1 ){
				fprintf(stderr, "Impossibile leggere file con lock attiva\n");
				write(fd_c, "Err:fileLocked", 15); 
				i = N;
			}
			else{
				if(tmp == NULL) //va bene nessun contenuto?
				    write(fd_c, "Ok", 3);
				else{
					tmp->lst_op = 0;
					strcpy(msg, "Ok"); //vedi se sovrascrive
					strcat(msg, tmp->cnt);
					write(fd_c, msg, strlen(msg)+1); //ha senso il terzo parametro? 
				}
					
			}
			
		}
		else if((stringa = strstr(msg, "lckf")) != NULL){ //problema lock unlock in strstr da gestire meglio 
	        strcpy(aux, stringa + 4);
			fprintf(stderr, "---LOCK FILE %s\n", aux);
			
			if((tmp = icl_hash_find(hash_table, aux)) == NULL){ 
				fprintf(stderr, "Impossibile fare lock su file non esistente\n");
				write(fd_c, "Err:fileNonEsistente", 21); 
				i = N;
			}
			/*
			else if(tmp->open_owner != fd_c){ //scelto di fare lock solo su file aperti, è ok? 
				fprintf(stderr, "Impossibile fare lock su file non aperto\n");
				write(fd_c, "Err:fileNonAperto", 18); 
				i = N;
			}
			//*/
			else{
			    while((tmp->lock_owner != -1 && tmp->lock_owner != fd_c)){
					sleep(1); //attesa attiva??
				}
				tmp->lock_owner = fd_c;
				tmp->lst_op = 0; //vedi se crea problemi alla openfile(O_create|o_lock)
				write(fd_c, "Ok", 3);		
				
				#ifdef DEBUG
					if (hash_table) {
						file_info *aux;
						for (k = 0; k < hash_table->nbuckets; k++)  {
							for (entry = hash_table->buckets[k]; entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL); entry = entry->next){
								aux = icl_hash_find(hash_table, key);
								printf("k = %d, chiave: %s, lock_owner: %d, open_owner: %d, last_op: %d \n",k, key, aux->lock_owner, aux->open_owner, aux->lst_op);
								if(aux->cnt != NULL)
									printf("contenuto: %s\n", aux->cnt);
							}
						}
					}
				#endif
			}
			
		}
		else if((stringa = strstr(msg, "unlockf")) != NULL){
	        strcpy(aux, stringa + 7);
			fprintf(stderr, "---UNLOCK FILE %s\n", aux);
			if((tmp = icl_hash_find(hash_table, aux)) == NULL){ 
				fprintf(stderr, "Impossibile fare lock su file non esistente\n");
				write(fd_c, "Err:fileNonEsistente", 21); 
				i = N;
			}
			/*
			else if(tmp->open_owner != fd_c){ //scelto di fare unlock solo su file aperti, è ok? 
				fprintf(stderr, "Impossibile fare unlock su file non aperto\n");
				write(fd_c, "Err:fileNonAperto", 18); 
				i = N;
			}
			//*/
			else if(tmp->lock_owner != fd_c){
				fprintf(stderr, "Impossibile fare unlock su file di cui non si ha la lock\n");
				write(fd_c, "Err:Nolock", 21); 
				i = N;
			}
			else{
				tmp->lock_owner = -1;
				tmp->lst_op = 0;
				write(fd_c, "Ok", 3);
				
				#ifdef DEBUG
					if (hash_table) {
						file_info *aux;
						for (k = 0; k < hash_table->nbuckets; k++)  {
							for (entry = hash_table->buckets[k]; entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL); entry = entry->next){
								aux = icl_hash_find(hash_table, key);
								printf("k = %d, chiave: %s, lock_owner: %d, open_owner: %d, last_op: %d \n",k, key, aux->lock_owner, aux->open_owner, aux->lst_op);
								if(aux->cnt != NULL)
									printf("contenuto: %s\n", aux->cnt);
							}
						}
					}
				#endif
			
			}
			
			
		}
		else if((stringa = strstr(msg, "writef")) != NULL){
	        strcpy(aux, stringa + 6);
			fprintf(stderr, "---WRITE FILE %s\n", aux);
			
			if((tmp = icl_hash_find(hash_table, aux)) == NULL){ 
				fprintf(stderr, "Impossibile leggere file non esistente\n");
				write(fd_c, "Err:fileNonEsistente", 21); 
				i = N;
			}
			//qui mancano dei controlli, il file è realmente aperto?
			else{
				
				if(tmp->lst_op == 0) { //forse dovrei usare l'update insert 
				    fprintf(stderr, "L'operazione precedente a writeFile deve essere openFile\n");
					write(fd_c, "Err:last operation", 19); 
					i = N;
				}
				else{
					fp = fopen(aux, "r"); //va bene questa funz? 
					errno = 0;
					if (fscanf(fp, "%s", tmp->cnt) == EOF && errno != 0){//va bene questo controllo? va bene come ho aggiornato il file?
						fprintf(stderr, "fscanf fallita\n");
					    write(fd_c, "Err:fscanf", 19); 
					    i = N;
					}	
					fclose(fp);
					write(fd_c, "Ok", 3);	
				}
					
			}
			
		}
		else{
			fprintf(stderr, "Opzione non ancora implementata\n");
			write(fd_c, "Ok", 3);
		}
		free(stringa);	
		//free(msg); non ho capito chi fa la free di msg, mi sa la close, ma va bene?			
	}
	
	#ifdef DEBUG
		fprintf(stderr,"\n---HASHTABLE\n");
		if (hash_table) {
			file_info *aux;
			for (k = 0; k < hash_table->nbuckets; k++)  {
				for (entry = hash_table->buckets[k]; entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL); entry = entry->next){
					aux = icl_hash_find(hash_table, key);
					printf("k = %d, chiave: %s, lock_owner: %d, open_owner: %d, last_op: %d \n",k, key, aux->lock_owner, aux->open_owner, aux->lst_op);
					if(aux->cnt != NULL)
						printf("contenuto: %s\n", aux->cnt);
				}
			}
		}
	#endif
			
	free(aux);		
	
	//close(fd_c);
	close(fd_skt);
	
	
	return 0;
}

//NB: quando chiudo la connessione devo modificare il file (togliere quel client dalla lista)
// quando faccio qualsiasi op sul file (tranne quella speciale) devo resettare il flag
//all'inizio forse dovrei inizializzare il tutto, non so se lo sto facendo o no 