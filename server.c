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

#define UNIX_PATH_MAX 108 //lunghezza massima path
#define MSG_SIZE  130 //path + spazio comandi 
#define MAX_SIZE 230 //path, comandi, contenuto file
#define CNT_SIZE 100

#define SOCKNAME "/home/giulia/Server_storage/mysock" //in realtà lo leggo dal file di configurazione credo 
#define NBUCKETS  256 // n. di buckets nella tabella hash, forse sempre legato alla config 
#define MAX_REQ   20 //n. max di richieste in coda 
#define N 20




typedef struct open_node{
    int fd;
    struct open_node *next;
} open_node;

typedef struct file_info{ 
	int lock_owner;
	open_node *open_owners; 
	int lst_op; //controllo per la writeFile
	char *cnt;
} file_info;



file_info *init_file(int lock_owner, int fd, int lst_op, char *contenuto){
	file_info *tmp = malloc(sizeof(file_info)); 

		
	open_node *client;
    client = malloc(sizeof(open_node));	
	client->fd = fd;	
	client->next = NULL;
	
	tmp->open_owners = client;
		tmp->lock_owner = lock_owner;
	tmp->lst_op = lst_op;
	
	if(strcmp(contenuto, "0") != 0)
		tmp->cnt = contenuto;
	else 
		tmp->cnt = NULL;

	return tmp;
}

int insert_head(open_node **list, int info){
	open_node *tmp = malloc(sizeof(open_node));
	tmp->fd = info;
	tmp->next = *list;
	*list = tmp;
	
	return 0;
}

int remove_elm(open_node **list, int info){
	open_node *curr, *prec;
	
	curr = *list;

	if(curr != NULL){
		if(curr->fd == info){
			*list = (*list)->next;
			free(curr);
		}
		else{
			prec = curr;
			curr = curr->next;
			while(curr != NULL){
				if(curr->fd == info){
					prec->next = curr->next;
					free(curr);
				}
				else{
					prec = curr;
					curr = curr->next;
				}
			}
		}
	}
	return 0;
}

void print_list(open_node *list){
	open_node *aux = list; 
	if (list == NULL)
		fprintf(stderr, "open_owners: NULL\n");
	else{
		fprintf(stderr, "open_owners: ");
		while(aux != NULL){
			fprintf(stderr, "%d, ", aux->fd);
			aux = aux->next;
		}
		fprintf(stderr, "\n");
	}
}

void print_deb(icl_hash_t *ht){
	icl_entry_t *entry;
	char *key, *value;
	int k;
	
	if (ht) {
		file_info *aux;
		for (k = 0; k < ht->nbuckets; k++)  {
			for (entry = ht->buckets[k]; entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL); entry = entry->next){
				aux = icl_hash_find(ht, key);
				fprintf(stderr, "k = %d, chiave: %s, lock_owner: %d, last_op: %d \n",k, key, aux->lock_owner, aux->lst_op);
			
				print_list(aux->open_owners);
					
				if(aux->cnt != NULL)
					fprintf(stderr, "contenuto: %s\n", aux->cnt);
				else
					fprintf(stderr, "contenuto: NULL\n");	
			}
		}
	}
	else{
		fprintf(stderr, "hash table vuota\n");
	}
}

int openFC(icl_hash_t *ht, char *path, int fd){ //free(data)?
	file_info *data;
	if(icl_hash_find(ht, path) != NULL){ 
		fprintf(stderr, "Impossibile ricreare file già esistente\n");
		write(fd, "Err:fileEsistente", 18); 
		return -1;
	}
	else{
		data = init_file(-1, fd, 0, "0"); 

		if(icl_hash_insert(ht, path, data) == NULL){
			fprintf(stderr, "Errore nell'inseriemento del file nello storage\n");
			write(fd, "Err:inserimento", 16);
			return -1;
		}
		
		write(fd, "Ok", 3);
		
		#ifdef DEBUG
			fprintf(stderr,"---OPEN CREATE FILE\n");
			print_deb(ht);
		#endif 
	}
	return 0;
}

int openFL(icl_hash_t *ht, char *path, int fd){
	//HO CIRCA COPIATO IL CODICE DEL SERVER SUL MESSAGGIO LCKF, POI IMPACCHETTA TUTTO IN DELLE FUNZIONI 		
	file_info *tmp;
	if((tmp = icl_hash_find(ht, path)) == NULL){ 
		fprintf(stderr, "Impossibile fare lock su file non esistente\n");
		write(fd, "Err:fileNonEsistente", 21); 
		/*char * msg = malloc(MSG_SIZE*sizeof(char));
		
		read(fd, msg, 21);
		fprintf("VEDIAMO: msg %s strlen %d\n", msg, strlen(msg));*/
		//qui ci vuole un return -1?
		return -1;
	} 
	else{ //se sta cercando di aprire un file che ha già aperto, lo ignoro (per ora)
		while((tmp->lock_owner != -1 && tmp->lock_owner != fd)){
			sleep(1); //attesa attiva??
		}
		tmp->lock_owner = fd;
		insert_head(&(tmp->open_owners), fd); //dovrei controllare val ritorno
		
		tmp->lst_op = 0;
		write(fd, "Ok", 3);	
		
	}
	
	#ifdef DEBUG
		fprintf(stderr,"---OPEN LOCKED FILE\n");
		print_deb(ht);
	#endif
	return 0;
}

int openFO(icl_hash_t *ht, char *path, int fd){
	file_info *tmp;
	
	if((tmp = icl_hash_find(ht, path)) == NULL){ 
		fprintf(stderr, "Impossibile aprire un file non esistente\n");
		write(fd, "Err:fileNonEsistente", 21);
		return -1;
	} 
	else{ //se sta cercando di aprire un file che ha già aperto, lo ignoro (per ora)
		insert_head(&(tmp->open_owners), fd); //dovrei controllare val ritorno
		tmp->lst_op = 0;
		
		write(fd, "Ok", 3);	
	}
	#ifdef DEBUG
		fprintf(stderr,"---OPEN FILE \n");
		print_deb(ht);
	#endif
	
	return 0;
	
}

int openFCL(icl_hash_t *ht, char *path, int fd){
		file_info *data;
		
		if(icl_hash_find(ht, path) != NULL){ 
			fprintf(stderr, "Impossibile ricreare un file già esistente\n");
			write(fd, "Err:fileEsistente", 18); 
			return -1;
		} 
		else{ 
			data = init_file(fd, fd, 1, "0"); 
		
			if(icl_hash_insert(ht, path, data) == NULL){
				fprintf(stderr, "Errore nell'inseriemento del file nello storage\n");
				write(fd, "Err:inserimento", 16);
				return -1;
			}			
			
			write(fd, "Ok", 3);	
		}
		
		#ifdef DEBUG
			fprintf(stderr,"---OPEN CREATE LOCKED FILE\n");
			print_deb(ht);
		#endif
		
		return 0;
	}
	
int closeF(icl_hash_t *ht, char *path, int fd){
	file_info *tmp;
	
	if((tmp = icl_hash_find(ht, path)) == NULL){ 
		fprintf(stderr, "Impossibile chiudere file non esistente\n");
		write(fd, "Err:fileNonEsistente", 21); 
		return -1;
	}
	else if(tmp->open_owners == NULL || tmp->open_owners->fd != fd){
		fprintf(stderr, "Impossibile chiudere file che non si è prima aperto\n");
		write(fd, "Err:fileNonAperto", 18); 
		return -1;
	}
	else{
		
		remove_elm(&(tmp->open_owners), fd);
						
		if(tmp->lock_owner == fd)
			tmp->lock_owner = -1;
		tmp->lst_op = 0;

		write(fd, "Ok", 3);
	}
	#ifdef DEBUG
		fprintf(stderr,"---CLOSE FILE \n");
		print_deb(ht);
	#endif
	
	return 0;
}

int readF(icl_hash_t *ht, char *path, int fd){
	file_info *tmp;	
    char *msg = malloc(MSG_SIZE*sizeof(char));	
	if((tmp = icl_hash_find(ht, path)) == NULL){ 
		fprintf(stderr, "Impossibile leggere file non esistente\n");
		write(fd, "Err:fileNonEsistente", 21); 
		return -1;
	}
	else if(tmp->open_owners == NULL || tmp->open_owners->fd != fd){
		fprintf(stderr, "Impossibile leggere file non aperto\n");
		write(fd, "Err:fileNonAperto", 18); 
		return -1;
	}
	else if(tmp->lock_owner != fd && tmp->lock_owner != -1 ){
		fprintf(stderr, "Impossibile leggere file con lock attiva\n");
		write(fd, "Err:fileLocked", 15); 
		return -1;
	}
	else{
		tmp->lst_op = 0;
		strcpy(msg, "Ok"); //vedi se sovrascrive
		if(tmp->cnt != NULL)
			strcat(msg, tmp->cnt);
		write(fd, msg, strlen(msg)+1); //ha senso il terzo parametro? 
	}
	
	#ifdef DEBUG
		fprintf(stderr,"---READ FILE \n");
		print_deb(ht);
	#endif
	free(msg);
	
	return 0;
			
}

int writeF(icl_hash_t *ht, char *path, char *cnt, int fd){
	file_info *tmp;	
	
	if((tmp = icl_hash_find(ht, path)) == NULL){ 
		fprintf(stderr, "Impossibile scrivere su file non esistente\n");
		write(fd, "Err:fileNonEsistente", 21); 
		return -1;
	}
	else if(tmp->open_owners == NULL || tmp->open_owners->fd != fd){
		fprintf(stderr, "Impossibile scrivere su file non aperto\n");
		write(fd, "Err:fileNonAperto", 18); 
		return -1;
	}
	else if (tmp->lst_op == 0) { //forse dovrei usare l'update insert 
		fprintf(stderr, "L'operazione precedente a writeFile deve essere openFile\n");
		write(fd, "Err:lastOperation", 18); 
		return -1;
	}
	else if(tmp->lock_owner != fd){ //questo me lo assicura l'if precedente in realtà 
		fprintf(stderr, "Impossibile scrivere su file senza lock\n");
		write(fd, "Err:fileSenzaLock", 18); 
		return -1;
	}
	else{
		if(cnt != NULL){
			if(tmp->cnt == NULL)
				tmp->cnt = malloc(MSG_SIZE*sizeof(char));
			strcpy(tmp->cnt, cnt);
		}
		else
			fprintf(stderr, "richiesta scrittura di file vuoto\n");
	    write(fd, "Ok", 3);
	}
	
	#ifdef DEBUG
		fprintf(stderr,"---WRITE FILE \n");
		print_deb(ht);
	#endif
		
	
	return 0;
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

	char *token, *token2;
	char *tmpstr;

	
    for(int i = 0; i < N ; i++){
		msg = malloc(MSG_SIZE*sizeof(char)); //controlla la malloc 
		
		if(read(fd_c, msg, MSG_SIZE) == -1){
			perror("read socket lato server");
			return -1; //ok?
	    }
		
		fprintf(stderr, "\n*********Contenuto canale di comunicazione: %s*******\n\n", msg);
		
		//stringa = malloc(MSG_SIZE*sizeof(char)); //potrei usare direttamente msg 
		
		token = strtok_r(msg, ";", &tmpstr);
		
		if(token != NULL){
			
			if(strcmp(token, "openfc") == 0){
				token = strtok_r(NULL, ";", &tmpstr);
				if(openFC(hash_table, token, fd_c) == -1){
					i = N;
				}
			}
			else if(strcmp(token, "openfl") == 0){
				token = strtok_r(NULL, ";", &tmpstr);
				if(openFL(hash_table, token, fd_c) == -1){
					//i = N;
				}
			}
			else if(strcmp(token, "openfo") == 0){
				token = strtok_r(NULL, ";", &tmpstr);
				if(openFO(hash_table, token, fd_c) == -1){
					i = N;
				}
			}
			else if(strcmp(token, "openfcl") == 0){
				token = strtok_r(NULL, ";", &tmpstr);
				if(openFCL(hash_table, token, fd_c) == -1){
					i = N;
				}
			}
			else if(strcmp(token, "disconnesso") == 0){//da chiudere i file e aggiornarli
				close(fd_c); //non so se lo fa in automatico
				i = N;
			}
			else if(strcmp(token, "closef") == 0){
				token = strtok_r(NULL, ";", &tmpstr);
				if(closeF(hash_table, token, fd_c) == -1){
					i = N;
				}
			}
			else if(strcmp(token, "readf") == 0){
				token = strtok_r(NULL, ";", &tmpstr);
				if(readF(hash_table, token, fd_c) == -1){
					i = N;
				}
			}
			else if(strcmp(token, "writef") == 0){
				token = strtok_r(NULL, ";", &tmpstr);
				token2 = strtok_r(NULL, ";", &tmpstr);
				if(strcmp(token2, "$") == 0)
					token2 = NULL;
					
				if(writeF(hash_table, token, token2, fd_c) == -1){
					i = N;
				}
						
			}
			else{
				fprintf(stderr, "Opzione non ancora implementata\n");
				write(fd_c, "Ok", 3);
			}
			
			//free(stringa);	
			//free(msg); non ho capito chi fa la free di msg, mi sa la close, ma va bene?	
		}
		/*
		else if((stringa = strstr(msg, "lckf")) != NULL){ //problema lock unlock in strstr da gestire meglio 
	        strcpy(aux, stringa + 4);
			fprintf(stderr, "---LOCK FILE %s\n", aux);
			
			if((tmp = icl_hash_find(hash_table, aux)) == NULL){ 
				fprintf(stderr, "Impossibile fare lock su file non esistente\n");
				write(fd_c, "Err:fileNonEsistente", 21); 
				i = N;
			}
			

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
							fprintf(stderr, "k = %d, chiave: %s, lock_owner: %d, last_op: %d \n",k, key, aux->lock_owner, aux->lst_op);
						
							print_list(aux->open_owners);
								
							if(aux->cnt != NULL)
								fprintf(stderr, "contenuto: %s\n", aux->cnt);
							else
								fprintf(stderr, "contenuto: NULL\n");							
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
							fprintf(stderr, "k = %d, chiave: %s, lock_owner: %d, last_op: %d \n",k, key, aux->lock_owner, aux->lst_op);
						
							print_list(aux->open_owners);
								
							if(aux->cnt != NULL)
								fprintf(stderr, "contenuto: %s\n", aux->cnt);
							else
								fprintf(stderr, "contenuto: NULL\n");
						}
					}
				}
			#endif	
			
			}
			
			
		}
		
		else if((stringa = strstr(msg, "appendtof")) != NULL){
	        aux = strtok_r(stringa, ";", &tmpstr);
			aux = strtok_r(NULL, ";", &tmpstr);
			fprintf(stderr, "---APPEND TO FILE %s\n", aux);
			
			if((tmp = icl_hash_find(hash_table, aux)) == NULL){ 
				fprintf(stderr, "Impossibile scrivere su file non esistente\n");
				write(fd_c, "Err:fileNonEsistente", 21); 
				i = N;
			}
			else if(tmp->open_owners == NULL || tmp->open_owners->fd != fd_c){
					fprintf(stderr, "Impossibile scrivere su file non aperto\n");
					write(fd_c, "Err:fileNonAperto", 18); 
					i = N;
				}
			else if(tmp->lock_owner != fd_c){
					fprintf(stderr, "Impossibile scrivere su file senza lock\n");
					write(fd_c, "Err:fileSenzaLock", 18); 
					i = N;
				}
			else{
				aux = strtok_r(NULL, ";", &tmpstr);
				if(aux != NULL)
					if(tmp->cnt == NULL){
							tmp->cnt = malloc(MSG_SIZE*sizeof(char));
							strcpy(tmp->cnt, aux);
					}
				    else
					    strcat(tmp->cnt, aux);
				else
					fprintf(stderr, "richiesta scrittura di file vuoto\n");
				write(fd_c, "Ok", 3);			
			}
			
		}	
	
*/
		
		#ifdef DEBUG
			fprintf(stderr,"\n---HASHTABLE:\n");
			print_deb(hash_table);
		#endif	
	}
				
	//free(aux);		
	
	//close(fd_c);
	close(fd_skt);
	
	
	return 0;
}

//NB: quando chiudo la connessione devo modificare il file (togliere quel client dalla lista)
// quando faccio qualsiasi op sul file (tranne quella speciale) devo resettare il flag
//all'inizio forse dovrei inizializzare il tutto, non so se lo sto facendo o no 