#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include "icl_hash.h"

#define DEBUG
#undef DEBUG

#define UNIX_PATH_MAX 108 //lunghezza massima path
#define MSG_SIZE  130 //path + spazio comandi 
#define MAX_SIZE 230 //path, comandi, contenuto file
#define CNT_SIZE 100

#define SOCKNAME "/home/giulia/Server_storage/mysock" //in realtà lo leggo dal file di configurazione credo 
#define NBUCKETS  256 // n. di buckets nella tabella hash, forse sempre legato alla config 
#define MAX_REQ   20 //n. max di richieste in coda 
#define N 30
#define THREAD_NUM 4

//ricordati di pensare alle lock su l'hashtable 

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


void print_deb(icl_hash_t *ht);

file_info *init_file(int lock_owner, int fd, int lst_op, char *contenuto);

int insert_head(open_node **list, int info);
int remove_elm(open_node **list, int info);
void print_list(open_node *list);

int openFC(icl_hash_t *ht, char *path, int fd);
int openFL(icl_hash_t *ht, char *path, int fd);
int openFO(icl_hash_t *ht, char *path, int fd);
int openFCL(icl_hash_t *ht, char *path, int fd);
int closeF(icl_hash_t *ht, char *path, int fd);
int readF(icl_hash_t *ht, char *path, int fd);
int writeF(icl_hash_t *ht, char *path, char *cnt, int fd);
int lockF(icl_hash_t *ht, char *path, int fd);
int unlockF(icl_hash_t *ht, char *path, int fd);
int appendToF(icl_hash_t *ht, char *path, char *cnt, int fd);
int removeF(icl_hash_t *ht, char *path, int fd);
int readAF(icl_hash_t *ht, int fd, int seed);
int readAllF(icl_hash_t *ht, int fd);

typedef struct elm_coda{
	int fd;
	struct elm_coda *next;
} elm_coda;

elm_coda *coda = NULL;
pthread_mutex_t mutexCoda;
pthread_cond_t condCoda;

icl_hash_t *hash_table = NULL;

void enqueue(int x){
	elm_coda *new = malloc(sizeof(elm_coda));
	elm_coda *aux = coda;
	
	new->fd = x;
	new->next = NULL;
	if(coda == NULL){
		coda = new;
	}
	else{
		while(aux->next != NULL)
			aux = aux->next;
		aux->next = new;
	}
	return;
}

int dequeue(){ 
	int x;
	elm_coda *aux;
	
	if(coda == NULL)
		return -1;
	
	aux = coda;
	x = coda->fd;
	coda = coda->next;
	free(aux);
	
	return x;
}


int executeTask(int fd_c){ //qui faccio la read, capisco cosa devo fare, lo faccio chiamando la giusta funzione, rispondo al client, scrivo fd sulla pipe per il server  
	/*int res;
	res= fd * 2;
	fprintf(stderr, "il doppio di %d è %d\n", fd, res);*/
	
	char *msg = malloc(MSG_SIZE*sizeof(char)); //controlla la malloc 
	char *tmpstr, *token, *token2;
	int seed = time(NULL); 
		
	if(read(fd_c, msg, MSG_SIZE) == -1){
		perror("read socket lato server");
		return -1; //ok?
	}
	
	#ifdef DEBUG
		fprintf(stderr, "\n*********Contenuto canale di comunicazione: %s*******\n\n", msg);
	#endif
	
	token = strtok_r(msg, ";", &tmpstr);
	
	if(token != NULL){
		
		if(strcmp(token, "openfc") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(openFC(hash_table, token, fd_c) == -1){
				return -1;
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
				return -1;
			}
		}
		else if(strcmp(token, "openfcl") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(openFCL(hash_table, token, fd_c) == -1){
				return -1;
			}
		}
		else if(strcmp(token, "disconnesso") == 0){//da chiudere i file e aggiornarli
			close(fd_c); //non so se lo fa in automatico
			return -1;
		}
		else if(strcmp(token, "closef") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(closeF(hash_table, token, fd_c) == -1){
				return -1;
			}
		}
		else if(strcmp(token, "readf") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(readF(hash_table, token, fd_c) == -1){
				return -1;
			}
		}
		else if(strcmp(token, "writef") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			token2 = strtok_r(NULL, ";", &tmpstr);
			if(strcmp(token2, "$") == 0)
				token2 = NULL;
				
			if(writeF(hash_table, token, token2, fd_c) == -1){
				return -1;
			}			
		}
		else if(strcmp(token, "lockf") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(lockF(hash_table, token, fd_c) == -1){
				return -1;
			}
		}
		else if(strcmp(token, "unlockf") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(unlockF(hash_table, token, fd_c) == -1){
				return -1;
			}
		}
		else if(strcmp(token, "appendTof") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			token2 = strtok_r(NULL, ";", &tmpstr);
			if(strcmp(token2, "$") == 0)
				token2 = NULL;
				
			if(appendToF(hash_table, token, token2, fd_c) == -1){
				return -1;
			}			
		}
		else if(strcmp(token, "removef") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(removeF(hash_table, token, fd_c) == -1){
				return -1;
			}
		}
		else if(strcmp(token, "readAf") == 0){
			if(readAF(hash_table, fd_c, seed) == -1){
				return -1;
			}
			seed++;
		}
		else if(strcmp(token, "readAllf") == 0){
			if(readAllF(hash_table, fd_c) == -1){
				return -1;
			}
		}	
		else{
			fprintf(stderr, "Opzione non ancora implementata\n");
			write(fd_c, "Ok", 3);
		}
		
		//free(msg); non ho capito chi fa la free di msg, mi sa la close, ma va bene?	
	}

	#ifdef DEBUG
		fprintf(stderr,"\n---HASHTABLE:\n");
		print_deb(hash_table);
	#endif	
	
	//SCRIVI SULLA PIPE
	
	return 0;
}

void *init_thread(void *args){ //CONSUMATORE
	int fd;
	
	while(1){
		
		pthread_mutex_lock(&mutexCoda); //sostituiscilo con un wrapper che fa gestione errori 
		
		while(coda == NULL) 
			pthread_cond_wait(&condCoda, &mutexCoda); 
		
		if((fd = dequeue()) == -1){
			fprintf(stderr,"Errore dequeue, init thread\n");
			exit(1); //da modificare 
		}

		pthread_mutex_unlock(&mutexCoda);

		if(executeTask(fd) == -1)
			return -1; //In questo caso è ok? controlla pthread_create 
	}
	return (void *) 0; 
}


void *submitTask(int descr){ //PRODUTTORE
	
    pthread_mutex_lock(&mutexCoda);
	
	enqueue(descr);
	
	pthread_cond_signal(&condCoda); 
	
	pthread_mutex_unlock(&mutexCoda);
	
	return (void *) 0;
}

static void run_server(struct sockaddr *sa){
	
	int fd_skt, fd_c; 
	int fd_num = 0; //si ricorda il max fd attivo 
    int	fd; //indice per select

	//char buf[N]; 
	
	fd_set set; //fd attivi
	fd_set rdset; //fd attesi in lettura 
	
	//int nread; //num caratteri letti
	
	if((fd_skt = socket(AF_UNIX, SOCK_STREAM, 0)) == -1){
		perror("Errore nella socket");
		return -1;
	}
	
	if(bind(fd_skt, (struct sockaddr *) &sa, sizeof(sa)) == -1){
		perror("Errore nella bind");
		return -1;
	} 

	listen(fd_skt, MAX_REQ); //fai gestire errore
	
	if(fd_skt < fd_num) //mi voglio ricordare l'indice max tra i descrittori attivi
		fd_num = fd_skt;
	
	FD_ZERO(&set); //per evitare errori prima lo azzero
	FD_SET(fd_skt, &set); //metto a 1 il bit di fd_skt, fd_skt pronto
	
	for(int i = 0; i < N; i++){ //qui ci va un while(1)
		
		rdset = set; //preparo la maschera per la select, ne servono due perchè select modifica quello che gli passi
		
		if(select(fd_num+1,	&rdset, NULL, NULL, NULL) == -1){ //si blocca finchè uno tra i descrittori in rdset non si blocca più su una read
			perror("select");
			return -1;
		}
		for(fd = 0; fd <= fd_num; fd++){ //guardo tutti i descrittori che select ha modificato in rdset
			if(FD_ISSET(fd, &rdset)){ //controllo quali di questi è pronto in lettura
				if(fd == fd_skt){ //se è il listen socket faccio accept 
					fd_c = accept(fd_skt, NULL, 0);
					FD_SET(fd_c, &set); //fd_c pronto
					if(fd_c < fd_num) 
						fd_num = fd_c;
				}
				else{ //altrimenti vado subito con la read
					FD_CLR(fd, &set); //tolgo il fd da set 
					submitTask(fd);
					
					/*nread = read(fd, buf, N);
					if(nread == 0){ //client finito
						FD_CLR(fd, &set); //tolgo il fd da set 
						fd_num = aggiorna(&set);
						close(fd);
					}
					else{
						fprintf(err, "Server got: %s\n", buf);
						write(fd, "Bye", 4);
					}*/
					
					
					//MANCA LA PARTE DELLA PIPE 
				}
			}
		}
	}
}
	


int main(){
	
	pthread_t th[THREAD_NUM];
	
	pthread_mutex_init(&mutexCoda, NULL);
	pthread_cond_init(&condCoda, NULL);
	
	struct sockaddr_un sa;
	
	memset(&sa, 0, sizeof(struct sockaddr_un));
	strncpy(sa.sun_path, SOCKNAME, UNIX_PATH_MAX);
	sa.sun_family = AF_UNIX;
	
	if((hash_table = icl_hash_create(NBUCKETS, hash_pjw, string_compare)) == NULL){
		fprintf(stderr, "Errore nella creazione della hash table\n");
		return -1;
	}
	
	for(int i = 0; i < THREAD_NUM; i++){ //inizializzo pool di thread
		if(pthread_create(&th[i], NULL, &init_thread, NULL) != 0){ //creo il thread che inizia ad eseguire init_thread
			perror("Err in creazione thread");
			return -1;
		}
	}
	
	run_server(&sa); 
	
	/*for(int i = 0; i < 10; i++){ //PRODUZIONE TASK, ci andrà la select poi 
		//elm_coda t = { .x = 5 }; //produzione task 
		int fd = i;
		submitTask(fd); //sorta di enqueue 
	}*/
	
	for(int i = 0; i < THREAD_NUM; i++){ //quando terminano i thread? Quando faccio la exit? 
		if(pthread_join(th[i], NULL) != 0){ //aspetto terminino tutti i thread (la join mi permette di liberare la memoria occupata dallo stack privato del thread)
			perror("Err in join thread");
			return -1;
		}
	}
	
	pthread_mutex_destroy(&mutexCoda); //non so se vanno bene
	pthread_cond_destroy(&condCoda);
	
	return 0;
}

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
		fprintf(stderr, "Impossibile fare lock su file non esistente\n"); //me lo stampa ogni volta che creo un file, aggiustalo
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

int lockF(icl_hash_t *ht, char *path, int fd){
	file_info *tmp;	

	if((tmp = icl_hash_find(ht, path)) == NULL){ 
		fprintf(stderr, "Impossibile fare lock su file non esistente\n");
		write(fd, "Err:fileNonEsistente", 21); 
		return -1;
	}
	

	else{
		while((tmp->lock_owner != -1 && tmp->lock_owner != fd)){
			sleep(1); //attesa attiva??
		}
		tmp->lock_owner = fd;
		tmp->lst_op = 0; //vedi se crea problemi alla openfile(O_create|o_lock)
		write(fd, "Ok", 3);	
	}
	
	#ifdef DEBUG
		fprintf(stderr,"---LOCK FILE \n");
		print_deb(ht);
	#endif
		
	
	return 0;

}

int unlockF(icl_hash_t *ht, char *path, int fd){
	file_info *tmp;
	
	if((tmp = icl_hash_find(ht, path)) == NULL){ 
		fprintf(stderr, "Impossibile fare lock su file non esistente\n");
		write(fd, "Err:fileNonEsistente", 21); 
		return -1;
    }

	else if(tmp->lock_owner != fd){
		fprintf(stderr, "Impossibile fare unlock su file di cui non si ha la lock\n");
		write(fd, "Err:Nolock", 21); 
		return -1;
	}
	else{
		tmp->lock_owner = -1;
		tmp->lst_op = 0;
		write(fd, "Ok", 3);
	}
	#ifdef DEBUG
		fprintf(stderr,"---UNLOCK FILE \n");
		print_deb(ht);
	#endif
		
	
	return 0;
}

int appendToF(icl_hash_t *ht, char *path, char *cnt, int fd){
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
	else if(tmp->lock_owner != fd){
			fprintf(stderr, "Impossibile scrivere su file senza lock\n");
			write(fd, "Err:fileSenzaLock", 18); 
			return -1;
		}
	else{
		if(cnt != NULL)
			if(tmp->cnt == NULL){
				tmp->cnt = malloc(MSG_SIZE*sizeof(char));
				strcpy(tmp->cnt, cnt);
			}
			else
				strcat(tmp->cnt, cnt);
		else
			fprintf(stderr, "richiesta scrittura di file vuoto\n");
		
		write(fd, "Ok", 3);			
	}
	#ifdef DEBUG
		fprintf(stderr,"---APPEND TO FILE \n");
		print_deb(ht);
	#endif
		
	
	return 0;
}

int removeF(icl_hash_t *ht, char *path, int fd){ //controlla di aver liberato bene la memoria
	file_info *tmp;
	
	if((tmp = icl_hash_find(ht, path)) == NULL){ 
		fprintf(stderr, "Impossibile rimuovere file non esistente\n");
		write(fd, "fail:fileNonEsistente", 22); 
		return -1;
	}
	else if(tmp->lock_owner != fd){
		fprintf(stderr, "Impossibile rimuovere file senza lock\n");
		write(fd, "fail:fileSenzaLock", 19); 
		return -1;
	}
	else{
		if(icl_hash_delete(ht, path) == -1){
			fprintf(stderr, "Errore nella rimozione del file dallo storage\n");
			write(fd, "Err:rimozione", 14);
			return -1;
		}
		
		
		write(fd, "Ok", 3);
		
		#ifdef DEBUG
			fprintf(stderr,"---REMOVE FILE\n");
			print_deb(ht);
		#endif 
	}
	return 0;
}

int readAF(icl_hash_t *ht, int fd, int seed){
    char *msg = malloc(MSG_SIZE*sizeof(char));	
	    
	icl_entry_t *entry;
	char *key, *value;
	int k;
	int found = 0;
	srand(seed); //da aggiustare 
	int num = NBUCKETS;
	if (ht) {
		file_info *aux;
		while(!found && num > 0){
			k = rand()%NBUCKETS; //così non è detto io li controlli tutti
			for (entry = ht->buckets[k]; !found && entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL); entry = entry->next){
				//openFL(key);
				aux = icl_hash_find(ht, key);
				//closeF(key);
				found = 1;
				strcpy(msg, "Ok;"); //vedi se sovrascrive
				strncat(msg, key, MSG_SIZE - strlen(msg) - 1);
				if(aux != NULL){
					if(aux->cnt != NULL){
						strncat(msg, ";", MSG_SIZE - strlen(msg) - 1);
						strncat(msg, aux->cnt, MSG_SIZE - strlen(msg) - 1);
					}
				}
				
				write(fd, msg, strlen(msg)+1);
			}
			num--;
		}
	}
	else{
		fprintf(stderr, "hash table vuota\n");
		write(fd, "Err", 4);
	}
		
	
	
	#ifdef DEBUG
		fprintf(stderr,"---SEND RANDOM FILE %s \n", msg);
		print_deb(ht);
	#endif
	if(num == 0){
		fprintf(stderr, "Nessun file trovato\n");
		write(fd, "Err", 4);
	}
	free(msg);
	
	return 0;			
}

int readAllF(icl_hash_t *ht, int fd){
    char *msg = malloc(MSG_SIZE*sizeof(char));	
	    
	icl_entry_t *entry;
	char *key, *value;
	int go = 1;
	int k;
	
	if (ht) {
		file_info *aux;
		for (k = 0; k < ht->nbuckets && go; k++)  {
			for (entry = ht->buckets[k]; go && entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL); entry = entry->next){
				//openFL(key);
				aux = icl_hash_find(ht, key);
				//closeF(key);
				strcpy(msg, "Ok;"); //vedi se sovrascrive
				strncat(msg, key, MSG_SIZE - strlen(msg) - 1);
				if(aux != NULL){
					if(aux->cnt != NULL){
						strncat(msg, ";", MSG_SIZE - strlen(msg) - 1);
						strncat(msg, aux->cnt, MSG_SIZE - strlen(msg) - 1);
					}
				}
				write(fd, msg, strlen(msg)+1);
					
				if(read(fd, msg, MSG_SIZE) == -1){
					perror("errore lettura");
					free(msg);
					return -1;
				}
				if(strcmp(msg, "Ok") != 0)
					go = 0;
			}
		}
	}
	else{
		fprintf(stderr, "hash table vuota\n");
		write(fd, "Err", 4);
	}
	write(fd, "STOP", 5);	
	
	#ifdef DEBUG
		fprintf(stderr,"---SEND ALL FILES %s \n", msg);
		print_deb(ht);
	#endif
	
	free(msg);
	
	return 0;			
}

