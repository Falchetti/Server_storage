#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>

#include "icl_hash.h"
#include "queue.h"

#define DEBUG
#undef DEBUG

#define UNIX_PATH_MAX 108 //lunghezza massima path
#define MAX_SIZE 10000 

#define SOCKNAME "/home/giulia/Server_storage/mysock" 
#define NBUCKETS  20 // n. di file max nello storage 
#define ST_DIM 256 //capacità storage  
#define THREAD_NUM 1
#define LOGNAME "/home/giulia/Server_storage/log" 

#define MAX_REQ   20 //n. max di richieste in coda 

//vedi se usare exit o pthread exit, nel caso di malloc uccido tutto il programma o solo il thread?


//    FUNZIONI PER MUTUA ESCLUSIONE -> Versione vista a lezione   //
static void Pthread_mutex_lock (pthread_mutex_t* mtx)
{
    int err;
    if ((err = pthread_mutex_lock(mtx)) != 0 )
    {
        errno = err;
        perror("lock");
        pthread_exit((void*)&errno);
    }
}

#define LOCK(l)      if (Pthread_mutex_lock(l)!=0)        { \
    fprintf(stderr, "ERRORE FATALE lock\n");		    \
    pthread_exit((void*)EXIT_FAILURE);			    \
  } 
static void Pthread_mutex_unlock (pthread_mutex_t* mtx)
{
    int err;
    if ((err = pthread_mutex_unlock(mtx)) != 0 )
    {
        errno = err;
        perror("unlock");
        pthread_exit((void*)&errno);
    }
}

/** Evita letture parziali
 *
 *   \retval -1   errore (errno settato)
 *   \retval  0   se durante la lettura da fd leggo EOF
 *   \retval size se termina con successo
 */
static inline int readn(long fd, void *buf, size_t size) {
    size_t left = size;
    int r;
    char *bufptr = (char*)buf;
    while(left>0) {
	if ((r=read((int)fd ,bufptr,left)) == -1) {
	    if (errno == EINTR) continue;
	    return -1;
	}
	if (r == 0) return 0;   // EOF
        left    -= r;
	bufptr  += r;
    }
    return size;
}

/** Evita scritture parziali
 *
 *   \retval -1   errore (errno settato)
 *   \retval  0   se durante la scrittura la write ritorna 0
 *   \retval  1   se la scrittura termina con successo
 */
static inline int writen(long fd, void *buf, size_t size) {
    size_t left = size;
    int r;
    char *bufptr = (char*)buf;
    while(left>0) {
	if ((r=write((int)fd ,bufptr,left)) == -1) {
	    if (errno == EINTR) continue;
	    return -1;
	}
	if (r == 0) return 0;  
        left    -= r;
	bufptr  += r;
    }
    return 1;
}


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

typedef struct th_arg {
    sigset_t *mask;
    int pipe_d;
} th_arg;


void print_deb();
int isNumber(void *el, int *n);
int st_repl(int dim, int fd, char *path, file_node **list, int app_wr);
int file_sender(file_node *list, int fd);
int print_summ(char *log);

file_info *init_file(int lock_owner, int fd, int lst_op, char *cnt, int sz);

int insert_head(open_node **list, int info);
int remove_elm(open_node **list, int info);
int find_elm(open_node *list, int elm);
void print_list(open_node *list);
int insert_head_f(file_node **list, char *path, int sz_cnt, char *cnt); //potrei usare delle funz generiche
void free_data_ht(void *data);

int openFC(char *path, int fd);
int openFL(char *path, int fd);
int openFO(char *path, int fd);
int openFCL(char *path, int fd);
int closeF(char *path, int fd);
int readF(char *path, int fd);
int writeF(char *path, char *cnt, int sz, int fd);
int lockF(char *path, int fd);
int unlockF(char *path, int fd);
int appendToF(char *path, char *cnt, int sz, int fd);
int removeF(char *path, int fd);
int readNF(int fd, int n);

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
//controlla la mutex sia nelle api (anche quella di inizializzazione)

icl_hash_t *hash_table = NULL;
pthread_mutex_t ht_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ht_cond = PTHREAD_COND_INITIALIZER;


int st_dim;
int st_dim_MAX;
int n_files;
int n_files_MAX;
pthread_mutex_t server_info_mtx = PTHREAD_MUTEX_INITIALIZER;

FILE *log_file; // puntatore al file di log
pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;


static void *sigHandler(void *arg) { //mi raccomando breve
    sigset_t *set = ((th_arg *)arg)->mask; //i segnali per cui sono in attesa (mascherati dal chiamante)
	int pipe_d = ((th_arg *)arg)->pipe_d;

    for( ;; ) {
		int sig;
		int rit;
		int r = sigwait(set, &sig); //attesa passiva (smaschera i segnali e si mette in attesa atomicamente)
		if (r != 0) {
			errno = r;
			perror("FATAL ERROR 'sigwait'");
			return NULL;
		}

		switch(sig) { //segnale ricevuto 
			case SIGINT: 
			case SIGQUIT:
				//termina = 1;
				// sblocca l'accept rendendo non piu' valido il listenfd
				//shutdown(listenfd, SHUT_RDWR);
				rit = 1;
				if(writen(pipe_d, &rit, 2) == -1)
					perror("Write sig_handler");
				
				// SOLUZIONE ALTERNATIVA CHE NON USA shutdown:
				// Il thread dispatcher (in questo esercizio il main thread) invece di sospendersi su una accept si
				// sospende su una select in cui, oltre al listen socket, registra il discrittore di lettura di una
				// pipe condivisa con il thread sigHandler. Il thead sigHandler quando riceve il segnale chiude il
				// descrittore di scrittura in modo da sbloccare la select.	    	    
				return NULL;
			case SIGHUP: 
				rit = -1;
				if(writen(pipe_d, &rit, 2) == -1)
					perror("Write sig_handler");
				
				return NULL;  //va bene? O devo rimanere in attesa di un possibile sigint?
			default:  ; 
		}
    }
    return NULL;	   
}


int executeTask(int fd_c){ //qui faccio la read, capisco cosa devo fare, lo faccio chiamando la giusta funzione, rispondo al client, scrivo fd sulla pipe per il server  
		
	char *msg, *path_cpy; 
	char *tmpstr, *token, *token2;
	//int seed = time(NULL);
	int n, sz_msg;
		
	if((msg = malloc(MAX_SIZE*sizeof(char))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		exit(errno_copy);
	}
	memset(msg, '\0', MAX_SIZE);
	if(readn(fd_c, &sz_msg, sizeof(int)) == -1){
		perror("read socket lato server");
		free(msg);
		return -1; //ok???  
	}
	//fprintf(stderr, "ok s: %d\n", sz_msg);
	if(readn(fd_c, msg, sz_msg) == -1){
		perror("read socket lato server");
		free(msg);
		return -1; //ok???  
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
			if((path_cpy = malloc((strlen(token)+1)*sizeof(char))) == NULL){
				perror("malloc");
				int errno_copy = errno;
				fprintf(stderr,"FATAL ERROR: malloc\n");
				free(msg);
				exit(errno_copy);
			}
			strncpy(path_cpy, token, strlen(token)+1);
			if(openFC(path_cpy, fd_c) == -1){
				free(msg);
				free(path_cpy);
				return -1;
			}
			Pthread_mutex_lock(&log_mtx); //vedi se modificarle con la maiuscola (funzione util)
			fprintf(log_file,"%s %s -1 -1 %lu\n", "open_c", token, pthread_self());
			Pthread_mutex_unlock(&log_mtx);				   	
		}
		else if(strcmp(token, "openfl") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(openFL(token, fd_c) == -1){
				//fprintf(stderr, "openLock non riuscita\n");
			}
			else{
				Pthread_mutex_lock(&log_mtx); //vedi se modificarle con la maiuscola (funzione util)
				fprintf(log_file,"%s %s -1 -1 %lu\n", "open_l", token, pthread_self());
				Pthread_mutex_unlock(&log_mtx);
			}
			
		}
		else if(strcmp(token, "openfo") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(openFO(token, fd_c) == -1){
				free(msg);
				return -1;
			}
			Pthread_mutex_lock(&log_mtx); //vedi se modificarle con la maiuscola (funzione util)
			fprintf(log_file,"%s %s -1 -1 %lu\n", "open_o", token, pthread_self());
			Pthread_mutex_unlock(&log_mtx);
			
		}
		else if(strcmp(token, "openfcl") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if((path_cpy = malloc((strlen(token)+1)*sizeof(char))) == NULL){
				perror("malloc");
				int errno_copy = errno;
				fprintf(stderr,"FATAL ERROR: malloc\n");
				free(msg);
				exit(errno_copy);
			}
			strncpy(path_cpy, token, strlen(token)+1);
			if(openFCL(path_cpy, fd_c) == -1){
				free(msg);
				free(path_cpy);
				return -1;
			}
			Pthread_mutex_lock(&log_mtx); //vedi se modificarle con la maiuscola (funzione util)
			fprintf(log_file,"%s %s -1 -1 %lu\n", "open_cl", token, pthread_self());
			Pthread_mutex_unlock(&log_mtx);
			
		}
		else if(strcmp(token, "disconnesso") == 0){//da chiudere i file e aggiornarli
			
			Pthread_mutex_lock(&ht_mtx);
			//fprintf(stderr, "QUEUE INFO:\nlen: %d\nElm:\n", (int) length(file_queue));
			int n = length(file_queue);
			for(int i = 0; i < n; i++){
				file_entry_queue *tmp = pop(file_queue);
				#ifdef DEBUG
					fprintf(stderr, "path: %s, sz_cnt: %d\ncnt:\n", tmp->path, tmp->pnt->cnt_sz); 
					if(tmp->pnt->cnt_sz > 0){
						fwrite(tmp->pnt->cnt, tmp->pnt->cnt_sz, 1, stdout);
					}
					else
						fprintf(stderr, "//\n");
				#endif
				free(tmp);
			}
			
			
			pthread_cond_broadcast(&ht_cond); //sveglio chi era in attesa della lock 
			Pthread_mutex_unlock(&ht_mtx);
			//devo creare una funz che scorre lo storage e toglie ovunque lock_owner (e open_owner)
			
			close(fd_c); //non so se farlo qui o in run_server 
			
			
			Pthread_mutex_lock(&log_mtx); //vedi se modificarle con la maiuscola (funzione util)
			fprintf(log_file,"%s %d -1 -1 %lu\n", "close_connection", fd_c, pthread_self());
			Pthread_mutex_unlock(&log_mtx);
			

			free(msg);
			return -2;
		}
		else if(strcmp(token, "closef") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(closeF(token, fd_c) == -1){
				free(msg);
				return -1;
			}
			Pthread_mutex_lock(&log_mtx); //vedi se modificarle con la maiuscola (funzione util)
			fprintf(log_file,"%s %s -1 -1 %lu\n", "close", token, pthread_self());
			Pthread_mutex_unlock(&log_mtx);
			
		}
		else if(strcmp(token, "readf") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(readF(token, fd_c) == -1){
				free(msg);
				return -1;
			}
			
		}
		else if(strcmp(token, "writef") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			token2 = strtok_r(NULL, ";", &tmpstr);
			if(isNumber(token2, &n) == 0 && n > 0){
				if(readn(fd_c, token2, n) == -1){
					perror("read server in execute task");
					free(msg);
					return -1;
				}
			}
			else
				token2 = NULL;
				
			if(writeF(token, token2, n, fd_c) == -1){
				free(msg);
				return -1;
			}			
		}
		else if(strcmp(token, "lockf") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			int res = lockF(token, fd_c);
			int n;
			switch(res){
				case -1: 
					n = 21;
					if(writen(fd_c, &n, sizeof(int)) == -1){//le write dovranno essere riviste (atomicità)
						perror("Errore nella write");
						free(msg);
						return -1;
					}
					if(writen(fd_c, "Err:fileNonEsistente", n) == -1)
						perror("write server execute task lockf");
					free(msg);
					return -1;
				case 0: 
					n = 3;
					if(writen(fd_c, &n, sizeof(int)) == -1){//le write dovranno essere riviste (atomicità)
						perror("Errore nella write");
						free(msg);
						return -1;
					}
					if(writen(fd_c, "Ok", n) == -1){
						perror("write server execute task lockf");
						free(msg);
						return -1;
					}
					break;
				default: fprintf(stderr, "Errore, return from lockF");
					n = 16;
					if(writen(fd_c, &n, sizeof(int)) == -1){//le write dovranno essere riviste (atomicità)
						perror("Errore nella write");
						free(msg);
						return -1;
					}
					if(writen(fd_c, "Err:sconosciuto", n) == -1){
						perror("write server execute task lockf");
						free(msg);
						return -1;
					}
			
			}
			if(res != -1){
				Pthread_mutex_lock(&log_mtx); //vedi se modificarle con la maiuscola (funzione util)
				fprintf(log_file,"%s %s -1 -1 %lu\n", "lock", token, pthread_self());
				Pthread_mutex_unlock(&log_mtx);
			}
			
		}
		else if(strcmp(token, "unlockf") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(unlockF(token, fd_c) == -1){
				free(msg);
				return -1;
			}
			Pthread_mutex_lock(&log_mtx); //vedi se modificarle con la maiuscola (funzione util)
			fprintf(log_file,"%s %s -1 -1 %lu\n", "unlock", token, pthread_self());
			Pthread_mutex_unlock(&log_mtx);
			
		}
		else if(strcmp(token, "appendTof") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			token2 = strtok_r(NULL, ";", &tmpstr);
			if(isNumber(token2, &n) == 0 && n > 0){
				if(readn(fd_c, token2, n) == -1){
					perror("read server in execute task");
					free(msg);
					return -1;
				}
			}
			else
				token2 = NULL;
				
			if(appendToF(token, token2, n, fd_c) == -1){
				free(msg);
				return -1;
			}			
		}
		else if(strcmp(token, "removef") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(removeF(token, fd_c) == -1){
				free(msg);
				return -1;
			}
			
		}
		
		else if(strcmp(token, "readNf") == 0){
			token2 = strtok_r(NULL, ";", &tmpstr);
			if(isNumber(token2, &n) == 0){
				if(readNF(fd_c, n) == -1){
					free(msg);
					return -1;
				}
			}
		}

		else{
			fprintf(stderr, "Opzione non ancora implementata: %s\n", token);
			if(writen(fd_c, "Ok", 3) == -1){
				perror("write server execute task opz sconosciuta");
				free(msg);
				return -1;
			}
		}
		
	}

	#ifdef DEBUG
		fprintf(stderr,"\n---HASHTABLE:\n");
		print_deb(hash_table);
	#endif	
	
	free(msg); 

	return 0;
}

void *init_thread(void *args){ //CONSUMATORE
	int res, n = 0;
	int descr;
	
	//fprintf(stderr, "sono nato %lu\n", pthread_self());
	while(1){
		
		int  *fd = pop(task_queue);		
		if(fd == NULL){
			fprintf(stderr,"Errore pop, init thread\n");
			pthread_exit(NULL); //da modificare 
		}
		if( *fd == -1)
            return (void*) 0;
		
		res = executeTask(*fd);
		descr = *fd;
		free(fd);
		if( res == -1){ //errori non fatali
			fprintf(stderr, "Impossibile servire richiesta client %d\n", descr);
		}
		
		if( res == -2){ //disconnessione client fd 
			n = 1;
		}
		if( res == -3){ //errori fatali 
			pthread_exit(NULL);
		}

		//fprintf(stderr, "scrivo sulla pipe: %d fd : %d\n", *((int*)args), fd);

		if(writen(*((int*)args), &n, 2) == -1){
			perror("write init_thread su pipe");
			return NULL; //In questo caso è ok? controlla pthread_create 
		}
		if(!n){
			if(writen(*((int*)args), &descr, 2) == -1){
				perror("write init_thread su pipe");
				return NULL; //In questo caso è ok? controlla pthread_create 
			}
		}
	}
	
	return (void *) 0; 
}


void *submitTask(int descr){ //PRODUTTORE
	int *fd = malloc(sizeof(int));
	*fd = descr;
	
    if (push(task_queue, fd) == -1) {
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

int run_server(struct sockaddr_un *sa, int pipe, int sig_pipe){
	
	int stop = 0;
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
	
	if(sig_pipe > fd_num)
		fd_num = sig_pipe;
	
	FD_ZERO(&set); //per evitare errori prima lo azzero
	FD_ZERO(&rdset); //per evitare errori prima lo azzero
	
	FD_SET(fd_skt, &set); //metto a 1 il bit di fd_skt, fd_skt pronto
	FD_SET(pipe, &set);
	FD_SET(sig_pipe, &set);
    	
	//select in cui, oltre al listen socket, registra il discrittore di lettura di una
	    // pipe condivisa con il thread sigHandler.
	//inizio il ciclo di comunicazione con i workers 
	int n_conn = 0;
	int sighup = 0;
	
	while(!stop){
		int res;
		//fprintf(stderr, "ITERAZIONE: i: %d, pipe: %d, skt: %d, fd_num: %d, nuovo fd_r: %d \n", i, pipe, fd_skt, fd_num, fd_r);
		rdset = set; //aggiorno ogni volta la maschera per la select, ne servono due perchè select modifica quello che gli passi
		if((res = select(fd_num+1,	&rdset, NULL, NULL, NULL)) == -1){ //si blocca finchè uno tra i descrittori in rdset non si blocca più su una read
			perror("select");
			return -1;
		}
		if(sighup)  //non ci sono più descrittori di connessioni attive
			if(n_conn == 0)
				break;
		/*if(res == 0){ //non ho specificato il timeout tanto
			stop = 1;
		}*/
		//appena la select mi dice che c'è un worker pronto (descrittore client libero), inizio a scorrerli
		
		for(fd = 0; fd <= fd_num; fd++){ //guardo tutti i descrittori nel nuovo rdset (aggiornato da select)
			
			if(FD_ISSET(fd, &rdset)){ //controllo quali di questi è pronto in lettura
			   // fprintf(stderr, "-----------fd: %d è pronto\n", fd);
				
				if(fd == fd_skt){ //se è il listen socket faccio accept [richiesta connessione da client]
					//fprintf(stderr, "ho letto il SOCKET, faccio accept\n");
					if(!sighup){  //se non c'è stato il segnale sighup accetto nuove connessioni 
						fd_c = accept(fd_skt, NULL, 0);
						//fprintf(stderr, "ACCEPT fatta, %d è il fd_c del client\n", fd_c);
						//if fd_c != -1 
						
						Pthread_mutex_lock(&log_mtx); //vedi se modificarle con la maiuscola (funzione util)
						fprintf(log_file,"%s %d -1 -1 %lu\n", "open_connection", fd_c, pthread_self());
						Pthread_mutex_unlock(&log_mtx);
					   
						FD_SET(fd_c, &set); //fd_c pronto
						if(fd_c > fd_num) 
							fd_num = fd_c;
					}
					n_conn++;
				}
				else{ //(altrimenti vado con la read)
				
					if(fd == pipe){ //se è la pipe (comunicaz master workers) aggiorno i descrittori
						int disc = 0;
					//fprintf(stderr, "ho letto il fd: %d di PIPE, faccio read\n", fd);					
						
						if(readn(pipe, &disc, 2) != -1){ //richiesta servita, fd_r è di nuovo libero, aggiorno set e fd_num
							if(disc)
									n_conn--;
							else{
								if(readn(pipe, &fd_r, 2) != -1){
									FD_SET(fd_r, &set);
									if(fd_r > fd_num) 
										fd_num = fd_r;
								}
								else{ //quel descrittore è stato chiuso, non lo riaggiungo
									perror("READ PIPE MASTER:");
									return -1;
								}
							}
							
							/*else{  
								fd = fd*(-1);
								FD_CLR(fd, &set); //tolgo il fd da set, forse basta non riaggiungercelo?  
								if(fd == fd_num) {
									if((fd_num = aggiorna_max(fd, set)) == -1)
									return -1; //???
								
							    }
							}*/
							
						}
						else{
							perror("READ PIPE MASTER:");
							return -1;
						}
						//fprintf(stderr, "read su pipe fatta, ho letto fd_r: %d\n", fd_r);
						
					}
					else if(fd == sig_pipe){ 
						
						if(readn(sig_pipe, &fd_r, 2) != -1){
							if(fd_r > 0){ //sigint, sigquit
								stop = 1;
								//dovrei chiudere la pipe, il socket e le connessioni attive??
							}
							else
								sighup = 1;
						}
						else{
							perror("READ SIG_PIPE MASTER:");
							return -1;
						}
					}
					else{ //altrimenti eseguo il comando richiesto dal descrittore 
						
						//fprintf(stderr, "ho letto un descrittore client fd: %d, faccio SUBMIT\n", fd);

						FD_CLR(fd, &set); //tolgo il fd da set 
						
						if(fd == fd_num) {
							if((fd_num = aggiorna_max(fd, set)) == -1){
								return -1; //???
							}
		
						}
						submitTask(fd);
						
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
	
		if((aux = malloc(MAX_SIZE*sizeof(char))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
				
        while (fgets(aux, MAX_SIZE, fp)){ //dovresti fare gestione errore di fgets
			token = strtok_r(aux, ":", &tmpstr);
			if(strcmp(token, "NFILES") == 0){
				token = strtok_r(NULL, ":", &tmpstr);
				token[strlen(token) - 2] = '\0'; //brutto, vedi se lo puoi migliorare (non voglio considerare il \n)
				if(isNumber(token, &n) == 0 && n > 0){
					n_files_MAX = n;
				}
				else{
					fprintf(stderr, "errore in config file: nfiles_max value errato, verrà usato valore di default\n");
				}
			}
			else if(strcmp(token, "DIM") == 0){
				token = strtok_r(NULL, ":", &tmpstr);
				token[strlen(token) - 2] = '\0'; //brutto, vedi se lo puoi migliorare (non voglio considerare il \n)
				if(isNumber(token, &n) == 0 && n > 0){
					st_dim_MAX = n;
				}
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
					if((socket = malloc(UNIX_PATH_MAX*sizeof(char))) == NULL){
						perror("malloc");
						int errno_copy = errno;
						fprintf(stderr,"FATAL ERROR: malloc\n");
						exit(errno_copy);
					}
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
					if((log = malloc(UNIX_PATH_MAX*sizeof(char))) == NULL){
						perror("malloc");
						int errno_copy = errno;
						fprintf(stderr,"FATAL ERROR: malloc\n");
						exit(errno_copy);
					}
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
	
	//apro log_file 
	log_file = fopen(log,"w"); 
    fflush(log_file);   // ripulisco il file aperto da precedenti scritture, serve?
	
	//Inizializzazione thread pool 
	pthread_t th[n_workers];
	
	
	//pthread_mutex_init(&mutexCoda, NULL);
	//pthread_cond_init(&condCoda, NULL);
	//stanno in initqueue
	
	//inizializzazione pipe socket 
	int pfd[2];
	
	if(pipe(pfd) == -1){
		perror("creazione pipe");
		return -1;
	}
	
	//inizializzazione pipe signal 
	int sig_pfd[2];
	
	if(pipe(sig_pfd) == -1){
		perror("creazione signal pipe");
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
		free(socket);
		free(log);
		return -1;
	}
	
	//inizializzazione task_queue
	task_queue = initQueue();
    if (!task_queue) {
		fprintf(stderr, "initQueue su tsk_q fallita\n");
		free(socket);
		free(log);
		exit(errno); //controlla
    }
	
	//inizializzazione file_queue
	file_queue = initQueue();
    if (!file_queue) {
		fprintf(stderr, "initQueue su file_q fallita\n");
		free(socket);
		free(log);
		exit(errno); //controlla
    }
	
	
	//creo maschera per thread gestore dei segnali, prima la azzero
	//poi inserisco i segnali che voglio gestire 
	
	sigset_t     mask_s;
    sigemptyset(&mask_s); 
    sigaddset(&mask_s, SIGINT); 
    sigaddset(&mask_s, SIGQUIT);
    sigaddset(&mask_s, SIGHUP);    
	
    if (pthread_sigmask(SIG_BLOCK, &mask_s,NULL) != 0) {
		fprintf(stderr, "FATAL ERROR\n");
		free(socket);
		free(log);
		abort();
    }
    
	// ignoro SIGPIPE per evitare di essere terminato da una scrittura su un socket senza lettori 
	// Magari il client fa una richiesta e chiude la comunicazione senza attendere la risposta
	//il server non deve terminare in questo caso
    
	struct sigaction s;
    memset(&s,0,sizeof(s));    
    
	s.sa_handler = SIG_IGN;
    if ( (sigaction(SIGPIPE, &s, NULL) ) == -1 ) {   
		perror("sigaction");
		free(socket);
		free(log);
		abort();
    } 
	
	//inizializzo thread gestore dei segnali
	pthread_t sighandler_thread;
	
	//gli devo passare anche il descrittore di lettura della pipe 
	
	//la maschra viene ereditata, ma la devo comunque inviare per poter fare la wait 
    
	th_arg *args;
	
	if((args = malloc(sizeof(th_arg))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		free(socket);
		free(log);
		exit(errno_copy);
	}
	
	args->mask = &mask_s;
	args->pipe_d = sig_pfd[1];
	
	
    if (pthread_create(&sighandler_thread, NULL, sigHandler, args) != 0) {
		fprintf(stderr, "errore nella creazione del signal handler thread\n");
		free(socket);
		free(log);
		free(args);
		abort();
    }
	
	//creazione e avvio threadpool 
	
	for(int i = 0; i < n_workers; i++){
	
	    //maschero i segnali per tutti i thread che non siano il signal handler thread
		sigset_t mask;
		sigemptyset(&mask);
		sigaddset(&mask, SIGINT); 
		sigaddset(&mask, SIGQUIT);
		sigaddset(&mask, SIGHUP);

		if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) { //questa maschera viene ereditata dal thread creato 
			fprintf(stderr, "FATAL ERROR\n");
			//close(connfd);
			free(socket);
			free(log);
			free(args);
			return -1;
		}
		
		if(pthread_create(&th[i], NULL, &init_thread, (void *) &pfd[1]) != 0){ //creo il thread che inizia ad eseguire init_thread
			perror("Err in creazione thread");
			//pulizia 
			free(socket);
			free(log);
			free(args);
			return -1;
		}
		
	}
	//avvio server 
	run_server(&sa, pfd[0], sig_pfd[0]); 
   
    for (int i=0;i<n_workers;i++)
        {   int k = -1;
            push(task_queue, &k);
        }
	
	 // aspetto la terminazione de signal handler thread
    if(pthread_join(sighandler_thread, NULL) != 0){
		perror("Err in join sighandler thread");
		free(socket);
		free(log);
		free(args);
		return -1;
	}
	
  
	for(int i = 0; i < n_workers; i++){ //quando terminano i thread? Quando faccio la exit? 
		if(pthread_join(th[i], NULL) != 0){ //aspetto terminino tutti i thread (la  join mi permette di liberare la memoria occupata dallo stack privato del thread)
			perror("Err in join thread");
			free(socket);
			free(log);
			free(args);
			return -1;
		}
	}
	

	fclose(log_file);
	 
	
	fprintf(stdout, "\n\nLISTA FILE:\n\n");
	fflush(stdout);
	print_deb(hash_table);
	
	print_summ(log);
	
	//eliminazione elm thread 
	//pthread_mutex_destroy(&mutexCoda); //non so se vanno bene
	//pthread_cond_destroy(&condCoda);
	//sostituiti con: 
	//eliminazione task_queue  
	
	icl_hash_destroy(hash_table, &free, &free_data_ht);
	
	deleteQueue(task_queue);
	/*file_entry_queue *tmp;
	while(file_queue != NULL && file_queue->head != file_queue->tail){
		tmp = pop(file_queue);
		if(tmp->path != NULL)
			free(tmp->path);
	}*/
	deleteQueue(file_queue);
	//assicurati tutte le connessioni siano chiuse sul socket prima diterminare
	//finora chiudo la connessione quando ricevo il mess "disconnesso" dal client
	free(socket);
	free(log);
	free(args);
	
	//dovai anche chiudere il socket direi, ma forse lo fa run server 
	return 0;
}

void free_data_ht(void *data){
	if((file_info *)data){
		if(((file_info *)data)->open_owners){
			open_node *tmp;
			while(((file_info *)data)->open_owners != NULL){
				tmp = ((file_info *)data)->open_owners;
				((file_info *)data)->open_owners = ((file_info *)data)->open_owners->next;
				free(tmp);
			}
		}
	}
	free(data);
	return;
}
	

int print_summ(char *log){

	FILE *fd = NULL;
	char *buffer = NULL;
	int cnt_dim = 0;
	int cnt_dim_MAX = 0;
	int cnt_nf = 0;
	int cnt_nf_MAX = 0;
	int cnt_rimp = 0;
	int bW, bR;
	
	char *cmd, *bW_s, *bR_s, *newline, *tmpstr;
	
	if ((fd = fopen(log, "r")) == NULL) {
		perror("opening password file");
		return -1;
    }
	if ((buffer = malloc(MAX_SIZE*sizeof(char))) == NULL) {
		perror("malloc buffer"); 
		fclose(fd);
        return -1;		
	}
	while(fgets(buffer, MAX_SIZE, fd) != NULL) {
		// controllo di aver letto tutta una riga
		if ((newline = strchr(buffer, '\n')) == NULL) {
			fprintf(stderr, "buffer di linea troppo piccolo");
			fclose(fd);
			return -1;
		}
		*newline = '\0';  // tolgo lo '\n', non strettamente necessario
		
		cmd = strtok_r(buffer, " ", &tmpstr);
		strtok_r(NULL, " ", &tmpstr);
		bW_s = strtok_r(NULL, " ", &tmpstr);
		bR_s = strtok_r(NULL, " ", &tmpstr);
		if(isNumber(bW_s, &bW) != 0){
			free(buffer);
			fclose(fd);
			return -1;
		}
		if(isNumber(bR_s, &bR) != 0){
			free(buffer);
			fclose(fd);
			return -1;
		}
		
		if (strncmp(cmd, "rimpiazzamento", MAX_SIZE) == 0) {
			cnt_rimp++;
		}
		if (strncmp(cmd, "writeF", MAX_SIZE) == 0 || strncmp(cmd, "append", MAX_SIZE) == 0) {
			cnt_dim += bW;
			if(cnt_dim > cnt_dim_MAX)
				cnt_dim_MAX = cnt_dim;
		}
		if (strncmp(cmd, "remove", MAX_SIZE) == 0 || strncmp(cmd, "rimpiazzamento", MAX_SIZE) == 0) {
			cnt_dim -= bR;
		}
		if (strncmp(cmd, "open_c", MAX_SIZE) == 0 || strncmp(cmd, "open_cl", MAX_SIZE) == 0) {
			cnt_nf++;
			if(cnt_nf > cnt_nf_MAX)
				cnt_nf_MAX = cnt_nf;
		}
		if (strncmp(cmd, "remove", MAX_SIZE) == 0 || strncmp(cmd, "rimpiazzamento", MAX_SIZE) == 0) {
			cnt_nf--;
		}
	}
    fprintf(stderr, "\nIl numero massimo di file memorizzati nel server è: %d\n", cnt_nf_MAX);
	
	fprintf(stderr, "La dimensione massima in MBytes raggiunta dal file storage è: %.2f\n", ((float)cnt_dim_MAX)/1000);
	
	fprintf(stderr, "L'algoritmo di rimpiazzamento è stato eseguito %d volte\n\n", cnt_rimp);

	fclose(fd);
	free(buffer);
	
	return 0;
}

file_info *init_file(int lock_owner, int fd, int lst_op, char *cnt, int sz){
	
	file_info *tmp;
	   
	if((tmp = malloc(sizeof(file_info))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		exit(errno_copy);
	}
		
	open_node *client;
	if((client = malloc(sizeof(open_node))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		exit(errno_copy);
	}
	client->fd = fd;	
	client->next = NULL;
	tmp->open_owners = client;

	tmp->lock_owner = lock_owner;
	tmp->lst_op = lst_op;
	if(sz == -1)
		sz = 0;
	tmp->cnt_sz = sz;
	if(sz > 0)
		memcpy(tmp->cnt, cnt, sz); //controlla 

	return tmp; //viene liberato quando faccio la delete_ht o destroy_ht 
}

int insert_head_f(file_node **list, char *path, int cnt_sz, char *cnt){
	
	file_node *tmp;
	
	if((tmp = malloc(sizeof(file_node))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		exit(errno_copy);
	}
	tmp->path = path;
	tmp->cnt_sz = cnt_sz;
   
	if (cnt_sz > 0)
		memcpy(tmp->cnt, cnt, cnt_sz);
	
	tmp->next = *list;
	
	*list = tmp;
	
	return 0;
}
int insert_head(open_node **list, int info){
	open_node *tmp;
	open_node *aux = *list;
	
	while(aux != NULL){ //no duplicati
		if(aux->fd == info)
			return 0;
		else 
			aux = aux->next;
	}
	
	if((tmp = malloc(sizeof(open_node))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		exit(errno_copy);
	}
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

int find_elm(open_node *list, int elm){
	
	while(list != NULL){
		if(list->fd == elm)
			return 0;
		else
			list = list->next;
    }
	return -1;
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

void print_deb(){
	icl_entry_t *entry;
	char *key, *value;
	int k;
	Pthread_mutex_lock(&ht_mtx);
	if (hash_table) {
		file_info *aux;
		
				
		for (k = 0; k < hash_table->nbuckets; k++)  {
			for (entry = hash_table->buckets[k]; entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL); entry = entry->next){
				if((aux = icl_hash_find(hash_table, key)) != NULL){
				
					fprintf(stderr, "File: %s", key);
					
					#ifdef DEBUG
					fprintf(stderr, "\nk = %d, lock_owner: %d, last_op: %d\n",k, aux->lock_owner, aux->lst_op);
					print_list(aux->open_owners);
					if(aux->cnt != NULL){
						fprintf(stderr, "cnt_sz: %d, cnt:\n", aux->cnt_sz);						
						fwrite(aux->cnt, aux->cnt_sz, 1, stderr);
						fprintf(stderr, "\n");
					}
					else
						fprintf(stderr, "contenuto: NULL\n");
					#endif
					fprintf(stderr, "\n");
			    }
			}
		}
		fprintf(stderr, "\n");
	}
	else{
		fprintf(stderr, "hash table vuota\n");
	}
	Pthread_mutex_unlock(&ht_mtx);
	
	return;
}

//inizio in modo semplice, mettendo grandi sezioni critiche 

int openFC(char *path, int fd){ //free(data)?
	file_info *data;
	int n;
	int found = 0;
	
	Pthread_mutex_lock(&ht_mtx);
	if(icl_hash_find(hash_table, path) != NULL){ 
		fprintf(stderr, "Impossibile ricreare file già esistente\n");
		n = 18;
		if(writen(fd, &n, sizeof(int)) == -1){//le write dovranno essere riviste (atomicità)
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "Err:fileEsistente", n) == -1)
			perror("write server openFC");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
		
		file_node *aux = NULL;
		file_node *prec = NULL;
		
		if(st_repl(0, fd, path, &aux, -1) == -1) //per ora lo faccio tutto lockato 
				fprintf(stderr, "Errore in repl\n");
		if(aux != NULL){		
			while(aux != NULL){
				prec = aux;
				if(icl_hash_delete(hash_table, aux->path, &free, &free_data_ht) == -1){ 
					fprintf(stderr, "Errore nella rimozione del file dallo storage, inrepl\n");
					pthread_cond_broadcast(&ht_cond);
					Pthread_mutex_unlock(&ht_mtx);
					return -1;
				}	
				aux = aux->next;
				free(prec);
			}
			found = 1;
		}
			
		data = init_file(-1, fd, 0, NULL, -1);  
		
		
		if(icl_hash_insert(hash_table, path, (void *) data) == NULL){
			fprintf(stderr, "Errore nell'inseriemento del file nello storage\n");
			n = 16;
			if(writen(fd, &n, sizeof(int)) == -1){
				perror("Errore nella write");
				if(found)
					pthread_cond_broadcast(&ht_cond);
				Pthread_mutex_unlock(&ht_mtx);
				free_data_ht(data);
				return -1;
			}
			if(writen(fd, "Err:inserimento", n) == -1)
				perror("write server openFC");
			if(found) pthread_cond_broadcast(&ht_cond);
			Pthread_mutex_unlock(&ht_mtx);
			free_data_ht(data);
			return -1;
		}
		if(found) pthread_cond_broadcast(&ht_cond);
		Pthread_mutex_unlock(&ht_mtx);
		
		Pthread_mutex_lock(&server_info_mtx);
		n_files++; 
		Pthread_mutex_unlock(&server_info_mtx);
		
		file_entry_queue *qdata;
		if((qdata = malloc(sizeof(file_entry_queue))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		
		qdata->pnt = data;
		qdata->path = path;
		if (push(file_queue, qdata) == -1) {
			fprintf(stderr, "Errore: push\n");
			pthread_exit(NULL); //controlla se va bene 
		}
		n = 3;
		if(writen(fd, &n, sizeof(int)) == -1){//le write dovranno essere riviste (atomicità)
			perror("Errore nella write");
			return -1;
		}
		if(writen(fd, "Ok", n) == -1){
			perror("write server openFC");
			return -1;
		}
		
		#ifdef DEBUG
			fprintf(stderr,"---OPEN CREATE FILE\n");
			print_deb();
		#endif 
		//free(data);
		//quando faccio il pop della coda devo ricordarmi di liberare la memoria
	}
	return 0;
}

int openFL(char *path, int fd){
	file_info *tmp;
	int n;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		fprintf(stderr, "Impossibile fare lock su file non esistente\n"); //me lo stampa ogni volta che creo un file, aggiustalo
		n = 21;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write di OpenFL");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "Err:fileNonEsistente", n) == -1)
			perror("write server openFL");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	} 
	else{ //se sta cercando di aprire un file che ha già aperto, lo ignoro (per ora)
		while((tmp->lock_owner != -1 && tmp->lock_owner != fd)){
			pthread_cond_wait(&ht_cond,&ht_mtx);
			
			if((tmp = icl_hash_find(hash_table, path)) == NULL){ //il file potrebbe essere stato rimosso dal detentore della lock 
				fprintf(stderr, "Impossibile fare lock su file non esistente\n"); //me lo stampa ogni volta che creo un file, aggiustalo
				n = 21;
				if(writen(fd, &n, sizeof(int)) == -1){
					perror("Errore nella write");
					Pthread_mutex_unlock(&ht_mtx);
					return -1;
				}
				if(writen(fd, "Err:fileNonEsistente", n) == -1)
					perror("write server openFL");
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			} 
		}
		
		tmp->lock_owner = fd;
		insert_head(&(tmp->open_owners), fd); //dovrei controllare val ritorno
		
		tmp->lst_op = 0;
		
		Pthread_mutex_unlock(&ht_mtx);
		n = 3;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			return -1;
		}
		
		if(writen(fd, "Ok", n) == -1){
			perror("write server openFL");
			return -1;
		}	
		
	}
	
	#ifdef DEBUG
		fprintf(stderr,"---OPEN LOCKED FILE\n");
		print_deb();
	#endif
	return 0;
}

int openFO(char *path, int fd){
	file_info *tmp;
	int n;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		fprintf(stderr, "Impossibile aprire un file non esistente\n");
		n = 21;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		
		if(writen(fd, "Err:fileNonEsistente", n) == -1)
			perror("write server openFO");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	} 
	else{ //se sta cercando di aprire un file che ha già aperto, lo ignoro (per ora)
		insert_head(&(tmp->open_owners), fd); //dovrei controllare val ritorno
		tmp->lst_op = 0;
		
		Pthread_mutex_unlock(&ht_mtx);
		n = 3;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			return -1;
		}
		if(writen(fd, "Ok", n) == -1){
			perror("write server openF0");
			return -1;
		}
	}
	#ifdef DEBUG
		fprintf(stderr,"---OPEN FILE \n");
		print_deb();
	#endif
	
	return 0;
	
}

int openFCL(char *path, int fd){
		file_info *data;
		int n;
		int found = 0;
		
		Pthread_mutex_lock(&ht_mtx);
		if(icl_hash_find(hash_table, path) != NULL){ 
			fprintf(stderr, "Impossibile ricreare un file già esistente\n");
			n = 18;
			if(writen(fd, &n, sizeof(int)) == -1){
				perror("Errore nella write");
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}
			
			if(writen(fd, "Err:fileEsistente", n) == -1)
				perror("write server openFCL");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		} 
		else{ 
			
			file_node *aux = NULL;
			file_node *prec = NULL;
	
			/*if(st_repl(0, fd, path, &aux, -1) == -1)
				fprintf(stderr, "Errore in repl\n");*/
			if(aux != NULL){
				while(aux != NULL){
					prec = aux;
					
					if(icl_hash_delete(hash_table, aux->path, &free, &free_data_ht) == -1){ 
						fprintf(stderr, "Errore nella rimozione del file dallo storage, inrepl\n");
						pthread_cond_broadcast(&ht_cond);
						Pthread_mutex_unlock(&ht_mtx);
						return -1;
					}	
					aux = aux->next;
					free(prec);
				
				}
				found = 1;
			}
			data = init_file(fd, fd, 1, NULL, -1); 
			
			if(icl_hash_insert(hash_table, path, data) == NULL){
				fprintf(stderr, "Errore nell'inseriemento del file nello storage\n");
				n = 16;
				if(writen(fd, &n, sizeof(int)) == -1){
					perror("Errore nella write");
						if(found) pthread_cond_broadcast(&ht_cond);
					Pthread_mutex_unlock(&ht_mtx);
					free_data_ht(data);
					return -1;
				}
				
				if(write(fd, "Err:inserimento", n) == -1)
					perror("write server openFCL");
				if(found) pthread_cond_broadcast(&ht_cond);
				Pthread_mutex_unlock(&ht_mtx);
				free_data_ht(data);
				return -1;
			}
				if(found) pthread_cond_broadcast(&ht_cond);
			Pthread_mutex_unlock(&ht_mtx);			
			
			n = 3;
			if(writen(fd, &n, sizeof(int)) == -1){
				perror("Errore nella write");
				return -1;
			}
			if(writen(fd, "Ok", n) == -1){
				perror("write server openFCL");
				return -1;
			}	
			Pthread_mutex_lock(&server_info_mtx);
			n_files++; 
			Pthread_mutex_unlock(&server_info_mtx);
			file_entry_queue *qdata;
			
			if((qdata = malloc(sizeof(file_entry_queue))) == NULL){
				perror("malloc");
				int errno_copy = errno;
				fprintf(stderr,"FATAL ERROR: malloc\n");
				exit(errno_copy);
			}
			qdata->pnt = data;
			qdata->path = path;
			if (push(file_queue, qdata) == -1) {
				fprintf(stderr, "Errore: push\n");
				pthread_exit(NULL); //controlla se va bene 
			}
			
		}
		
		#ifdef DEBUG
			fprintf(stderr,"---OPEN CREATE LOCKED FILE\n");
			print_deb(hash_table);
		#endif
		
		return 0;
	}
	
int closeF(char *path, int fd){
	file_info *tmp;
	int n;
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		fprintf(stderr, "Impossibile chiudere file non esistente\n");
		n = 21;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
	
		if(writen(fd, "Err:fileNonEsistente", n) == -1)
			perror("write server closeF");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->open_owners == NULL || find_elm(tmp->open_owners, fd) != 0){
		fprintf(stderr, "Impossibile chiudere file che non si è prima aperto\n");
		int n = 18;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		
		if(writen(fd, "Err:fileNonAperto", n) == -1)
			perror("write server closeF");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
		
		remove_elm(&(tmp->open_owners), fd);
						
		if(tmp->lock_owner == fd)
			tmp->lock_owner = -1;
		tmp->lst_op = 0;
		Pthread_mutex_unlock(&ht_mtx);
		
		int n = 3;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			return -1;
		}
		if(writen(fd, "Ok", n) == -1){
			perror("write server openFC");
			return -1;
		}
	}
	#ifdef DEBUG
		fprintf(stderr,"---CLOSE FILE \n");
		print_deb();
	#endif
	
	return 0;
}

int readF(char *path, int fd){
	file_info *tmp;		
	int n;

	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		fprintf(stderr, "Impossibile leggere file non esistente\n");
		n = 21;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "Err:fileNonEsistente", n) == -1)
			perror("write server readF");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->open_owners == NULL || find_elm(tmp->open_owners, fd) != 0){
		fprintf(stderr, "Impossibile leggere file non aperto\n");
		n = 18;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "Err:fileNonAperto", n) == -1)
			perror("write server readF");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->lock_owner != fd && tmp->lock_owner != -1 ){
		fprintf(stderr, "Impossibile leggere file con lock attiva\n");
		n = 15;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "Err:fileLocked", n) == -1)
			perror("write server readF");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
		tmp->lst_op = 0;
		
		if(writen(fd, &tmp->cnt_sz, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(tmp->cnt_sz > 0){
			if(writen(fd, tmp->cnt, tmp->cnt_sz) == -1){
				perror("write server readF");
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}
		}
		Pthread_mutex_unlock(&ht_mtx);
	}
	Pthread_mutex_lock(&log_mtx); 
	fprintf(log_file,"%s %s -1 %d %lu\n", "readF", path, tmp->cnt_sz, pthread_self());
	Pthread_mutex_unlock(&log_mtx);
			
	#ifdef DEBUG
		fprintf(stderr,"---READ FILE \n");
		print_deb();
	#endif
	//free(msg);
	
	return 0;
			
}

int writeF(char *path, char *cnt, int sz, int fd){
	file_info *tmp;	
	int n;
	int found = 0;
	Pthread_mutex_lock(&ht_mtx);
	
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		fprintf(stderr, "Impossibile scrivere su file non esistente\n");
		n = 21;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "Err:fileNonEsistente", n) == -1)
			perror("write server writeF");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->open_owners == NULL || find_elm(tmp->open_owners, fd) != 0){
		fprintf(stderr, "Impossibile scrivere su file non aperto\n");
		n = 18;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "Err:fileNonAperto", n) == -1)
			perror("write server writeF");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if (tmp->lst_op == 0) { //forse dovrei usare l'update insert 
		fprintf(stderr, "L'operazione precedente a writeFile deve essere openFile\n");
		n = 18;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "Err:lastOperation", n) == -1)
			perror("write server writeF");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->lock_owner != fd){ //questo me lo assicura l'if precedente in realtà 
		fprintf(stderr, "Impossibile scrivere su file senza lock\n");
		n = 18;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "Err:fileSenzaLock", n) == -1)
			perror("write server writeF");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
		if(cnt != NULL){
			file_node *list = NULL;

			int res = st_repl(sz, fd, path, &list, 2);
			
			if(res == -1)
				fprintf(stderr, "Errore in repl su WriteFile\n");
			if(res == -2){
				int n = 21;
				if(writen(fd, &n, sizeof(int)) == -1){
					perror("Errore nella write");
					Pthread_mutex_unlock(&ht_mtx);
					return -1;
				}
				if(writen(fd, "Err:fileTroppoGrande", n) == -1){
					perror("write server writeF");
					Pthread_mutex_unlock(&ht_mtx);
					return -1;
				}
				Pthread_mutex_unlock(&ht_mtx);
				return -3; //vedi poi come modificare dato che non vuoi interrompere la lettura per questo 
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
			file_node *prec = NULL;
			if(aux != NULL){	
				while(aux != NULL){
					prec = aux;
					file_sender(aux, fd);

					if(icl_hash_delete(hash_table, aux->path, &free, &free_data_ht) == -1){ 
						fprintf(stderr, "Errore nella rimozione del file dallo storage, inrepl\n");
						 pthread_cond_broadcast(&ht_cond);
						Pthread_mutex_unlock(&ht_mtx);
						return -1;
					}	
					aux = aux->next;
					free(prec);
				}
				found = 1;
			}
			
			if(sz > 0){
				
				memcpy(tmp->cnt, cnt, sz);
				Pthread_mutex_lock(&server_info_mtx);
				st_dim = st_dim - tmp->cnt_sz + sz;  
				Pthread_mutex_unlock(&server_info_mtx);
			    tmp->cnt_sz = sz;
			}
		}
		else{
			#ifdef DEBUG
				fprintf(stderr, "richiesta scrittura di file vuoto\n");
			#endif
		}
			if(found) pthread_cond_broadcast(&ht_cond);
		Pthread_mutex_unlock(&ht_mtx);
		
		Pthread_mutex_lock(&log_mtx); 
		fprintf(log_file,"%s %s %d -1 %lu\n", "writeF", path, sz, pthread_self());
		Pthread_mutex_unlock(&log_mtx);
		
		int n = 2;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			return -1;
		}
		if(writen(fd, "$", n) == -1){
			perror("write server writeF");
			return -1;
		}

		n = 3;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			return -1;
		}
		if(writen(fd, "Ok", n) == -1){
			perror("write server writeF");
			return -1;
		}

	}
	
	#ifdef DEBUG
		fprintf(stderr,"---WRITE FILE \n");
		print_deb();
	#endif
		
	
	return 0;
}

int lockF(char *path, int fd){
	file_info *tmp;	
    
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		fprintf(stderr, "Impossibile fare lock su file non esistente\n");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
		while((tmp->lock_owner != -1 && tmp->lock_owner != fd)){
			pthread_cond_wait(&ht_cond,&ht_mtx);
			if((tmp = icl_hash_find(hash_table, path)) == NULL){ //il file potrebbe essere stato rimosso dal detentore della lock 
				fprintf(stderr, "Impossibile fare lock su file non esistente\n");
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}
		}
		
		tmp->lock_owner = fd;
		tmp->lst_op = 0; //vedi se crea problemi alla openfile(O_create|o_lock)
	}
	Pthread_mutex_unlock(&ht_mtx);
	
	#ifdef DEBUG
		fprintf(stderr,"---LOCK FILE \n");
		print_deb();
	#endif
		
	
	return 0;

}

//lock e unlock sono gestite in modo diverso 

int unlockF(char *path, int fd){
	file_info *tmp;
	int n;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		fprintf(stderr, "Impossibile fare lock su file non esistente\n");
		n = 21;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "Err:fileNonEsistente", n) == -1)
			perror("write server unlockF");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
    }

	else if(tmp->lock_owner != fd){
		fprintf(stderr, "Impossibile fare unlock su file di cui non si ha la lock\n");
		n = 11;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "Err:Nolock", n) == -1)
			perror("write server unlockF");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
		tmp->lock_owner = -1;
		tmp->lst_op = 0;
		
        pthread_cond_broadcast(&ht_cond);
        Pthread_mutex_unlock(&ht_mtx);
		n = 3;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			return -1;
		}
		if(writen(fd, "Ok", n) == -1){
			perror("write server unlockF");
			return -1;
		}
	}
	#ifdef DEBUG
		fprintf(stderr,"---UNLOCK FILE \n");
		print_deb();
	#endif
		
	
	return 0;
}

int appendToF(char *path, char *cnt, int sz, int fd){
	file_info *tmp;	
	int res;
	int n, old_errno;
	int found = 0;
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		fprintf(stderr, "Impossibile scrivere su file non esistente\n");
		n = 21;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "Err:fileNonEsistente", n) == -1)
			perror("write server appendToF");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->open_owners == NULL || find_elm(tmp->open_owners, fd) != 0){
			fprintf(stderr, "Impossibile scrivere su file non aperto\n");
			n = 18;
			if(writen(fd, &n, sizeof(int)) == -1){
				perror("Errore nella write");
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}
			if(writen(fd, "Err:fileNonAperto", n) == -1)
				perror("write server appendToF");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
	else if(tmp->lock_owner != fd){
			fprintf(stderr, "Impossibile scrivere su file senza lock\n");
			n = 18;
			if(writen(fd, &n, sizeof(int)) == -1){//le write dovranno essere riviste (atomicità)
				perror("Errore nella write");
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}
			if(writen(fd, "Err:fileSenzaLock", n) == -1)
				perror("write server appendToF");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
	else{
		if(cnt != NULL){
			file_node *list = NULL;
			res = st_repl(sz, fd, path, &list, 1);
			
				
			if(res == -1)
				fprintf(stderr, "Errore in repl su AppendToFile\n");
			else if(res == -2){
				n = 21;
				if(writen(fd, &n, sizeof(int)) == -1){
					old_errno = errno;
					perror("Errore nella write");
					
					file_node *aux = list;
					file_node *prec = NULL;
					if(aux != NULL){
						while(aux != NULL){
							file_sender(aux, fd);
							prec = aux;
							
							if(icl_hash_delete(hash_table, aux->path, &free, &free_data_ht) == -1){ 
								fprintf(stderr, "Errore nella rimozione del file dallo storage, inrepl\n");
								pthread_cond_broadcast(&ht_cond);
								Pthread_mutex_unlock(&ht_mtx);
								return -1;
							}			
											
							aux = aux->next;
							free(prec);
							found = 1;
						}
					}
					if(found) pthread_cond_broadcast(&ht_cond);
					Pthread_mutex_unlock(&ht_mtx);
					errno = old_errno;
					return -1;
				}
				if(writen(fd, "Err:fileTroppoGrande", n) == -1)
					perror("write server appendToF, fileTroppoGrande");
				old_errno = errno;
				file_node *aux = list;
				file_node *prec = NULL;
				if(aux != NULL){
					while(aux != NULL){
						file_sender(aux, fd);
						prec = aux;
						
						if(icl_hash_delete(hash_table, aux->path, &free, &free_data_ht) == -1){ 
							fprintf(stderr, "Errore nella rimozione del file dallo storage, inrepl\n");
							 pthread_cond_broadcast(&ht_cond);
							Pthread_mutex_unlock(&ht_mtx);
							return -1;
						}			
										
						aux = aux->next;
						free(prec);
					}
					found = 1;
				}
				if(found) pthread_cond_broadcast(&ht_cond);
				Pthread_mutex_unlock(&ht_mtx);
				errno = old_errno;
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
				file_node *prec = NULL;
				if(aux != NULL){
					while(aux != NULL){
						file_sender(aux, fd);
						prec = aux;
						
						if(icl_hash_delete(hash_table, aux->path, &free, &free_data_ht) == -1){ 
							fprintf(stderr, "Errore nella rimozione del file dallo storage, inrepl\n");
							 pthread_cond_broadcast(&ht_cond);
							Pthread_mutex_unlock(&ht_mtx);
							return -1;
						}			
										
						aux = aux->next;
						free(prec);
					}
					found = 1;
				}
				
			}
			if(tmp->cnt_sz > 0){ //prima era >= 
				memcpy(tmp->cnt + tmp->cnt_sz, cnt, sz);
				tmp->cnt_sz += sz;
				Pthread_mutex_lock(&server_info_mtx);
				st_dim +=sz; 
				Pthread_mutex_unlock(&server_info_mtx);
			}
			 
	    }
		else
			fprintf(stderr, "richiesta scrittura di file vuoto\n");
		
		if(found) pthread_cond_broadcast(&ht_cond);
		Pthread_mutex_unlock(&ht_mtx);
		
		Pthread_mutex_lock(&log_mtx); 
		fprintf(log_file,"%s %s %d -1 %lu\n", "append", path, sz, pthread_self());
		Pthread_mutex_unlock(&log_mtx);
		
		int n = 2;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			return -1;
		}
		if(writen(fd, "$", n) == -1){
			perror("write server writeF");
			return -1;
		}
		n = 3;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			return -1;
		}
		
		if(writen(fd, "Ok", n) == -1){
			perror("write server appendToF");
			return -1;
		}		
	}
	#ifdef DEBUG
		fprintf(stderr,"---APPEND TO FILE \n");
		print_deb();
	#endif
		
	
	return 0;
}

int removeF(char *path, int fd){ 
	file_info *tmp;
	int old_sz, n;
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		fprintf(stderr, "Impossibile rimuovere file non esistente\n");
		n = 22;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "fail:fileNonEsistente", n) == -1)
			perror("write server removeF");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->lock_owner != fd){
		fprintf(stderr, "Impossibile rimuovere file senza lock\n");
		n = 19;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, "fail:fileSenzaLock", n) == -1)
			perror("write server removeF");
		Pthread_mutex_unlock(&ht_mtx);
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
				found = 1;
				free(del);
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
		
		
		if(icl_hash_delete(hash_table, path, &free, &free_data_ht) == -1){ 
			fprintf(stderr, "Errore nella rimozione del file dallo storage\n");
			if(writen(fd, "Err:rimozione", 14) == -1)
				perror("write server removeF");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		
		
		pthread_cond_broadcast(&ht_cond); //sveglio chi era in attesa della lock 
        Pthread_mutex_unlock(&ht_mtx);

		Pthread_mutex_lock(&server_info_mtx);
		n_files--; 
		st_dim -= old_sz; 
		Pthread_mutex_unlock(&server_info_mtx);
		
		Pthread_mutex_lock(&log_mtx); 
		fprintf(log_file,"%s %s -1 %d %lu\n", "remove", path, old_sz, pthread_self());
		Pthread_mutex_unlock(&log_mtx);
		
		n = 3;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			return -1;
		}
		if(writen(fd, "Ok", n) == -1){
			perror("write server removeF");
			return -1;
		}
		
		#ifdef DEBUG
			fprintf(stderr,"---REMOVE FILE\n");
			print_deb();
		#endif 
	}
	return 0;
}


//queste chiamate non devono aprire e chiudere il file
int readNF(int fd, int n){
	    
	icl_entry_t *entry;
	char *key, *value;
	int k;
	int all = 0;
	file_node *file;
	
	
	if(n <= 0){
		all = 1;
		n = 1;
	}
	Pthread_mutex_lock(&ht_mtx);
	if (hash_table) {
		file_info *aux;
		for (k = 0; k < hash_table->nbuckets && n > 0; k++)  {
			for (entry = hash_table->buckets[k]; entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL) && n > 0; entry = entry->next){
				if((aux = icl_hash_find(hash_table, key)) != NULL){
				
				if((file = malloc(sizeof(file_node))) == NULL){
					perror("malloc");
					int errno_copy = errno;
					fprintf(stderr,"FATAL ERROR: malloc\n");
					Pthread_mutex_unlock(&ht_mtx);
					exit(errno_copy);
				}
				if((file->path = malloc(sizeof(char)*(strlen(key)+1))) == NULL){
					perror("malloc");
					int errno_copy = errno;
					fprintf(stderr,"FATAL ERROR: malloc\n");
					Pthread_mutex_unlock(&ht_mtx);
					free(file);
					exit(errno_copy);
				}
				
				strncpy(file->path, key, strlen(key)+1);
				file->cnt_sz = aux->cnt_sz;
		
				
				if(file->cnt_sz > 0){
					memset(file->cnt, 0, MAX_SIZE);
					memcpy(file->cnt, aux->cnt, file->cnt_sz);
				}
				
				
				if(file_sender(file, fd) != -1){
					Pthread_mutex_lock(&log_mtx); 
					fprintf(log_file,"%s %s -1 %d %lu\n", "readNF", key, file->cnt_sz, pthread_self());
					Pthread_mutex_unlock(&log_mtx);
				}
				
				if(!all)
					n--;
				
				free(file->path);
				free(file);
				}
			}
		}
	}
	else{
		fprintf(stderr, "hash table vuota\n");
	}
	Pthread_mutex_unlock(&ht_mtx);
	

    n = 2;
	if(writen(fd, &n, sizeof(int)) == -1){
		perror("Errore nella write");
		return -1;
	}
	if(writen(fd, "$", n) == -1){
		perror("write server readNF");
		return -1;
	}
	
	n = 3;
	if(writen(fd, &n, sizeof(int)) == -1){
		perror("Errore nella write");
		return -1;
	}
	if(writen(fd, "Ok", n) == -1){
		perror("write server writeF");
		return -1;
	}
   
	#ifdef DEBUG
		fprintf(stderr,"---SEND N FILES\n");
		print_deb(hash_table);
	#endif
	
	
	return 0;			
}

int st_repl(int dim, int fd, char *path, file_node **list, int app_wr){
	file_entry_queue *exp_file;
	file_info *tmp;
	int found = 0;
	int res = 0;
	int old_sz;
	Pthread_mutex_lock(&server_info_mtx);
	if(dim > st_dim_MAX || st_dim_MAX == 0 || n_files_MAX == 0){
		fprintf(stderr, "Non è possibile rimpiazzare file, %d > %d, n_files_MAX : %d\n", dim, st_dim_MAX, n_files_MAX);
		Pthread_mutex_unlock(&server_info_mtx);
		return -2;
    }
	
	if(app_wr < 0 && n_files + 1 > n_files_MAX ){
		Pthread_mutex_unlock(&server_info_mtx);
		if((exp_file = pop(file_queue)) == NULL){
			perror("st_repl, pop");
			free(exp_file);
			return -1;
		}
		if(strncmp(exp_file->path, path, UNIX_PATH_MAX) == 0){
				push(file_queue, exp_file);
				fprintf(stderr, "impossibile rimpiazzare file\n");
				return -2;
		}
		fprintf(stderr, "FILE DA RIMUOVERE: %s, len_q: %ld\n", exp_file->path, length(file_queue));
	
		if((tmp = icl_hash_find(hash_table, exp_file->path)) == NULL){ 
			fprintf(stderr, "Impossibile fare lock su file non esistente in repl\n");
			free(exp_file);
			return -1;
		}
		while((tmp->lock_owner != -1 && tmp->lock_owner != fd)){
			pthread_cond_wait(&ht_cond,&ht_mtx);
			if((tmp = icl_hash_find(hash_table, exp_file->path)) == NULL){ //il file potrebbe essere stato rimosso dal detentore della lock 
				fprintf(stderr, "Impossibile fare lock su file non esistente in repl\n");
				free(exp_file);
				return -1;
			}
		}
		
		tmp->lock_owner = fd;
		tmp->lst_op = 0; //vedi se crea problemi alla openfile(O_create|o_lock)

		old_sz = exp_file->pnt->cnt_sz;
				
		if(list != NULL)
			insert_head_f(list, exp_file->path, old_sz, exp_file->pnt->cnt);
		
		Pthread_mutex_lock(&server_info_mtx);
		n_files--; 
		st_dim = st_dim - old_sz; 
		fprintf(stderr, "n_files: %d, st_dim: %d\n", n_files, st_dim);
		Pthread_mutex_unlock(&server_info_mtx);
		
		Pthread_mutex_lock(&log_mtx); //vedi se modificarle con la maiuscola (funzione util)
		fprintf(log_file,"%s %s -1 %d %lu\n", "rimpiazzamento",  exp_file->path, old_sz, pthread_self());
		Pthread_mutex_unlock(&log_mtx);
		
		free(exp_file);
			
	}
	else
		Pthread_mutex_unlock(&server_info_mtx);
	
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		fprintf(stderr, "Impossibile fare lock su file non esistente in repl\n");
		return -1;
	}
	
	Pthread_mutex_lock(&server_info_mtx);
	
	if(app_wr == 1){
		if(dim + tmp->cnt_sz > st_dim_MAX){
			fprintf(stderr, "!!!!!!!!!Non è possibile rimpiazzare file per fare l'append, %d > %d, n_files_MAX : %d\n", dim+tmp->cnt_sz, st_dim_MAX, n_files_MAX);
			Pthread_mutex_unlock(&server_info_mtx);
			return -2;
		}
	}
			
	while(st_dim + dim > st_dim_MAX && length(file_queue) > 0){
		
		Pthread_mutex_unlock(&server_info_mtx);
		
		if((exp_file = pop(file_queue)) == NULL){
			perror("st_repl, pop");
			free(exp_file);
			return -1;
		}
		if(strncmp(exp_file->path, path, UNIX_PATH_MAX) == 0){
			if(!found){
				push(file_queue, exp_file);
				found = 1;
				continue;
			}
			else{
				fprintf(stderr, "impossibile rimpiazzare file\n");
				free(exp_file);
				return -2;
			}
		}
		
			
		fprintf(stderr, "FILE DA RIMUOVERE: %s, len_q: %ld\n", exp_file->path, length(file_queue));
		
		if((tmp = icl_hash_find(hash_table, exp_file->path)) == NULL){ 
			fprintf(stderr, "Impossibile fare lock su file non esistente in repl\n");
			free(exp_file);
			return -1;
		}
		else{
			while((tmp->lock_owner != -1 && tmp->lock_owner != fd)){
				pthread_cond_wait(&ht_cond,&ht_mtx);
					if((tmp = icl_hash_find(hash_table, exp_file->path)) == NULL){ //il file potrebbe essere stato rimosso dal detentore della lock 
						fprintf(stderr, "Impossibile fare lock su file non esistente in repl\n");
						free(exp_file);
						return -1;
					}
			}
			
			tmp->lock_owner = fd;
			tmp->lst_op = 0; 
		}
		
		old_sz = exp_file->pnt->cnt_sz;
		if(list != NULL)
			insert_head_f(list, exp_file->path, old_sz, exp_file->pnt->cnt);
		
		
		
		
		/*if(icl_hash_delete(hash_table, exp_file->path, &free, &free_data_ht) == -1){ 
			fprintf(stderr, "Errore nella rimozione del file dallo storage, in repl\n");
			free(exp_file);
			return -1;
		}*/
		Pthread_mutex_lock(&server_info_mtx);
		n_files--;
		st_dim = st_dim - old_sz; 
		Pthread_mutex_unlock(&server_info_mtx);
		
		Pthread_mutex_lock(&log_mtx); 
		fprintf(log_file,"%s %s -1 %d %lu\n", "rimpiazzamento",  exp_file->path, old_sz, pthread_self());
		Pthread_mutex_unlock(&log_mtx);
		
		free(exp_file);
		
		Pthread_mutex_lock(&server_info_mtx);
		
	}
		
	if(st_dim + dim > st_dim_MAX){
		Pthread_mutex_unlock(&server_info_mtx);
		fprintf(stderr, "Non è possibile rimpiazzare file\n");
		return -2;
    }
	Pthread_mutex_unlock(&server_info_mtx);
		
	return res;
}

int file_sender(file_node *file, int fd){
		
	int msg_sz;
	
	msg_sz = strlen(file->path) +1;
	if(writen(fd, &msg_sz, sizeof(int)) == -1){
		perror("Errore nella write1");
		return -1;
	}
	//fprintf(stderr, "path: %s, msg_sz: %d\n", file->path, msg_sz);
	if(writen(fd, file->path , msg_sz) == -1){
		perror("Errore nella write2");
		return -1;
	}
	
	//fprintf(stderr, "path: %s, msg_sz: %d\n", file->path, msg_sz);
	if(writen(fd, &(file->cnt_sz), sizeof(int)) == -1){
		perror("Errore nella write3");
		return -1;
	}
	//fprintf(stderr, "Tutto bene: file: %s,  %d\n", file->path, file->cnt_sz);
	if(file->cnt_sz > 0){
		//fprintf(stderr, "path: %s, cnt_sz: %d, cnt: %s\n", file->path, msg_sz, file->cnt);
	
		if(writen(fd, file->cnt , file->cnt_sz) == -1){
			perror("Errore nella write4");
			return -1;
		}
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