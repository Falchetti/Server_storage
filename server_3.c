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
#include "queue.h"

#define DEBUG
//#undef DEBUG

#define UNIX_PATH_MAX 108 //lunghezza massima path
#define MSG_SIZE  230 //path + spazio comandi 
#define MAX_SIZE 230 //path, comandi, contenuto file
#define CNT_SIZE 100

#define SOCKNAME "/home/giulia/Server_storage/mysock" 
#define NBUCKETS  20 // n. di buckets nella tabella hash 
#define ST_DIM 256 //capacità storage  
#define THREAD_NUM 1
#define LOGNAME "/home/giulia/Server_storage/log"

#define MAX_REQ   20 //n. max di richieste in coda 
#define N 60



//ricordati di pensare alle lock su l'hashtable 

typedef struct open_node{
    int fd;
    struct open_node *next;
} open_node;

typedef struct file_node{
	char *path;
	int cnt_sz;
	char cnt[MAX_SIZE];
	struct file_node *next;
} file_node;

typedef struct file_info{ 
	int lock_owner;
	open_node *open_owners; 
	int lst_op; //controllo per la writeFile
	char cnt[MAX_SIZE]; //era char *
	int cnt_sz; //non c'era 
} file_info;

typedef struct file_entry_queue{
	file_info *pnt;
	char *path;
} file_entry_queue;


void print_deb(icl_hash_t *ht);
int isNumber(void *el, int *n);
int st_repl(int dim, int fd, char *path, file_node **list);
int file_sender(file_node *list, int fd);

file_info *init_file(int lock_owner, int fd, int lst_op, char *cnt, int sz);

int insert_head(open_node **list, int info);
int remove_elm(open_node **list, int info);
void print_list(open_node *list);
int insert_head_f(file_node **list, char *path, int sz_cnt, char *cnt); //potrei usare delle funz generiche

int openFC(icl_hash_t *ht, char *path, int fd);
int openFL(icl_hash_t *ht, char *path, int fd);
int openFO(icl_hash_t *ht, char *path, int fd);
int openFCL(icl_hash_t *ht, char *path, int fd);
int closeF(icl_hash_t *ht, char *path, int fd);
int readF(icl_hash_t *ht, char *path, int fd);
int writeF(icl_hash_t *ht, char *path, char *cnt, int sz, int fd);
int lockF(icl_hash_t *ht, char *path, int fd);
int unlockF(icl_hash_t *ht, char *path, int fd);
int appendToF(icl_hash_t *ht, char *path, char *cnt, int sz, int fd);
int removeF(icl_hash_t *ht, char *path, int fd);
int readNF(icl_hash_t *ht, int fd, int n);

/*
typedef struct elm_coda{
	int fd;
	struct elm_coda *next;
} elm_coda;

elm_coda *task_coda= NULL;
pthread_mutex_t mutexCoda;
pthread_cond_t condCoda;*/


//variabili globali 
Queue_t *task_queue;
Queue_t *file_queue;

icl_hash_t *hash_table = NULL;

int st_dim; 
int n_files;
int st_dim_MAX;
int n_files_MAX;


int executeTask(int fd_c){ //qui faccio la read, capisco cosa devo fare, lo faccio chiamando la giusta funzione, rispondo al client, scrivo fd sulla pipe per il server  
	
	char *msg = malloc(MSG_SIZE*sizeof(char)); //controlla la malloc 
	char *tmpstr, *token, *token2;
	//int seed = time(NULL);
	int n;
		
	if(read(fd_c, msg, MSG_SIZE) == -1){
		perror("read socket lato server");
		return -1; //ok?
	}
	
	#ifdef DEBUG
		fprintf(stderr, "len queue: %d\n", (int) length(file_queue));
		fprintf(stderr, "N_FILES: %d / %d, ST_DIM: %d / %d\n", n_files, n_files_MAX, st_dim, st_dim_MAX);
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
			#ifdef DEBUG
				fprintf(stderr, "QUEUE INFO:\nlen: %d\nElm:\n", (int) length(file_queue));
				int n = length(file_queue);
				for(int i = 0; i < n; i++){
					file_entry_queue *tmp = pop(file_queue);
					fprintf(stderr, "path: %s, sz_cnt: %d\ncnt:\n", tmp->path, tmp->pnt->cnt_sz); 
					if(tmp->pnt->cnt_sz > 0){
						fwrite(tmp->pnt->cnt, tmp->pnt->cnt_sz, 1, stdout);
					}
					else
						fprintf(stderr, "//\n");
						
				}
			#endif
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
			if(isNumber(token2, &n) == 0 && n > 0){
				write(fd_c, "Ok", 3); //gestione errore 
				if(read(fd_c, token2, n) == -1){
					perror("read server su write client");
					return -1;
				}
			}
			else
				token2 = NULL;
				
			if(writeF(hash_table, token, token2, n, fd_c) == -1){
				return -1;
			}			
		}
		else if(strcmp(token, "lockf") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			int res = lockF(hash_table, token, fd_c);
			switch(res){
				case -1: write(fd_c, "Err:fileNonEsistente", 21); 
					return -1;
				case 0: write(fd_c, "Ok", 3);	
					break;
				default: fprintf(stderr, "Errore, return from lockF");
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
			if(isNumber(token2, &n) == 0 && n > 0){
				write(fd_c, "Ok", 3); //gestione errore 
				if(read(fd_c, token2, n) == -1){
					perror("read server su appendto client");
					return -1;
				}
			}
			else
				token2 = NULL;
				
			if(appendToF(hash_table, token, token2, n, fd_c) == -1){
				return -1;
			}			
		}
		else if(strcmp(token, "removef") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(removeF(hash_table, token, fd_c) == -1){
				return -1;
			}
		}
		
		else if(strcmp(token, "readNf") == 0){
			token2 = strtok_r(NULL, ";", &tmpstr);
			if(isNumber(token2, &n) == 0){
				if(readNF(hash_table, fd_c, n) == -1)
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
	
	
	return 0;
}

void *init_thread(void *args){ //CONSUMATORE
	
	while(1){
		
		int  *fd;		
		if((fd = pop(task_queue)) == NULL){
			fprintf(stderr,"Errore pop, init thread\n");
			exit(1); //da modificare 
		}
		if(executeTask(*fd) == -1){
			free(fd);
			return (void *) -1; //In questo caso è ok? controlla pthread_create 
		}
		//fprintf(stderr, "scrivo sulla pipe: %d fd : %d\n", *((int*)args), fd);
		write(*((int*)args), fd, 2); //gestione errore scrittura su pipe 
	
	     free(fd);
	}
	return (void *) 0; 
}


void *submitTask(int *descr){ //PRODUTTORE
	
    if (push(task_queue, descr) == -1) {
	    fprintf(stderr, "Errore: push\n");
	    pthread_exit(NULL); //controlla se va bene 
	}
	
	return (void *) 0;
	
}

int aggiorna_max(int fd_max, fd_set set){
	
    for(int i = fd_max - 1; i >= 0; i--)
        if (FD_ISSET(i, &set)) return i;
 
    return -1;
}

int run_server(struct sockaddr_un *sa, int pipe){
	
	int fd_skt, fd_c, fd_r = 0;
	int fd_num = 0; //si ricorda il max fd attivo 
    int	fd; //indice per select


	fd_set set; //fd attivi
	fd_set rdset; //fd attesi in lettura 
	
    //inizializzazione socket 
	
	if((fd_skt = socket(AF_UNIX, SOCK_STREAM, 0)) == -1){
		perror("Errore nella socket");
		return -1;
	}
	
	if(bind(fd_skt, (struct sockaddr *) sa, sizeof(*sa)) == -1){
		perror("Errore nella bind");
		return -1;
	} 
	listen(fd_skt, MAX_REQ); //fai gestire errore

	//aggiorno fd_num e set 
	
	if(fd_skt > fd_num) //mi voglio ricordare l'indice max tra i descrittori attivi
		fd_num = fd_skt;
		
	if(pipe > fd_num)
		fd_num = pipe;
	
	FD_ZERO(&set); //per evitare errori prima lo azzero
	FD_SET(fd_skt, &set); //metto a 1 il bit di fd_skt, fd_skt pronto
	
	FD_SET(pipe, &set);
	
	
	//inizio il ciclo di comunicazione con i workers 
	
	for(int i = 0; i < N; i++){ //qui ci va un while(1)
		//fprintf(stderr, "ITERAZIONE: i: %d, pipe: %d, skt: %d, fd_num: %d, nuovo fd_r: %d \n", i, pipe, fd_skt, fd_num, fd_r);
		
		rdset = set; //aggiorno ogni volta la maschera per la select, ne servono due perchè select modifica quello che gli passi
		
		if(select(fd_num+1,	&rdset, NULL, NULL, NULL) == -1){ //si blocca finchè uno tra i descrittori in rdset non si blocca più su una read
			perror("select");
			return -1;
		}
		
		//appena la select mi dice che c'è un worker pronto (descrittore client libero), inizio a scorrerli
		
		for(fd = 0; fd <= fd_num; fd++){ //guardo tutti i descrittori nel nuovo rdset (aggiornato da select)
			
			if(FD_ISSET(fd, &rdset)){ //controllo quali di questi è pronto in lettura
			   // fprintf(stderr, "-----------fd: %d è pronto\n", fd);
				
				if(fd == fd_skt){ //se è il listen socket faccio accept 
					//fprintf(stderr, "ho letto il SOCKET, faccio accept\n");
					fd_c = accept(fd_skt, NULL, 0);
				    //fprintf(stderr, "ACCEPT fatta, %d è il fd_c del client\n", fd_c);
				   
					FD_SET(fd_c, &set); //fd_c pronto
					if(fd_c > fd_num) 
						fd_num = fd_c;
					}
				else{ //(altrimenti vado con la read)
				
					if(fd == pipe){ //se è la pipe aggiorno i descrittori

					//fprintf(stderr, "ho letto il fd: %d di PIPE, faccio read\n", fd);					
						
						if(read(pipe, &fd_r, 2) != -1){ //fd_r è di nuovo libero, aggiorno set e fd_num
							FD_SET(fd_r, &set);
							if(fd_r > fd_num) 
								fd_num = fd_r;
						}
						else{
							perror("READ PIPE MASTER: %d");
							return -1;
						}
						//fprintf(stderr, "read su pipe fatta, ho letto fd_r: %d\n", fd_r);
						
					}
					else{ //altrimenti eseguo il comando richiesto dal descrittore 
						
						//fprintf(stderr, "ho letto un descrittore client fd: %d, faccio SUBMIT\n", fd);
						int *data = malloc(sizeof(int));
						if (data == NULL) {
							perror("Producer malloc");
							pthread_exit(NULL);
						}
						*data = fd;
						submitTask(data);
						FD_CLR(fd, &set); //tolgo il fd da set 
						if(fd == fd_num) {
							if((fd_num = aggiorna_max(fd, set)) == -1){
								return -1; //???
							}
		
						}
						//devo chiudere fd? (close(fd));
					}				
					
				}
			}
		}
	}
	
	return 0;
}
	
//nb: poi devi CHIUDERE la pipe 

int main(int argc, char *argv[]){
	
	char *socket, *log, *aux, *tmpstr, *token;
	int n_workers, n;
	FILE *fp;
	
	//valori di default
	socket = SOCKNAME;
	log = LOGNAME;
	n_workers = THREAD_NUM;
	n_files_MAX = NBUCKETS;
	st_dim_MAX = ST_DIM;
	
	//parsing file di configurazione 
	
	if((getopt(argc, argv, "k:")) == 'k'){ 
		if((fp = fopen(optarg, "r")) == NULL){
			perror("errore nella fopen"); //devo controllare se va a buon fine SI
            return -1;
        }
		aux = malloc(MAX_SIZE*sizeof(char));
		
        while (fgets(aux, MAX_SIZE, fp)){ //dovresti fare gestione errore di fgets
			token = strtok_r(aux, ":", &tmpstr);
			if(strcmp(token, "NFILES") == 0){
				token = strtok_r(NULL, ":", &tmpstr);
				token[strlen(token) - 2] = '\0'; //brutto, vedi se lo puoi migliorare (non voglio considerare il \n)
				if(isNumber(token, &n) == 0 && n > 0)
					n_files_MAX = n;
				else{
					fprintf(stderr, "errore in config file: nfiles_max value errato, verrà usato valore di default\n");
				}
			}
			else if(strcmp(token, "DIM") == 0){
				token = strtok_r(NULL, ":", &tmpstr);
				token[strlen(token) - 2] = '\0'; //brutto, vedi se lo puoi migliorare (non voglio considerare il \n)
				if(isNumber(token, &n) == 0 && n > 0)
					st_dim_MAX = n;
				else{
					fprintf(stderr, "errore in config file: st_dim_max value errato, verrà usato valore di default\n");
				}
			}
			else if (strcmp(token, "NWORKERS") == 0){
				token = strtok_r(NULL, ":", &tmpstr);
				token[strlen(token) - 2] = '\0'; //brutto, vedi se lo puoi migliorare (non voglio considerare il \n)
				if(isNumber(token, &n) == 0 && n > 0)
					n_workers = n;
				else{
					fprintf(stderr, "errore in config file: nworkers value errato, verrà usato valore di default\n");
				}
			}
			else if (strcmp(token, "SOCKET") == 0){
				token = strtok_r(NULL, ":", &tmpstr);
				if(token != NULL){
					socket = malloc(UNIX_PATH_MAX*sizeof(char));
					strcpy(socket, token); //usa quello con n 
					socket[strlen(socket)-2] = '\0'; //brutto, vedi se lo puoi migliorare (non voglio considerare il \n)
				}
				else{
					fprintf(stderr, "errore in config file: socket value errato, verrà usato valore di default\n");
				}			
			}
			else if (strcmp(token, "LOG") == 0){
				token = strtok_r(NULL, ":", &tmpstr);
				if(token != NULL){
					log = malloc(UNIX_PATH_MAX*sizeof(char));
					strcpy(log, token); //usa quello con n 
					log[strlen(socket)-2] = '\0'; //brutto, vedi se lo puoi migliorare (non voglio considerare il \n)
				}
				else{
					fprintf(stderr, "errore in config file: log value errato, verrà usato valore di default\n");
				}
			}
			else
				fprintf(stderr, "formato config file errato, verranno usati valori di default sui dati mancanti\n");
		}
		
		free(aux);
		fclose(fp);
		
	}
	
	//inizializzo var storage files
	st_dim = 0;
	n_files = 0;
	
	//Inizializzazione thread pool 
	pthread_t th[n_workers];
	
	//pthread_mutex_init(&mutexCoda, NULL);
	//pthread_cond_init(&condCoda, NULL);
	//stanno in initqueue
	
	//inizializzazione pipe
	int pfd[2];
	
	if(pipe(pfd) == -1){
		perror("creazione pipe");
		return -1;
	}
	
	//inizializzazione socket 
	struct sockaddr_un sa;
	
	memset(&sa, 0, sizeof(struct sockaddr_un));
	strncpy(sa.sun_path, socket, UNIX_PATH_MAX);

	sa.sun_family = AF_UNIX;
	
	//inizializzazione hash table 
	
	if((hash_table = icl_hash_create(n_files_MAX, hash_pjw, string_compare)) == NULL){
		fprintf(stderr, "Errore nella creazione della hash table\n");
		return -1;
	}
	
	//inizializzazione task_queue
	task_queue = initQueue();
    if (!task_queue) {
		fprintf(stderr, "initQueue su tsk_q fallita\n");
		exit(errno); //controlla
    }
	
	//inizializzazione file_queue
	file_queue = initQueue();
    if (!file_queue) {
		fprintf(stderr, "initQueue su file_q fallita\n");
		exit(errno); //controlla
    }
	
	//creazione e avvio threadpool 
	for(int i = 0; i < n_workers; i++){
		if(pthread_create(&th[i], NULL, &init_thread, (void *) &pfd[1]) != 0){ //creo il thread che inizia ad eseguire init_thread
			perror("Err in creazione thread");
			return -1;
		}
	}
	//avvio server 
	run_server(&sa, pfd[0]); 

   //join 
	for(int i = 0; i < n_workers; i++){ //quando terminano i thread? Quando faccio la exit? 
		if(pthread_join(th[i], NULL) != 0){ //aspetto terminino tutti i thread (la join mi permette di liberare la memoria occupata dallo stack privato del thread)
			perror("Err in join thread");
			return -1;
		}
	}
	
	//eliminazione elm thread 
	//pthread_mutex_destroy(&mutexCoda); //non so se vanno bene
	//pthread_cond_destroy(&condCoda);
	//sostituiti con: 
	//eliminazione task_queue  
	
	
	deleteQueue(task_queue);
	deleteQueue(file_queue);
	
	//assicurati tutte le connessioni siano chiuse sul socket prima diterminare
	//finora chiudo la connessione quando ricevo il mess "disconnesso" dal client
	
	return 0;
}

file_info *init_file(int lock_owner, int fd, int lst_op, char *cnt, int sz){
	file_info *tmp = malloc(sizeof(file_info)); 

		
	open_node *client;
    client = malloc(sizeof(open_node));	
	client->fd = fd;	
	client->next = NULL;
	
	tmp->open_owners = client;
		tmp->lock_owner = lock_owner;
	tmp->lst_op = lst_op;
	
	if(sz > 0)
		memcpy(tmp->cnt, cnt, sz); //controlla 

	return tmp;
}

int insert_head_f(file_node **list, char *path, int cnt_sz, char *cnt){
	
	file_node *tmp = malloc(sizeof(file_node));
	tmp->path = path;
	tmp->cnt_sz = cnt_sz;
   
	if (cnt_sz > 0)
		memcpy(tmp->cnt, cnt, cnt_sz);
	
	tmp->next = *list;
	
	*list = tmp;
	
	return 0;
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
					
				if(aux->cnt != NULL){
					fprintf(stderr, "cnt_sz: %d\n", aux->cnt_sz);						
					fwrite(aux->cnt, aux->cnt_sz, 1, stderr);
					fprintf(stderr, "\n");
				}
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
		data = init_file(-1, fd, 0, NULL, -1); 
		if(st_repl(0, fd, path, NULL) == -1)
				fprintf(stderr, "Errore in repl\n");

		if(icl_hash_insert(ht, path, (void *) data) == NULL){
			fprintf(stderr, "Errore nell'inseriemento del file nello storage\n");
			write(fd, "Err:inserimento", 16);
			return -1;
		}
		n_files++; //serve una lock
		
		file_entry_queue *qdata = malloc(sizeof(file_entry_queue));
		qdata->pnt = data;
		qdata->path = path;
		if (push(file_queue, qdata) == -1) {
			fprintf(stderr, "Errore: push\n");
			pthread_exit(NULL); //controlla se va bene 
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
			data = init_file(fd, fd, 1, NULL, -1); 
		
		    if(st_repl(0, fd, path, NULL) == -1)
				fprintf(stderr, "Errore in repl\n");
			fprintf(stderr, "BBBBBBBBB\n");
			if(icl_hash_insert(ht, path, data) == NULL){
				fprintf(stderr, "Errore nell'inseriemento del file nello storage\n");
				write(fd, "Err:inserimento", 16);
				return -1;
			}	
			
			
			write(fd, "Ok", 3);	
			n_files++; //lock
			file_entry_queue *qdata = malloc(sizeof(file_entry_queue));
			qdata->pnt = data;
			qdata->path = path;
			if (push(file_queue, qdata) == -1) {
				fprintf(stderr, "Errore: push\n");
				pthread_exit(NULL); //controlla se va bene 
			}
			
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
    char str[MAX_SIZE/10 +2];
	
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
		//strcpy(msg, "Ok"); //vedi se sovrascrive
		snprintf(str, MAX_SIZE/10 +2, "%d", tmp->cnt_sz);
		write(fd, str, MSG_SIZE);
		if(tmp->cnt_sz > 0)
		   write(fd, tmp->cnt, tmp->cnt_sz); //ha senso il terzo parametro? 
	}
	
	#ifdef DEBUG
		fprintf(stderr,"---READ FILE \n");
		print_deb(ht);
	#endif
	//free(msg);
	
	return 0;
			
}

int writeF(icl_hash_t *ht, char *path, char *cnt, int sz, int fd){
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
			file_node *list = NULL;

			int res = st_repl(sz, fd, path, &list);
			
			if(res == -1)
				fprintf(stderr, "Errore in repl su WriteFile\n");
			if(res == -2){
				write(fd, "Err:fileTroppoGrande", 21);
				return 0; //vedi poi come modificare dato che non vuoi interrompere la lettura per questo 
			}
			
			#ifdef DEBUG
				file_node *aux2 = list;
				fprintf(stderr, "LISTA ESPULSI: ");
				while(aux2 != NULL){
					fprintf(stderr, "%s -> ", aux2->path);
					aux2 = aux2->next;
				}
				fprintf(stderr, "//\n");
			#endif
			file_node *aux = list;
				
			while(aux != NULL){
				file_sender(aux, fd);
				aux = aux->next;
			}
            
			//dovrò liberarla questa lista 
			
			if(sz > 0){
				memcpy(tmp->cnt, cnt, sz);
				st_dim = st_dim - tmp->cnt_sz + sz; //lock 
			    tmp->cnt_sz = sz;
			}
		}
		else
			fprintf(stderr, "richiesta scrittura di file vuoto\n");
		write(fd, "$", 2);
	//	fprintf(stderr, "write server: $\n");
		char *msg = malloc(MSG_SIZE*sizeof(char));
		read(fd, msg, MSG_SIZE);
	//	fprintf(stderr, "read server dopo $: %s\n", msg);
		
		write(fd, "Ok", 3);
		//fprintf(stderr, "write server: OK\n");
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
		return -1;
	}
	

	else{
		while((tmp->lock_owner != -1 && tmp->lock_owner != fd)){
			sleep(1); //attesa attiva??
		}
		tmp->lock_owner = fd;
		tmp->lst_op = 0; //vedi se crea problemi alla openfile(O_create|o_lock)
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

int appendToF(icl_hash_t *ht, char *path, char *cnt, int sz, int fd){
	file_info *tmp;	
	int res;
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
		if(cnt != NULL){
			file_node *list = NULL;
			res = st_repl(sz, fd, path, &list);
			
				
			if(res == -1)
				fprintf(stderr, "Errore in repl su AppendToFile\n");
			else if(res == -2){
				write(fd, "Err:fileTroppoGrande", 21); 
				return -1;
			}
			else{
				#ifdef DEBUG
					file_node *aux2 = list;
					fprintf(stderr, "LISTA ESPULSI: ");
					while(aux2 != NULL){
						fprintf(stderr, "%s -> ", aux2->path);
						aux2 = aux2->next;
					}
					fprintf(stderr, "NULL\n");
				#endif
				file_node *aux = list;
				while(aux != NULL){
					file_sender(aux, fd);
					aux = aux->next;
				}
				write(fd, "$", 2);
			}
			if(tmp->cnt_sz > 0){ //prima era >= 
				memcpy(tmp->cnt + tmp->cnt_sz, cnt, sz);
				tmp->cnt_sz += sz;
				st_dim +=sz; //lock
			}
				
			
			 
	    }
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
	int old_sz;
	
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
		old_sz = tmp->cnt_sz;
		
		//aggiorno queue file
		int len_q = length(file_queue);
		file_entry_queue *aux[len_q];
		file_entry_queue *del;
		int j = 0;
		int found = 0;
		
		for(int i = 0; i < len_q; i++){
			del = pop(file_queue);			
			//attenzione omonimi 
			if(strncmp(del->path, path, UNIX_PATH_MAX) == 0){
				free(del);
				found = 1;
			}
			else{
				aux[j] = del;
				j++;
			}
		}
		for(int i = 0; i < len_q-1; i++)
			push(file_queue, aux[i]);
		if(!found)
			push(file_queue, aux[len_q]);
		
		
		//NB: creare funz che liberano memoria!!
		if(icl_hash_delete(ht, path, NULL, NULL) == -1){ 
			fprintf(stderr, "Errore nella rimozione del file dallo storage\n");
			write(fd, "Err:rimozione", 14);
			return -1;
		}
		
		n_files--; //lock
		st_dim -= old_sz; //lock 
		
		
		write(fd, "Ok", 3);
		
		#ifdef DEBUG
			fprintf(stderr,"---REMOVE FILE\n");
			print_deb(ht);
		#endif 
	}
	return 0;
}


//queste chiamate mi sa che non devono necessariamente aprire e chiudere il file 
int readNF(icl_hash_t *ht, int fd, int n){
	    
	icl_entry_t *entry;
	char *key, *value;
	int k;
	int all = 0;
	file_node *file;
	
	
	if(n <= 0){
		all = 1;
		n = 1;
	}
	if (ht) {
		file_info *aux;
		for (k = 0; k < ht->nbuckets && n > 0; k++)  {
			for (entry = ht->buckets[k]; entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL) && n > 0; entry = entry->next){
				//openFL(key);
				aux = icl_hash_find(ht, key);
				file = malloc(sizeof(file_node));
				file->path = key;
				file->cnt_sz = aux->cnt_sz;
				memcpy(file->cnt, aux->cnt, file->cnt_sz);
				
				file_sender(file, fd);//controlla risultato 
				
				if(!all)
					n--;
					
			}
		}
	}
	else{
		fprintf(stderr, "hash table vuota\n");
	}
    
	write(fd, "$", 2);
   // fprintf(stderr, "write 3 server\n");
    char *msg = malloc(MSG_SIZE*sizeof(char));
	read(fd, msg, MSG_SIZE);
	//	fprintf(stderr, "read server dopo $: %s\n", msg);
		
	write(fd, "Ok", 3);
	//fprintf(stderr, "write server: OK\n");
	
	
	
	#ifdef DEBUG
		fprintf(stderr,"---SEND N FILES\n");
		print_deb(ht);
	#endif
	
	
	return 0;			
}

int st_repl(int dim, int fd, char *path, file_node **list){
	file_entry_queue *exp_file;
	
	int res = 0;
	int old_sz;
	
	if(dim > st_dim_MAX || st_dim_MAX == 0){
		fprintf(stderr, "Non è possibile rimpiazzare file, %d > %d\n", dim, st_dim_MAX);
		return -2;
    }
	
	if(n_files + 1 > n_files_MAX){
		if((exp_file = pop(file_queue)) == NULL){
			perror("OpenFC, pop");
			return -1;
		}

		
		fprintf(stderr, "FILE DA RIMUOVERE: %s, len_q: %ld\n", exp_file->path, length(file_queue));
		if(lockF(hash_table, exp_file->path, fd) == -1){
			fprintf(stderr, "Errore in repl: lockF");
			return -1;
		}
		
		
        /*if(icl_hash_find(hash_table, exp_file->path) == NULL){ 
			fprintf(stderr, "Impossibile rimuovere file non esistente\n");
			return -1;
		}
		else if(exp_file->pnt->lock_owner != fd){
			fprintf(stderr, "Impossibile rimuovere file senza lock\n");
			return -1;
		}*/
		//ci pensa lui ad aggiornare st_dim e num_file	
		
		old_sz = exp_file->pnt->cnt_sz;
		
		
		if(list != NULL)
			insert_head_f(list, exp_file->path, old_sz, exp_file->pnt->cnt);
		
		//NB: creare funz che liberano memoria!!
		if(icl_hash_delete(hash_table, exp_file->path, NULL, NULL) == -1){ 
			fprintf(stderr, "Errore nella rimozione del file dallo storage, inrepl\n");
			return -1;
		}
		
		n_files--; //lock
		st_dim = st_dim - old_sz; //lock 
		fprintf(stderr, "n_files: %d, st_dim: %d\n", n_files, st_dim);
		
		
			
	}
	
	while(st_dim + dim > st_dim_MAX && length(file_queue) > 0){
		if((exp_file = pop(file_queue)) == NULL){
			perror("OpenFC, pop");
			return -1;
		}
		if(strncmp(exp_file->path, path, UNIX_PATH_MAX) == 0)
			continue;
		
			
		fprintf(stderr, "FILE DA RIMUOVERE: %s, len_q: %ld\n", exp_file->path, length(file_queue));
		
		if(lockF(hash_table, exp_file->path, fd) == -1){
			fprintf(stderr, "Errore in repl: lockF");
			return -1;
		}
        //copiato parte di removeF per non riaggiornare la queue 	
		
	
		/*if(icl_hash_find(hash_table, exp_file->path) == NULL){ 
			fprintf(stderr, "Impossibile rimuovere file non esistente\n");
			return -1;
		}
		else if(exp_file->pnt->lock_owner != fd){
			fprintf(stderr, "Impossibile rimuovere file senza lock\n");
			return -1;
		}*/
		old_sz = exp_file->pnt->cnt_sz;
		if(list != NULL)
			insert_head_f(list, exp_file->path, old_sz, exp_file->pnt->cnt);
		
		//NB: creare funz che liberano memoria!!
		if(icl_hash_delete(hash_table, exp_file->path, NULL, NULL) == -1){ 
			fprintf(stderr, "Errore nella rimozione del file dallo storage, in repl\n");
			return -1;
		}
		
		n_files--; //lock
		st_dim = st_dim - old_sz; //lock 
	
	}
		
	if(st_dim + dim > st_dim_MAX){
		fprintf(stderr, "Non è possibile rimpiazzare file\n");
		return -2;
    }
		
	return res;
}

int file_sender(file_node *file, int fd){
		
	//considera opzione file null
	char *msg = malloc(MSG_SIZE*sizeof(char));	
	char str[MAX_SIZE/10+2]; //cambiare
	
	strncpy(msg, file->path, UNIX_PATH_MAX);
	strncat(msg, ";", MSG_SIZE - strlen(msg) - 1);
	snprintf(str, MAX_SIZE/10+2, "%d", file->cnt_sz);
	strncat(msg, str, MSG_SIZE - strlen(msg) - 1);

	write(fd, msg, MSG_SIZE);
	//fprintf(stderr, "write path;sz server: %s\n", msg);
	if(file->cnt_sz > 0){
		if(read(fd, msg, MSG_SIZE) == -1){
			perror("errore lettura");
			free(msg);
			return -1;
		}
		//fprintf(stderr, "read (cnt_sz > 0) server: %s\n", msg);
		if(strncmp(msg, "Ok", 3) == 0){
			write(fd, file->cnt, file->cnt_sz);
			//fprintf(stderr, "write cnt server\n");
		}
	}


	if(read(fd, msg, MSG_SIZE) == -1){
		perror("errore lettura");
		free(msg);
		return -1;
	}
	//fprintf(stderr, "read dopo path;sz o cnt server: %s\n", msg);
	if(strncmp(msg, "Ok", 3) != 0)
		fprintf(stderr, "Errore in fileFile sender mess risposta client\n");	

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