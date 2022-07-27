#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include "./icl_hash.h"

#define DEBUG
//#undef DEBUG
#define UNIX_PATH_MAX 108
#define MSG_SIZE  50
#define SOCKNAME "/home/giulia/Server_storage/mysock"
#define NBUCKETS  256 // n. di buckets nella tabella hash 
#define MAX_REQ   20 //n. max di richieste in coda 
#define N 15

int main(int argc, char *argv[]){
	
	int  fd_skt, fd_c; //socket passivo che accetta, socket per la comunicazione
	char *msg; 
	char *string = NULL;
	
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

	listen(fd_skt, MAX_REQ); //gestire errore
	
	fd_c = accept(fd_skt, NULL, 0); //gestire errore, crea un nuovo socket per la comunicazione e ne restituisce il file descriptor

	char *aux = malloc(MSG_SIZE*sizeof(char));
	char *stringa;
	
	int k;
	icl_entry_t *entry;
	char *key, *value;
	
	
    for(int i = 0; i < N ; i++){
		msg = malloc(MSG_SIZE*sizeof(char)); //controlla la malloc 
		
		read(fd_c, msg, MSG_SIZE); //gestione errore 
		fprintf(stderr, "\n*********Contenuto canale di comunicazione: %s*******\n\n", msg);
		stringa = malloc(MSG_SIZE*sizeof(char));
		
		if((stringa = strstr(msg, "openfc")) != NULL){ //creo file nello storage
			strcpy(aux, stringa + 6);
			if(icl_hash_find(hash_table, aux) != NULL){ 
				fprintf(stderr, "Impossibile ricreare file già esistente\n");
				write(fd_c, "Err:fileEsistente", 18); 
				i = N;
			}
			else{
				if(icl_hash_insert(hash_table, aux, "blablabla") == NULL){
					fprintf(stderr, "Errore nell'inseriemento del file nello storage\n");
					write(fd_c, "Err:inserimento", 16);
					i = N;
				}
				else {
					
				    
                    fprintf(stderr,"---INSERIMENTO INIZIALE openFile\n");
					if (hash_table) {
						for (k = 0; k < hash_table->nbuckets; k++)  {
							//if(hash_table->buckets[k] != NULL  && hash_table->buckets[k]->key != NULL)
								//fprintf(stderr, "k = %d , key = %s\n", k,  (char *) hash_table->buckets[k]->key);
							for (entry = hash_table->buckets[k]; entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL); entry = entry->next)
												printf("k = %d, chiave: %s, valore %s\n",k, key, value);
						}
					}
						write(fd_c, "Ok", 3); //sovrascrive? 
					
				}
				
			}
		}
		else if(strstr(msg, "disconnesso") != NULL){
			close(fd_c); //non so se lo fa in automatico
			i = N;
		}
		else if((stringa = strstr(msg, "closef")) != NULL){
			strcpy(aux, stringa + 6);
			fprintf(stderr,"---CHIUSURA FILE %s\n", aux);
			if(icl_hash_find(hash_table, aux) == NULL){ 
				fprintf(stderr, "Impossibile chiudere file non esistente\n");
				write(fd_c, "Err:fileNonEsistente", 21); 
				i = N;
			}
			
			//qui mancano dei controllo legati alla lock, il file è realmente aperto?
			else
				write(fd_c, "Ok", 3);
		}
		else if((stringa = strstr(msg, "readf")) != NULL){
	        strcpy(aux, stringa + 5);
			fprintf(stderr, "---READ FILE %s\n", aux);
			if(icl_hash_find(hash_table, aux) == NULL){ 
				fprintf(stderr, "!!!Impossibile leggere file non esistente\n");
				write(fd_c, "Err:fileNonEsistente", 21); 
				i = N;
			}
			//qui mancano dei controllo legati alla lock, il file è realmente aperto?
			else{
				
				//string = malloc(MSG_SIZE*sizeof(char));
				string = icl_hash_find(hash_table, aux); //vedi se sovrascrive bene
				if(string == NULL)
				 write(fd_c, "Ok", 3);
				else{
					strcpy(msg, "Ok"); //vedi se sovrascrive
					strcat(msg, string);
					write(fd_c, msg, strlen(msg)+1); //ha senso il terzo parametro?
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
			
	free(aux);		
	
	//close(fd_c);
	close(fd_skt);
	
	
	return 0;
}