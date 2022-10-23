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

#define DEBUG2
#undef DEBUG2

#define UNIX_PATH_MAX 108 //lunghezza massima path
#define MAX_SIZE 10000 //lunghezza richiesta al server, riga log_file, contenuto file max memorizzato su server, vedi se quest'ultimo lo puoi rendere più flessibile (però ricorda che readF ti dà un buffer di taglia MAX_SIZE, vedi se lo devi riallocare in caso 

#define SOCKNAME "/home/giulia/File_server_storage/Server_storage/mysock" 
#define NBUCKETS  10 // n. di file max nello storage 
#define ST_DIM 256 //capacità storage  
#define THREAD_NUM 4
#define LOGNAME "/home/giulia/File_server_storage/Server_storage/log" 

#define MAX_REQ   20 //n. max di richieste in coda 
#define EOS (void*)0x1 //valore speciale usato per indicare End-Of-Stream (EOS)

//vedi se usare exit o pthread exit, nel caso di malloc uccido tutto il programma o solo il thread?

//NB: quando client si disconnette non posso fare a meno di fare un controllo in tutto lo storage 
//    per ripulirlo dalle lock/open ownership di quel client 
//    non sono riuscita a trovare un modo più efficiente
//    potrei fare che ogni client ha una struttura dati dove al suo descr
//    è associata lista di file aperti/lockati e alla sua disconnessione scorro
//    quella lista.. tuttavia devo gestire questa struttura 

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

//guarda valori di ritorno queue 
//ricordati di pensare alle lock su l'hashtable 

typedef struct open_node{  //struttura dei nodi nella lista open_owners di un file
    int fd;
    struct open_node *next;
} open_node;

typedef struct file_info{ //struttura dei valori nell'hashtable (storage)
	int lock_owner;
	open_node *open_owners;
	int lst_op; //controllo per la writeFile
	char cnt[MAX_SIZE]; //con char * ho maggiore flessibilità, ma più "difficile" gestione cnt (allocare/deallocare memoria, realloc buff di readF) (scrivilo sulla relazione e gestiscilo (tento di scrivere/appendere file troppo grande)
	int cnt_sz;  
	pthread_cond_t cond;
} file_info;

typedef struct file_repl{ //entry della coda per i rimpiazzamenti 
	file_info *pnt;
	char *path;
	struct file_repl *next;
} file_repl;

typedef struct th_sig_arg {  //argomento thread signal_handler 
    sigset_t *mask;
    int pipe_s;
} th_sig_arg;

typedef struct th_w_arg {  //argomento thread workers
    Queue_t *task_queue;
    int pipe_w;
} th_w_arg;

void print_deb();
int isNumber(void *el, int *n);
int st_repl(int dim, int fd, char *path, file_repl **list, int isCreate);
int file_sender(file_repl *list, int fd);
int print_summ(char *log);

file_info *init_file(int lock_owner, int fd, int lst_op, char *cnt, int sz);

int insert_head(open_node **list, int info);
int remove_elm(open_node **list, int info);
int find_elm(open_node *list, int elm);
void print_list(open_node *list);
void free_data_ht(void *data);
int clean_storage (int fd);

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


//variabili globali  [ controlla se potevano essere definite localmente e in caso contrario se necessitano o meno di lock]

Queue_t *files_queue; //coda per gestire rimpiazzamento (char **)

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
    sigset_t *set = ((th_sig_arg *)arg)->mask; //i segnali per cui sono in attesa (mascherati dal chiamante)
	int pipe_s = ((th_sig_arg *)arg)->pipe_s;

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

				rit = 1;
				if(writen(pipe_s, &rit, sizeof(int)) == -1)
					perror("Write sig_handler");
				
				// SOLUZIONE ALTERNATIVA CHE NON USA shutdown:
				// Il thread dispatcher (in questo esercizio il main thread) invece di sospendersi su una accept si
				// sospende su una select in cui, oltre al listen socket, registra il discrittore di lettura di una
				// pipe condivisa con il thread sigHandler. Il thead sigHandler quando riceve il segnale chiude il
				// descrittore di scrittura in modo da sbloccare la select.	    	    
				return NULL;
			case SIGHUP: 
				rit = -1;
				if(writen(pipe_s, &rit, sizeof(int)) == -1)
					perror("Write sig_handler");
				
				return NULL;  //va bene? O devo rimanere in attesa di un possibile sigint?
			default:  ; 
		}
    }
    return NULL;	   
}

int executeTask(int fd_c){ //qui faccio la read, capisco cosa devo fare, lo faccio chiamando la giusta funzione, rispondo al client, scrivo fd sulla pipe per il server  
		
	char *msg; 
	char *tmpstr = NULL;
    char *token, *token2;
	//int seed = time(NULL);
	int n, sz_msg;
		
	if((msg = malloc(MAX_SIZE*sizeof(char))) == NULL){ //!!attualmente non funziona mettendoci la size letta con la prima readn 
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		exit(errno_copy); 
	}
	memset(msg, '\0', MAX_SIZE);
	if(readn(fd_c, &sz_msg, sizeof(int)) == -1){
		perror("read socket lato server");
		free(msg);
		return -1;    
	}
	if(readn(fd_c, msg, sz_msg) == -1){
		perror("read socket lato server");
		free(msg);
		return -1;   
	}
	
	#ifdef DEBUG
		fprintf(stderr, "len repl_queue: %d\n", (int) length(files_queue));
		fprintf(stderr, "N_FILES: %d / %d, ST_DIM: %d / %d\n", n_files, n_files_MAX, st_dim, st_dim_MAX);
		fprintf(stderr, "\n*********Contenuto canale di comunicazione: %s*******\n\n", msg);
	#endif
	
	token = strtok_r(msg, ";", &tmpstr);
		
	if(token != NULL){
		
		if(strcmp(token, "openfc") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			
			if(openFC(token, fd_c) == -1){
				free(msg);
				return -1;
			} 

			Pthread_mutex_lock(&log_mtx); 
			fprintf(log_file,"%s %s -1 -1 %lu\n", "open_c", token, pthread_self());
			fflush(log_file);
			Pthread_mutex_unlock(&log_mtx);				   	
		}
		else if(strcmp(token, "openfl") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			int res;
			if((res = openFL(token, fd_c)) < 0){
				if(res == -1){
					free(msg);
					return -1;
				}
				else{
					; //ho deciso di evitare stampe di errore nel caso di file non esistente, per non appesantire l'output 
				}
			}
			else{
				Pthread_mutex_lock(&log_mtx); 
				fprintf(log_file,"%s %s -1 -1 %lu\n", "open_l", token, pthread_self());
				fflush(log_file);
				Pthread_mutex_unlock(&log_mtx);
			}
			
		}
		else if(strcmp(token, "openfo") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(openFO(token, fd_c) == -1){
				free(msg);
				return -1;
			}
			Pthread_mutex_lock(&log_mtx);
			fprintf(log_file,"%s %s -1 -1 %lu\n", "open_o", token, pthread_self());
			fflush(log_file);
			Pthread_mutex_unlock(&log_mtx);
			
		}
		else if(strcmp(token, "openfcl") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			
			if(openFCL(token, fd_c) == -1){
				free(msg);
				return -1;
			}
			
			Pthread_mutex_lock(&log_mtx); 
			fprintf(log_file,"%s %s -1 -1 %lu\n", "open_cl", token, pthread_self());
			fflush(log_file);
			Pthread_mutex_unlock(&log_mtx);
			
		}
		else if(strcmp(token, "disconnesso") == 0){//ancora da chiudere i file e aggiornarli
			
			//devo creare una funz che scorre lo storage e toglie ovunque lock_owner (e open_owner, anche se questo risolto lato client, guarda client.c)
			//oppure lato client mi tengo una lista di file dove ho la lock ownership e faccio una serie di richieste di unlock al server 
			
			clean_storage(fd_c);
			
			close(fd_c); //non so se farlo qui o in run_server, forse meglio in run_server 
			
			Pthread_mutex_lock(&log_mtx); //vedi se modificarle con la maiuscola (funzione util)
			fprintf(log_file,"%s %d -1 -1 %lu\n", "close_connection", fd_c, pthread_self());
			fflush(log_file);
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
			Pthread_mutex_lock(&log_mtx); 
			fprintf(log_file,"%s %s -1 -1 %lu\n", "close", token, pthread_self());
			fflush(log_file);
			Pthread_mutex_unlock(&log_mtx);	
		}
		else if(strcmp(token, "readf") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(readF(token, fd_c) == -1){
				free(msg);
				return -1;
			}
			//aggiornamento file di log avviene in readF
			
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
			//aggiornamento file di log avviene in writeF
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
					if(writen(fd_c, &n, sizeof(int)) == -1){
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
					if(writen(fd_c, &n, sizeof(int)) == -1){
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
				Pthread_mutex_lock(&log_mtx); 
				fprintf(log_file,"%s %s -1 -1 %lu\n", "lock", token, pthread_self());
				fflush(log_file);
				Pthread_mutex_unlock(&log_mtx);
			}
			
		}
		else if(strcmp(token, "unlockf") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(unlockF(token, fd_c) == -1){
				free(msg);
				return -1;
			}
			Pthread_mutex_lock(&log_mtx); 
			fprintf(log_file,"%s %s -1 -1 %lu\n", "unlock", token, pthread_self());
			fflush(log_file);
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
			//aggiornamento file di log avviene in appendToF
		}
		else if(strcmp(token, "removef") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			if(removeF(token, fd_c) == -1){
				free(msg);
				return -1;
			}
			//aggiornamento file di log avviene in removeF
			
		}
		else if(strcmp(token, "readNf") == 0){
			token2 = strtok_r(NULL, ";", &tmpstr);
			if(isNumber(token2, &n) == 0){
				if(readNF(fd_c, n) == -1){
					free(msg);
					return -1;
				}
			}
			else
				fprintf(stderr, "il parametro di readNFiles deve essere un numero [server]\n");
			//aggiornamento file di log avviene in readNF
		}
		else{
			fprintf(stderr, "Opzione non ancora implementata: %s\n", token);
			int m = strlen("Ok");
			if(writen(fd_c, &m, sizeof(int)) == -1){
				perror("write server execute task opz sconosciuta");
				free(msg);
				return -1;
			}
			if(writen(fd_c, "Ok", m) == -1){
				perror("write server execute task opz sconosciuta");
				free(msg);
				return -1;
			}
		}	
	}
	#ifdef DEBUG
		fprintf(stderr,"\n---HASHTABLE:\n");
		print_deb();
	#endif	
	
	free(msg); 

	return 0;
}

static void *init_thread(void *arg){ //CONSUMATORE
	int res;
	int descr;
	Queue_t *task_queue = ((th_w_arg *)arg)->task_queue; 
	int pipe = ((th_w_arg *)arg)->pipe_w;
	
	while(1){ //considera bene questa storia degli errori in executeTask, soprattutto perchè devi assicurarti di chiudere la connessione con il client se necessario (attualmente credo venga chiusa solo in caso di disconnect)
	
		
		int  *fd = pop(task_queue);	
		
		if(fd == NULL){
			fprintf(stderr,"Errore pop, init thread\n");
			pthread_exit(NULL); //da modificare 
		}
		if(fd == EOS) 
            return (void*) 0;
		
		descr = *fd;
		res = executeTask(*fd);

		free(fd);
		if( res == -1){ //errori non fatali
			#ifdef DEBUG2
				fprintf(stderr, "Impossibile servire richiesta client %d\n", descr);
			#endif
		}
		
		if( res == -2 ){ //disconnessione client fd 
			descr = descr*-1;
		}
		if( res == -3){ //errori fatali, attualmente nemmeno uno in executeTask, vedi se eliminarlo 
			descr = descr*-1;
			if(write(pipe, &descr, sizeof(int)) == -1){
				perror("write init_thread su pipe");
				return NULL; //In questo caso è ok? controlla pthread_create 
			}
			pthread_exit(NULL); //perchè p_thread exit? vorrei chiudere proprio processo forse..
		}

		if(write(pipe, &descr, sizeof(int)) == -1){
			perror("write init_thread su pipe");
			return NULL; //In questo caso è ok? controlla pthread_create 
		}

	}
	
	return (void *) 0; 
}

void *submitTask(int descr, Queue_t *task_queue){ //PRODUTTORE
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

int run_server(struct sockaddr_un *sa, int pipe, int sig_pipe, Queue_t *task_queue){
	
	int stop = 0;
	int fd_skt, fd_c, fd_r = 0;
	int fd_num = 0; //si ricorda il max fd attivo 
    int	fd; //indice per select
	int err = 0;
	
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
	
	if((listen(fd_skt, MAX_REQ)) == -1){
		perror("Errore nella listen");
		return -1;
	} 

	//aggiorno fd_num e set 
	
	if(fd_skt > fd_num) //mi voglio ricordare l'indice max tra i descrittori attivi
		fd_num = fd_skt;
		
	if(pipe > fd_num)
		fd_num = pipe;
	
	if(sig_pipe > fd_num)
		fd_num = sig_pipe;
	
	FD_ZERO(&set); //per evitare errori prima lo azzero
	FD_ZERO(&rdset); //per evitare errori prima lo azzero

	//fprintf(stderr, "fd_skt = %d, pipe_mw = %d, sig_pipe = %d\n", fd_skt, pipe, sig_pipe);
	FD_SET(fd_skt, &set); //metto a 1 il bit di fd_skt, fd_skt pronto
	FD_SET(pipe, &set);
	FD_SET(sig_pipe, &set);
    	
	//select in cui, oltre al listen socket, registra il discrittore di lettura di una
	// pipe condivisa con il thread sigHandler.
	
	//inizio il ciclo di comunicazione con i workers 
	int n_conn = 0;
	int sighup = 0;
	
	while(!stop && !err){
		int res;
		//fprintf(stderr, "ITERAZIONE: i: %d, pipe: %d, skt: %d, fd_num: %d, nuovo fd_r: %d \n", i, pipe, fd_skt, fd_num, fd_r);
		
		rdset = set; //aggiorno ogni volta la maschera per la select, ne servono due perchè select modifica quello che gli passi
		if(sighup)  //non ci sono più descrittori di connessioni attive dopo aver ricevuto sighup 
			if(n_conn == 0)
				break;
		
		if((res = select(fd_num+1, &rdset, NULL, NULL, NULL)) == -1){ //si blocca finchè uno tra i descrittori in rdset non si blocca più su una read
			perror("select");
			err = 1; //controlla se è corretto interrompere il server se accept non va a buon fine
		}

		/*if(res == 0){ //non ho specificato il timeout tanto
			stop = 1;
		}*/
		//appena la select mi dice che c'è un worker pronto (descrittore client libero), inizio a scorrerli
		
		//NB: fd_skt è il lissten socket del server dove attende le connessioni dei nuovi client
		//    fd_c è il socket di comunicazione con uno specifico client dopo che si sono connessi 
		
		for(fd = 0; fd <= fd_num; fd++){ //guardo tutti i descrittori nel nuovo rdset (aggiornato da select)
			
			if(FD_ISSET(fd, &rdset)){ //controllo quali di questi è pronto in lettura
			
				if(fd == fd_skt){ //se è il listen socket faccio accept [richiesta connessione da nuovo client]
					//fprintf(stderr, "ho letto il SOCKET, faccio accept\n");
					if(!sighup){  //se non c'è stato il segnale sighup accetto nuove connessioni 
						if((fd_c = accept(fd_skt, NULL, 0)) == -1){ //[creo canale di comunicazione con il client]
							perror("accept");
							err = 1; //controlla se va bene interrompersi se accept non va a buon fine 
						}
							
						//fprintf(stderr, "ACCEPT fatta, %d è il fd_c del client\n", fd_c);
						
						Pthread_mutex_lock(&log_mtx); 
						fprintf(log_file,"%s %d -1 -1 %lu\n", "open_connection", fd_c, pthread_self());
						fflush(log_file);
						Pthread_mutex_unlock(&log_mtx);
					   
						FD_SET(fd_c, &set); //fd_c pronto (salvo nella maschera il canale di comunicazione tra uno specifico client e il server)
						if(fd_c > fd_num) 
							fd_num = fd_c;
						
						n_conn++;
					}
				}
				else{ //(altrimenti vado con la read)
				
					if(fd == pipe){ //se è la pipe (comunicaz master workers) aggiorno i descrittori
				
						if(readn(pipe, &fd_r, sizeof(int)) == -1){
							perror("READ PIPE MASTER:");
							err = 1;
						}
						if(fd_r < 0){ //attualmente il descrittore lo chiudo nel execute_task, forse sarebbe meglio qua 
							n_conn--; //chi chiude il descrittore fd_r?? 
							fd_r = fd_r*-1;
							FD_CLR(fd_r, &set); //tolgo il fd da set, forse basta non riaggiungercelo?  sì, se lasci clr sull'ultimo else 
							if(fd_r == fd_num) {
								if((fd_num = aggiorna_max(fd_r, set)) == -1)
									err = 1; 
							}
						}
						else{
							FD_SET(fd_r, &set); //il canale di comunicazione è pronto a ricevere nuove richieste 
							if(fd_r > fd_num) 
								fd_num = fd_r;
						}
						
						//fprintf(stderr, "read su pipe fatta, ho letto fd_r: %d\n", fd_r);
						
					}
					else if(fd == sig_pipe){ 
						
						if(readn(sig_pipe, &fd_r, sizeof(int)) != -1){
							if(fd_r > 0) //sigint, sigquit
								stop = 1;
							else
								sighup = 1;
						}
						else{
							perror("READ SIG_PIPE MASTER:");
							err = 1;
						}
					}
					else{ //altrimenti eseguo il comando richiesto dal descrittore [richiesta client]
						
						//fprintf(stderr, "ho letto un descrittore client fd: %d, faccio SUBMIT\n", fd);

						FD_CLR(fd, &set);  //lo tolgo sempre, in caso lo raggiungerò dopo quando il thread ha risposto alla richiesta 
						
						if(fd == fd_num) {
							if((fd_num = aggiorna_max(fd, set)) == -1){
								err = 1; 
							}
						}
						submitTask(fd, task_queue);  
					}					
				}
			}
		}
		
	}
	
	//qui chiudi fd_skt, le 2 pipe e descrittori di socket client ancora aperti 
	/*
	close(fd_skt); //pipe e sig_pipe le chiudo nel main 
	for(fd = 0; fd < fd_num+1; fd++){
		if(IS_SET(fd, &rdset) && fd != pipe && fd != sig_pipe && fd != fd_skt){
			CLR(fd, &rdset);
			close(fd);
		}
	}*/
	//ATTENZIONE ci possono essere dei casi in cui il descrittore è stato tolto dalla maschera
	//perchè se ne sta occupando un thread
	//ma così può succere che quando arriva l'interruzione questo non venga chiuso 
	if(err)
		return -1;
	
	return 0;
}
	

int main(int argc, char *argv[]){
	
	char *socket, *log, *aux, *token;
	char *tmpstr = NULL;
	int n_workers, n;
	int found_s = 0;
	int found_l = 0;
	FILE *fp;
	
	//valori di default [configurazione]
	socket = SOCKNAME;
	log = LOGNAME;
	n_workers = THREAD_NUM;
	n_files_MAX = NBUCKETS;
	st_dim_MAX = ST_DIM;
	
	//parsing file di configurazione 
	
	if((getopt(argc, argv, "k:")) == 'k'){ 
		if((fp = fopen(optarg, "r")) == NULL)
			perror("errore fopen config file, verranno usati valori di default"); 
	
		if((aux = malloc((10 + UNIX_PATH_MAX)*sizeof(char))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
				
        while (fgets(aux, 10 + UNIX_PATH_MAX, fp) != NULL){ 
			token = strtok_r(aux, ":", &tmpstr);
			if(strcmp(token, "NFILES") == 0){
				token = strtok_r(NULL, ":", &tmpstr);
				token[strcspn(token, "\n")] = '\0';
				if(isNumber(token, &n) == 0 && n > 0) //non accetto n_files, n_workers e dim uguali a 0 
					n_files_MAX = n;
				else
					fprintf(stderr, "errore in config file: nfiles_max value errato, verrà usato valore di default\n");
			}
			else if(strcmp(token, "DIM") == 0){
				token = strtok_r(NULL, ":", &tmpstr);
				token[strcspn(token, "\n")] = '\0';
				if(isNumber(token, &n) == 0 && n > 0)
					st_dim_MAX = n;
				else
					fprintf(stderr, "errore in config file: st_dim_max value errato, verrà usato valore di default\n");
			}
			else if (strcmp(token, "NWORKERS") == 0){
				token = strtok_r(NULL, ":", &tmpstr);
				token[strcspn(token, "\n")] = '\0';
				if(isNumber(token, &n) == 0 && n > 0)
					n_workers = n;
				else
					fprintf(stderr, "errore in config file: nworkers value errato, verrà usato valore di default\n");
			}
			else if (strcmp(token, "SOCKET") == 0){
				token = strtok_r(NULL, ":", &tmpstr);
				if(token != NULL && strlen(token) < UNIX_PATH_MAX){
					if((socket = malloc(UNIX_PATH_MAX*sizeof(char))) == NULL){
						perror("malloc");
						int errno_copy = errno;
						fprintf(stderr,"FATAL ERROR: malloc\n");
						exit(errno_copy);
					}
					found_s = 1;
					token[strcspn(token, "\n")] = '\0';
					strncpy(socket, token, UNIX_PATH_MAX); 
				}
				else
					fprintf(stderr, "errore in config file: socket value errato, verrà usato valore di default\n");		
			}
			else if (strcmp(token, "LOG") == 0){
				token = strtok_r(NULL, ":", &tmpstr);
				if(token != NULL && strlen(token) < UNIX_PATH_MAX){
					if((log = malloc(UNIX_PATH_MAX*sizeof(char))) == NULL){
						perror("malloc");
						int errno_copy = errno;
						fprintf(stderr,"FATAL ERROR: malloc\n");
						exit(errno_copy);
					}
					found_l = 1;
					token[strcspn(token, "\n")] = '\0';
					strncpy(log, token, UNIX_PATH_MAX); 
				}
				else{
					fprintf(stderr, "errore in config file: log value errato, verrà usato valore di default\n");
				}
			}
			else
				fprintf(stderr, "formato config file errato, verranno usati valori di default\n");
		}
		
		if(ferror(fp))
			perror("errore fgets config file");
		
		if(fclose(fp) != 0)
			perror("chiusura config file");
		
		free(aux);
		
	}
	
	//inizializzo variabili di storage (inizialmente vuoto)
	st_dim = 0;
	n_files = 0;
	
	//apro log_file 
	if((log_file = fopen(log,"w")) == NULL){ //"w" ok (controllato)
		perror("opening log file");
		return -1; 
	}
	
	fprintf(log_file, "Operazione - File - byte scritti - byte letti - thread\n\n");
    fflush(log_file);
	
	
	//pthread_mutex_init(&mutexCoda, NULL);
	//pthread_cond_init(&condCoda, NULL);
	//stanno in initqueue
	
	//inizializzazione pipe m-ws (comunic. master - workers)
	int pfd[2];
	
	if(pipe(pfd) == -1){ 
		perror("creazione pipe");
		return -1;
	}
	
	//inizializzazione pipe signal (comunic. master - signal handler)
	int sig_pfd[2];
	
	if(pipe(sig_pfd) == -1){
		perror("creazione signal pipe");
		return -1;
	}
	
	//inizializzazione socket (comunic. server-clients)
	struct sockaddr_un sa;
	
	memset(&sa, 0, sizeof(struct sockaddr_un));
	strncpy(sa.sun_path, socket, UNIX_PATH_MAX);

	sa.sun_family = AF_UNIX;
	
	//NB: scrivi meglio una funzione di pulizia per non ripetere tutti questi free (e close) e poi
	//assicurati che log e socket siano stati creati con malloc e non di default 
	//idem anche per chiudere le pipe e i file, controlla 
	
	//inizializzazione hash table 
	if((hash_table = icl_hash_create(n_files_MAX, hash_pjw, string_compare)) == NULL){
		fprintf(stderr, "Errore nella creazione della hash table\n");
		free(socket);
		free(log);
		return -1;
	}
	
	//inizializzazione task_queue, perchè non farla locale e passarla ai thread? (la dovrei passare a init_thread e run_server e submit_task
	Queue_t * task_queue = initQueue();
    if (!task_queue) {
		fprintf(stderr, "initQueue su tsk_q fallita\n");
		free(socket);
		free(log);
		exit(errno); //controlla
    }
	
	//inizializzazione files_queue
	files_queue = initQueue();
    if (!files_queue) {
		fprintf(stderr, "initQueue su file_q fallita\n");
		free(socket);
		free(log);
		exit(errno); //controlla
    }
	
	//Inizializzazione thread pool 
	pthread_t th[n_workers];
	
	th_w_arg *w_args;

	if((w_args = malloc(sizeof(th_w_arg))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		free(socket);
		free(log);
		exit(errno_copy);
	}
	
	w_args->task_queue = task_queue;
	w_args->pipe_w = pfd[1];
	
	
	//creo maschera per thread signal handler
	//la azzero e poi inserisco i segnali che voglio gestire (li maschero)
	
	//così forzo la gestione da parte dello specifico thread sig_handler
	//che inoltre sarà in attesa per quei segnali con una sig_wait,
	//la quale prevede che i segnali da gestire siano stati mascherati 
	
	sigset_t     mask_s;
    sigemptyset(&mask_s); 
    sigaddset(&mask_s, SIGINT);  
    sigaddset(&mask_s, SIGQUIT);
    sigaddset(&mask_s, SIGHUP);    
	
    if (pthread_sigmask(SIG_BLOCK, &mask_s,NULL) != 0) {  //guarda SIG_SETMASK 
		fprintf(stderr, "FATAL ERROR\n"); //sigmask non setta errno 
		free(socket);
		free(log);
		abort();
    }
    
	// ignoro SIGPIPE per evitare di essere terminato da una scrittura su un socket senza lettori 
	// Magari il client fa una richiesta e chiude la comunicazione senza attendere la risposta
	// il server non deve terminare in questo caso (il comportamento di default è quit)
    
	struct sigaction s;
    memset(&s,0,sizeof(s));    
    
	s.sa_handler = SIG_IGN;

    if ((sigaction(SIGPIPE, &s, NULL)) == -1) {   
		perror("sigaction");
		free(socket);
		free(log);
		abort();
    } 
	
	//inizializzo thread gestore dei segnali
	pthread_t sighandler_thread;
	
	//gli devo passare anche il descrittore di lettura della pipe 
	
	//la maschra viene ereditata, ma la devo comunque inviare per poter fare la wait 
    
	th_sig_arg *sig_args;

	if((sig_args = malloc(sizeof(th_sig_arg))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		free(socket);
		free(log);
		exit(errno_copy);
	}
	
	sig_args->mask = &mask_s;
	sig_args->pipe_s = sig_pfd[1];
	
	
    if (pthread_create(&sighandler_thread, NULL, sigHandler, sig_args) != 0) {
		fprintf(stderr, "errore nella creazione del signal handler thread\n");
		free(socket);
		free(log);
		free(sig_args);
		free(w_args);
		abort();
    }
	
	//creazione e avvio threadpool 
	
	for(int i = 0; i < n_workers; i++){ //attenzione i seguenti 2 comandi fatti anche sopra 
	
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
			free(sig_args);
			free(w_args);
			return -1;
		}
		
		if(pthread_create(&th[i], NULL, &init_thread, w_args) != 0){ //creo il thread che inizia ad eseguire init_thread
			perror("Err in creazione thread");
			//pulizia 
			free(socket);
			free(log);
			free(sig_args);
			free(w_args);
			return -1;
		}
		
	}
	//avvio server 
	run_server(&sa, pfd[0], sig_pfd[0], task_queue); 
   
    for (int i = 0; i < n_workers; i++){   
        push(task_queue, EOS);
    }
	
	 // aspetto la terminazione de signal handler thread
    if(pthread_join(sighandler_thread, NULL) != 0){
		perror("Err in join sighandler thread");
		if(found_s)
			free(socket);
		free(log);
		free(sig_args);
		free(w_args);
		return -1;
	}
  
	for(int i = 0; i < n_workers; i++){ 
		if(pthread_join(th[i], NULL) != 0){ //aspetto terminino tutti i thread (la  join mi permette di liberare la memoria occupata dallo stack privato del thread)
			perror("Err in join thread");
			if(found_s)
				free(socket);
			free(log);
			free(sig_args);
			free(w_args);
			return -1;
		}
	}
	
	fclose(log_file);
	close(pfd[0]);
	close(pfd[1]);
	close(sig_pfd[0]);
	close(sig_pfd[1]);
	//socket descriptor chiuso in run_server 
	 
	
	fprintf(stdout, "\n\nLISTA FILE:\n\n");
	fflush(stdout);
	print_deb();
	
	print_summ(log);  //chiudendo log, faccio di default anche fflush
	
	//eliminazione elm thread 
	//pthread_mutex_destroy(&mutexCoda); //non so se vanno bene
	//pthread_cond_destroy(&condCoda);
	//sostituiti con: 
	//eliminazione task_queue  
	
	icl_hash_destroy(hash_table, &free, &free_data_ht);
	
	deleteQueue(task_queue);
	deleteQueue(files_queue);

	//assicurati tutte le connessioni siano chiuse sul socket prima diterminare
	//finora chiudo la connessione quando ricevo il mess "disconnesso" dal client
	if(found_s)
		free(socket);
	if(found_l)
		free(log);
	free(sig_args);
	free(w_args);
	
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
	
	char *cmd, *bW_s, *bR_s, *newline;
	char* tmpstr = NULL;
	
	if ((fd = fopen(log, "r")) == NULL) {
		perror("opening log file");
		return -1;
    }
	if ((buffer = malloc(MAX_SIZE*sizeof(char))) == NULL) {
		perror("malloc buffer"); 
		fclose(fd);
        return -1;		
	}
	//tolgo l'intestazione 
	fgets(buffer, MAX_SIZE, fd);
	fgets(buffer, MAX_SIZE, fd);
	
	while(fgets(buffer, MAX_SIZE, fd) != NULL) {
		// controllo di aver letto tutta una riga
		if ((newline = strchr(buffer, '\n')) == NULL) {
			fprintf(stderr, "buffer di linea troppo piccolo");
			fclose(fd);
			free(buffer);
			return -1;
		}
		*newline = '\0';  // tolgo lo '\n', non strettamente necessario
		
		cmd = strtok_r(buffer, " ", &tmpstr);
		strtok_r(NULL, " ", &tmpstr);  //il file di riferimento non mi serve
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
		
		if (strncmp(cmd, "rimpiazzamento", MAX_SIZE) == 0) 
			cnt_rimp++;
		
		if (strncmp(cmd, "writeF", MAX_SIZE) == 0 || strncmp(cmd, "append", MAX_SIZE) == 0) {
			cnt_dim += bW;
			if(cnt_dim > cnt_dim_MAX)  
				cnt_dim_MAX = cnt_dim;
		}
		if (strncmp(cmd, "remove", MAX_SIZE) == 0 || strncmp(cmd, "rimpiazzamento", MAX_SIZE) == 0) 
			cnt_dim -= bR;
		
		if (strncmp(cmd, "open_c", MAX_SIZE) == 0 || strncmp(cmd, "open_cl", MAX_SIZE) == 0) { 
			cnt_nf++;
			if(cnt_nf > cnt_nf_MAX)
				cnt_nf_MAX = cnt_nf;
		}
		if (strncmp(cmd, "remove", MAX_SIZE) == 0 || strncmp(cmd, "rimpiazzamento", MAX_SIZE) == 0) 
			cnt_nf--;
	}
    fprintf(stderr, "\nIl numero massimo di file memorizzati nel server è: %d\n", cnt_nf_MAX);
	
	fprintf(stderr, "La dimensione massima in MBytes raggiunta dal file storage è: %.3f\n", ((float) cnt_dim_MAX)/1000);
	
	fprintf(stderr, "L'algoritmo di rimpiazzamento è stato eseguito %d volte\n\n", cnt_rimp);

	fclose(fd);
	free(buffer);
	
	return 0;
}

file_info *init_file(int lock_owner, int fd, int lst_op, char *cnt, int sz){
	
	file_info *tmp;
	open_node *client;
	  
	if((tmp = malloc(sizeof(file_info))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		exit(errno_copy);
	}
	
	//inizializzo la lista degli open_owners  
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
	
	if(sz == -1) //CONTROLLA 
		sz = 0;
	tmp->cnt_sz = sz;
	if(sz > 0)
		memcpy(tmp->cnt, cnt, sz); //controlla 
	
	if (pthread_cond_init(&tmp->cond, NULL) != 0) {
		perror("mutex cond");
		return NULL;
	}

	return tmp; //viene liberato quando faccio la delete_ht o destroy_ht 
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
		
		for (k = 0; k < hash_table->nbuckets; k++)  {
			for (entry = hash_table->buckets[k]; entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL); entry = entry->next){
				fprintf(stderr, "File: %s", key);
				#ifdef DEBUG
				fprintf(stderr, "\nlock_owner: %d, last_op: %d\n", ((file_info *)value)->lock_owner, ((file_info *)value)->lst_op);
				print_list(((file_info *)value)->open_owners);
				if(((file_info *)value)->cnt != NULL){
					fprintf(stderr, "cnt_sz: %d, cnt:\n", ((file_info *)value)->cnt_sz);						
					fwrite(((file_info *)value)->cnt, ((file_info *)value)->cnt_sz, 1, stderr);
					fprintf(stderr, "\n");
				}
				else
					fprintf(stderr, "contenuto: NULL\n");
				#endif
				fprintf(stderr, "\n");
			}
		}
		fprintf(stderr, "\n");
	}
	else
		fprintf(stderr, "hash table vuota\n");
	Pthread_mutex_unlock(&ht_mtx);
	
	return;
}

//inizio in modo semplice, mettendo grandi sezioni critiche 


//tra find e insert non posso eliminare la lock, o un thread potrebbe inserirlo
//idem tra rimpiazzamento e delate (lì cerco i file da eliminare e poi li elimino, non posso rischiare qualcuno li modifichi
int openFC(char *path, int fd){ //free(data) viene fatto da delete_ht/destroy_ht
	file_info *data;
	int n;
	char *mess;
	
	char *path_storage;
	char *path_queue;
	
	Pthread_mutex_lock(&ht_mtx);
	
	if(icl_hash_find(hash_table, path) != NULL){ 
		fprintf(stderr, "Impossibile ricreare file già esistente\n");
		
		mess = "Err:fileEsistente";
		n = strlen(mess) + 1;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, mess, n) == -1)
			perror("write server openFC");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
//vedi se lo puoi fare fuori dalla sez critica
		if((path_storage = malloc((strlen(path)+1)*sizeof(char))) == NULL){ 
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		strncpy(path_storage, path, strlen(path)+1);
		
		data = init_file(-1, fd, 0, NULL, -1);  
		if(data == NULL){
			mess = "Err:creazioneFile";
			n = strlen(mess) + 1;
			if(writen(fd, &n, sizeof(int)) == -1){
				perror("Errore nella write");
				free_data_ht(data);
				free(path_storage);
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}
			if(writen(fd, mess, n) == -1)
				perror("write server openFC");
			free_data_ht(data);
			free(path_storage);
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
///////////////////////////////////////////////////
		file_repl *aux = NULL;
		file_repl *prec = NULL;
		int res;
		
		//!!!rimpiazzamento 
		Pthread_mutex_lock(&server_info_mtx);
		res = st_repl(0, fd, path, &aux, 1);
		
		if(res == -1) //per ora lo faccio tutto lockato 
			mess = "Err:rimpiazzamento";
		if(res == -2)
			mess = "Err:fileTroppoGrande";
		if(res == -1 || res == -2){
			n = strlen(mess) + 1;
			if(writen(fd, &n, sizeof(int)) == -1){
				perror("Errore nella write");
				Pthread_mutex_unlock(&server_info_mtx);
				free_data_ht(data);
				free(path_storage);
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}
			if(writen(fd, mess, n) == -1)
				perror("write server openFC");
			Pthread_mutex_unlock(&server_info_mtx);
			free_data_ht(data);
			free(path_storage);
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
						
		if(aux != NULL){		
			while(aux != NULL){
				prec = aux;
				//file_sender(aux, fd);
				
				Pthread_mutex_lock(&log_mtx);
				fprintf(log_file,"%s %s -1 %d %lu\n", "rimpiazzamento",  aux->path, aux->pnt->cnt_sz, pthread_self());
				fflush(log_file);
				Pthread_mutex_unlock(&log_mtx);
				
				int sz_cpy = aux->pnt->cnt_sz;
				pthread_cond_signal(&aux->pnt->cond);
				#ifdef DEBUG2
					Pthread_mutex_lock(&log_mtx);
					fprintf(log_file,"%s %s -1 -1 %lu\n", "SIGNAL",  aux->path, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
				#endif
				if(icl_hash_delete(hash_table, aux->path, &free, &free_data_ht) == -1){ 
					fprintf(stderr, "Errore nella rimozione del file dallo storage, inrepl\n");
					Pthread_mutex_unlock(&server_info_mtx);
					pthread_cond_broadcast(&ht_cond);
					Pthread_mutex_unlock(&ht_mtx);
					free_data_ht(data);
					free(path_storage);
					return -1;
				}	
				st_dim -= sz_cpy;
				n_files--;
				aux = aux->next;
				fprintf(stderr, "ECCO: %s\n", prec->path);
				free(prec->path);
				free(prec);
			}
		}

		if(icl_hash_insert(hash_table, path_storage, (void *) data) == NULL){
			fprintf(stderr, "Errore nell'inseriemento del file nello storage\n");
			mess = "Err:inserimento";
			n = strlen(mess) + 1;
			if(writen(fd, &n, sizeof(int)) == -1){
				perror("Errore nella write");
				Pthread_mutex_unlock(&server_info_mtx);
				Pthread_mutex_unlock(&ht_mtx);
				free_data_ht(data);
				return -1;
			}
			if(writen(fd, mess, n) == -1)
				perror("write server openFC");
			Pthread_mutex_unlock(&server_info_mtx);
			Pthread_mutex_unlock(&ht_mtx);
			free_data_ht(data);
			free(path_storage);
			return -1;
		}
		
		n_files++; 
		Pthread_mutex_unlock(&server_info_mtx);
		
		
		Pthread_mutex_unlock(&ht_mtx);
		
		if((path_queue = malloc((strlen(path)+1)*sizeof(char))) == NULL){ 
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		strncpy(path_queue, path, strlen(path)+1);
		
		if (push(files_queue, path_queue) == -1) {
			fprintf(stderr, "Errore: push\n");
			pthread_exit(NULL); //controlla se va bene 
		}

		mess = "Ok";
		n = strlen(mess) + 1;
		if(writen(fd, &n, sizeof(int)) == -1){//le write dovranno essere riviste (atomicità)
			perror("Errore nella write");
			return -1;
		}
		if(writen(fd, "Ok", n) == -1){
			perror("write server openFC");
			return -1;
		}
		
		#ifdef DEBUG5
			fprintf(stderr,"---OPEN CREATE FILE\n");
			print_deb();
		#endif 

	}
	return 0;
}

int openFL(char *path, int fd){
	file_info *tmp;
	int n;
	char *mess;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		//fprintf(stderr, "Impossibile fare lock su file non esistente\n");
		mess = "Err:fileNonEsistente";
		n = strlen(mess) + 1;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write di OpenFL");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(writen(fd, mess, n) == -1){
			perror("write server openFL");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		Pthread_mutex_unlock(&ht_mtx);
		return -2;
	} 
	else{ //se sta cercando di aprire un file che ha già aperto, lo ignoro (per ora)
		while(tmp->lock_owner != -1 && tmp->lock_owner != fd){
			#ifdef DEBUG2
				Pthread_mutex_lock(&log_mtx);
				fprintf(log_file,"%s %s -1 -1 %lu\n", "WAIT",  path, pthread_self());
				fflush(log_file);
				Pthread_mutex_unlock(&log_mtx);
			#endif
			pthread_cond_wait(&tmp->cond,&ht_mtx);
			
			if((tmp = icl_hash_find(hash_table, path)) == NULL){ //il file potrebbe essere stato rimosso dal detentore della lock 
			   //fprintf(stderr, "Impossibile fare lock su file non esistente\n"); 
				mess = "Err:fileNonEsistente";
	        	n = strlen(mess) + 1;
				
				if(writen(fd, &n, sizeof(int)) == -1){
					perror("Errore nella write");
					Pthread_mutex_unlock(&ht_mtx);
					return -1;
				}
				if(writen(fd, mess, n) == -1){
					perror("write server openFL");
					Pthread_mutex_unlock(&ht_mtx);
					return -1;
			    }
				Pthread_mutex_unlock(&ht_mtx);
				return -2;
			} 
		}
		
		tmp->lock_owner = fd;
		insert_head(&(tmp->open_owners), fd); 
		tmp->lst_op = 0;
		
		Pthread_mutex_unlock(&ht_mtx);
		mess = "Ok";
		n = strlen(mess) + 1;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			return -1;
		}
		
		if(writen(fd, mess, n) == -1){
			perror("write server openFL");
			return -1;
		}	
	}
	
	#ifdef DEBUG5
		fprintf(stderr,"---OPEN LOCKED FILE\n");
		print_deb();
	#endif
	return 0;
}

int openFO(char *path, int fd){
	file_info *tmp;
	int n;
	char *mess;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		fprintf(stderr, "Impossibile aprire un file non esistente\n");
		mess = "Err:fileNonEsistente";
		n = strlen(mess) + 1;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		
		if(writen(fd, mess, n) == -1)
			perror("write server openFO");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	} 
	else{ //se sta cercando di aprire un file che ha già aperto, lo ignoro (per ora)
		insert_head(&(tmp->open_owners), fd);
		tmp->lst_op = 0;
		
		Pthread_mutex_unlock(&ht_mtx);
		mess = "Ok";
		n = strlen(mess) + 1;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella write");
			return -1;
		}
		if(writen(fd, mess, n) == -1){
			perror("write server openF0");
			return -1;
		}
	}
	#ifdef DEBUG5
		fprintf(stderr,"---OPEN FILE \n");
		print_deb();
	#endif
	
	return 0;
	
}

int openFCL(char *path, int fd){
		file_info *data;
		int n;
		char *path_storage;
	    char *path_queue;
		char * mess;
		
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
			if((path_storage = malloc((strlen(path)+1)*sizeof(char))) == NULL){ 
				perror("malloc");
				int errno_copy = errno;
				fprintf(stderr,"FATAL ERROR: malloc\n");
				exit(errno_copy);
			}
			strncpy(path_storage, path, strlen(path)+1);
			
			data = init_file(fd, fd, 1, NULL, -1);  
			if(data == NULL){
				mess = "Err:creazioneFile";
				n = strlen(mess) + 1;
				if(writen(fd, &n, sizeof(int)) == -1){
					perror("Errore nella write");
					free_data_ht(data);
					free(path_storage);
					Pthread_mutex_unlock(&ht_mtx);
					return -1;
				}
				if(writen(fd, mess, n) == -1)
					perror("write server openFC");
				free_data_ht(data);
				free(path_storage);
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}

			file_repl *aux = NULL;
			file_repl *prec = NULL;
		
			//!!!rimpiazzamento 
			Pthread_mutex_lock(&server_info_mtx);

			int res = st_repl(0, fd, path, &aux, 1);

			if(res == -1) //per ora lo faccio tutto lockato 
				mess = "Err:rimpiazzamento";
			if(res == -2)
				mess = "Err:fileTroppoGrande";
			if(res == -1 || res == -2){
				n = strlen(mess) + 1;
				if(writen(fd, &n, sizeof(int)) == -1){
					perror("Errore nella write");
					Pthread_mutex_unlock(&server_info_mtx);
					free_data_ht(data);
					free(path_storage);
					Pthread_mutex_unlock(&ht_mtx);
					return -1;
				}
				if(writen(fd, mess, n) == -1)
					perror("write server openFC");
				Pthread_mutex_unlock(&server_info_mtx);
				free_data_ht(data);
				free(path_storage);
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}
			
			
			
			if(aux != NULL){
				while(aux != NULL){
					prec = aux;
					//file_sender(aux, fd);
					
					Pthread_mutex_lock(&log_mtx);
					fprintf(log_file,"%s %s -1 %d %lu\n", "rimpiazzamento",  aux->path, aux->pnt->cnt_sz, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
					
					int sz_cpy = aux->pnt->cnt_sz;
					pthread_cond_signal(&aux->pnt->cond);
				#ifdef DEBUG2
					Pthread_mutex_lock(&log_mtx);
					fprintf(log_file,"%s %s -1 -1 %lu\n", "SIGNAL",  aux->path, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
				#endif					
					if(icl_hash_delete(hash_table, aux->path, &free, &free_data_ht) == -1){ 
						fprintf(stderr, "Errore nella rimozione del file dallo storage, inrepl\n");
						Pthread_mutex_unlock(&server_info_mtx);
						Pthread_mutex_unlock(&ht_mtx);
						free_data_ht(data);
						free(path_storage);
						return -1;
					}	
					st_dim -= sz_cpy;
					n_files--;
					aux = aux->next;
					free(prec->path);
					free(prec);
				
				}
			}
	
			if(icl_hash_insert(hash_table, path_storage, data) == NULL){
				fprintf(stderr, "Errore nell'inseriemento del file nello storage\n");
				n = 16;
				if(writen(fd, &n, sizeof(int)) == -1){
					perror("Errore nella write");
					Pthread_mutex_unlock(&server_info_mtx);
					Pthread_mutex_unlock(&ht_mtx);
					free_data_ht(data);
					return -1;
				}
				
				if(write(fd, "Err:inserimento", n) == -1)
					perror("write server openFCL");
				Pthread_mutex_unlock(&server_info_mtx);
				Pthread_mutex_unlock(&ht_mtx);
				free_data_ht(data);
				return -1;
			}
			
			n_files++; 
			Pthread_mutex_unlock(&server_info_mtx);
			Pthread_mutex_unlock(&ht_mtx);			
			
			
			if((path_queue = malloc((strlen(path)+1)*sizeof(char))) == NULL){ 
				perror("malloc");
				int errno_copy = errno;
				fprintf(stderr,"FATAL ERROR: malloc\n");
				exit(errno_copy);
			}
			strncpy(path_queue, path, strlen(path)+1);
			
			if (push(files_queue, path_queue) == -1) {
				fprintf(stderr, "Errore: push\n");
				pthread_exit(NULL); //controlla se va bene 
		    }
			
			n = 3;
			if(writen(fd, &n, sizeof(int)) == -1){
				perror("Errore nella write");
				return -1;
			}
			if(writen(fd, "Ok", n) == -1){
				perror("write server openFCL");
				return -1;
			}	
			
		}
		
		#ifdef DEBUG5
			fprintf(stderr,"---OPEN CREATE LOCKED FILE\n");
			print_deb();
		#endif
		
		return 0;
	}
	
int closeF(char *path, int fd){ //quando chiudo un file rilascio anche la lock se la possiedo
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
						
		if(tmp->lock_owner == fd){
			tmp->lock_owner = -1;
			pthread_cond_signal(&tmp->cond); 
							#ifdef DEBUG2
					Pthread_mutex_lock(&log_mtx);
					fprintf(log_file,"%s %s -1 -1 %lu\n", "SIGNAL",  path, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
				#endif
		}
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
	#ifdef DEBUG5
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
	fflush(log_file);
	Pthread_mutex_unlock(&log_mtx);
			
	#ifdef DEBUG5
		fprintf(stderr,"---READ FILE \n");
		print_deb();
	#endif
	
	return 0;
			
}

int writeF(char *path, char *cnt, int sz, int fd){
	file_info *tmp;	
	int n;
	char *mess;
	
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
		fprintf(stderr, "L'operazione precedente a writeFile deve essere openFile(O_CREATE|O_LOCK)\n");
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
			file_repl *aux = NULL;
			file_repl *prec = NULL;
			Pthread_mutex_lock(&server_info_mtx);
			
			int res = st_repl(sz, fd, path, &aux, 0);
			
			if(res == -1) //per ora lo faccio tutto lockato 
				mess = "Err:rimpiazzamento";
			if(res == -2)
				mess = "Err:fileTroppoGrande";
			if(res == -1 || res == -2){
				n = strlen(mess) + 1;
				pthread_cond_signal(&tmp->cond); 
								#ifdef DEBUG2
					Pthread_mutex_lock(&log_mtx);
					fprintf(log_file,"%s %s -1 -1 %lu\n", "SIGNAL", path, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
				#endif
				if(icl_hash_delete(hash_table, path, &free, &free_data_ht) == -1){ 
					fprintf(stderr, "Errore nella rimozione del file dallo storage, inrepl\n");
					Pthread_mutex_unlock(&server_info_mtx);
					Pthread_mutex_unlock(&ht_mtx);
					return -1;
				}
				if(writen(fd, &n, sizeof(int)) == -1){
					perror("Errore nella write");
					Pthread_mutex_unlock(&server_info_mtx);
					Pthread_mutex_unlock(&ht_mtx);
					return -1;
				}
				if(writen(fd, mess, n) == -1)
					perror("write server openFC");
				Pthread_mutex_unlock(&server_info_mtx);
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}
			
			#ifdef DEBUG2
				file_repl *aux2 = aux;
				fprintf(stderr, "LISTA ESPULSI: ");
				while(aux2 != NULL){
					fprintf(stderr, "%s -> ", aux2->path);
					aux2 = aux2->next;
				}
				fprintf(stderr, "//\n");
			#endif

			if(aux != NULL){	
				while(aux != NULL){
					prec = aux;
					
					file_sender(aux, fd);
					
					Pthread_mutex_lock(&log_mtx);
					fprintf(log_file,"%s %s -1 %d %lu\n", "rimpiazzamento",  aux->path, aux->pnt->cnt_sz, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
					
					int sz_cpy = aux->pnt->cnt_sz;
					pthread_cond_signal(&aux->pnt->cond); 
									#ifdef DEBUG2
					Pthread_mutex_lock(&log_mtx);
					fprintf(log_file,"%s %s -1 -1 %lu\n", "SIGNAL",  aux->path, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
				#endif
					if(icl_hash_delete(hash_table, aux->path, &free, &free_data_ht) == -1){ 
						fprintf(stderr, "Errore nella rimozione del file dallo storage, inrepl\n");
						Pthread_mutex_unlock(&server_info_mtx); 
						Pthread_mutex_unlock(&ht_mtx);
						return -1;
					}	
					if(sz_cpy > 0)
						st_dim -= sz_cpy;
					n_files--;
					aux = aux->next;
					free(prec->path);
					free(prec);
				}
			}
			tmp->cnt_sz = sz;
			if(sz > 0){
				memcpy(tmp->cnt, cnt, sz);
				st_dim += sz;  
			}
			Pthread_mutex_unlock(&server_info_mtx);
			
		}
		else{
			#ifdef DEBUG3
				fprintf(stderr, "richiesta scrittura di file vuoto\n");
			#endif
			sz = 0; //me ne assicuro 
		}
		Pthread_mutex_unlock(&ht_mtx);
		
		Pthread_mutex_lock(&log_mtx); 
		fprintf(log_file,"%s %s %d -1 %lu\n", "writeF", path, sz, pthread_self());
		fflush(log_file);
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
	
	#ifdef DEBUG5
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

		while(tmp->lock_owner != -1 && tmp->lock_owner != fd){
			#ifdef DEBUG2
				Pthread_mutex_lock(&log_mtx);
				fprintf(log_file,"%s %s -1 -1 %lu\n", "WAIT",  path, pthread_self());
				fflush(log_file);
				Pthread_mutex_unlock(&log_mtx);
			#endif
			pthread_cond_wait(&tmp->cond,&ht_mtx);
			if((tmp = icl_hash_find(hash_table, path)) == NULL){ //il file potrebbe essere stato rimosso dal detentore della lock 
				fprintf(stderr, "Impossibile fare lock su file non esistente\n");
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}
		}
		tmp->lock_owner = fd;
		tmp->lst_op = 0; 
	}
	Pthread_mutex_unlock(&ht_mtx);
	
	#ifdef DEBUG5
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
		
        pthread_cond_signal(&tmp->cond);
						#ifdef DEBUG2
					Pthread_mutex_lock(&log_mtx);
					fprintf(log_file,"%s %s -1 -1 %lu\n", "SIGNAL",  path, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
				#endif
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
	#ifdef DEBUG5
		fprintf(stderr,"---UNLOCK FILE \n");
		print_deb();
	#endif
		
	
	return 0;
}

int appendToF(char *path, char *cnt, int sz, int fd){
	file_info *tmp;	
	int res, n;
	char *mess;
	
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
			file_repl *aux = NULL;
			file_repl *prec = NULL;
			Pthread_mutex_lock(&server_info_mtx);
			res = st_repl(sz, fd, path, &aux, 0);
			
			if(res == -1)
				mess = "Err:rimpiazzamento";
			if(res == -2)
				mess = "Err:fileTroppoGrande";							
			
			if(res == -1 || res == -2){
				n = strlen(mess) + 1;	
				if(writen(fd, &n, sizeof(int)) == -1){
					perror("Errore nella write");
					Pthread_mutex_unlock(&server_info_mtx);
					Pthread_mutex_unlock(&ht_mtx);
					return -1;
				}
				if(writen(fd, mess, n) == -1)
					perror("write server openFC");
				Pthread_mutex_unlock(&server_info_mtx);
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}

			#ifdef DEBUG
				file_repl *aux2 = aux;
				fprintf(stderr, "LISTA ESPULSI: ");
				while(aux2 != NULL){
					fprintf(stderr, "%s -> ", aux2->path);
					aux2 = aux2->next;
				}
				fprintf(stderr, "NULL\n");
			#endif

			if(aux != NULL){
				while(aux != NULL){
					file_sender(aux, fd);
					prec = aux;
					
					Pthread_mutex_lock(&log_mtx);
					fprintf(log_file,"%s %s -1 %d %lu\n", "rimpiazzamento",  aux->path, aux->pnt->cnt_sz, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
					
					int sz_cpy = aux->pnt->cnt_sz;
					pthread_cond_signal(&aux->pnt->cond); 
									#ifdef DEBUG2
					Pthread_mutex_lock(&log_mtx);
					fprintf(log_file,"%s %s -1 -1 %lu\n", "SIGNAL",  aux->path, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
				#endif
					if(icl_hash_delete(hash_table, aux->path, &free, &free_data_ht) == -1){ 
						fprintf(stderr, "Errore nella rimozione del file dallo storage, inrepl\n");
						Pthread_mutex_unlock(&server_info_mtx); 
						Pthread_mutex_unlock(&ht_mtx);
						return -1;
					}			
					if(sz_cpy > 0)
						st_dim -= sz_cpy;
					n_files--;	
					fprintf(stderr, "!!new_st_dim : %d, dim_new: %d, dim_exp: %d, exp: %s\n", st_dim, sz, sz_cpy, aux->path);
						
					aux = aux->next;
					free(prec->path);					
					free(prec);
				}
			}
			
			if(sz > 0){ //prima era >= 
				memcpy(tmp->cnt + tmp->cnt_sz, cnt, sz);
				tmp->cnt_sz += sz;
				st_dim +=sz; 
			}
			Pthread_mutex_unlock(&server_info_mtx); 
	    }
		else{
			#ifdef DEBUG3
				fprintf(stderr, "richiesta scrittura di file vuoto\n");
			#endif
		}

		Pthread_mutex_unlock(&ht_mtx);
		
		Pthread_mutex_lock(&log_mtx); 
		fprintf(log_file,"%s %s %d -1 %lu\n", "append", path, sz, pthread_self());
		fflush(log_file);
		Pthread_mutex_unlock(&log_mtx);
		
		int n = 2;
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("write server appendToF");
			return -1;
		}
		if(writen(fd, "$", n) == -1){
			perror("write server appendToF");
			return -1;
		}
		n = 3;
		
		if(writen(fd, &n, sizeof(int)) == -1){
			perror("Errore nella appendToF");
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
		old_sz = tmp->cnt_sz; //per inserire i byte rimossi nel file di log
		
        //non aggiorno la repl_queue perchè semplicemente quando farà il pop controllerà se il file esiste ancora o meno
		pthread_cond_signal(&tmp->cond); //sveglio chi era in attesa della lock, questo dovrà SEMPRE ricontrollare che il file esista ancora 
				#ifdef DEBUG2
					Pthread_mutex_lock(&log_mtx);
					fprintf(log_file,"%s %s -1 -1 %lu\n", "SIGNAL",  path, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
				#endif       
	   //non so se va bene questa cosa di non fare immediatamente un'unlock dopo la signal 
		if(icl_hash_delete(hash_table, path, &free, &free_data_ht) == -1){ 
			fprintf(stderr, "Errore nella rimozione del file dallo storage\n");
			if(writen(fd, "Err:rimozione", 14) == -1)
				perror("write server removeF");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		
		Pthread_mutex_unlock(&ht_mtx);

		Pthread_mutex_lock(&server_info_mtx);
		n_files--; 
		st_dim -= old_sz; 
		Pthread_mutex_unlock(&server_info_mtx);
		
		Pthread_mutex_lock(&log_mtx); 
		fprintf(log_file,"%s %s -1 %d %lu\n", "remove", path, old_sz, pthread_self());
		fflush(log_file);
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
		
		#ifdef DEBUG5
			fprintf(stderr,"---REMOVE FILE\n");
			print_deb();
		#endif 
	}
	return 0;
}

//queste chiamate non devono aprire e chiudere il file (azione che viene compiuta all'interno del server, solo il client segue quel protocollo)
int readNF(int fd, int n){
	    
	icl_entry_t *entry;
	char *key, *value;
	int k;
	int all = 0;
	file_repl *file;
	
	
	if(n <= 0){
		all = 1;
		n = 1;
	}
	Pthread_mutex_lock(&ht_mtx);
	if (hash_table) {
		for (k = 0; k < hash_table->nbuckets && n > 0; k++)  {
			for (entry = hash_table->buckets[k]; entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL) && n > 0; entry = entry->next){
					
				if((file = malloc(sizeof(file_repl))) == NULL){
					perror("malloc");
					int errno_copy = errno;
					fprintf(stderr,"FATAL ERROR: malloc\n");
					Pthread_mutex_unlock(&ht_mtx);
					exit(errno_copy);
				}
				
				file->path = key;
				file->pnt = ((file_info *)value);
				
				if(file_sender(file, fd) != -1){
					Pthread_mutex_lock(&log_mtx); 
					fprintf(log_file,"%s %s -1 %d %lu\n", "readNF", key, file->pnt->cnt_sz, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
				}
				
				if(!all)
					n--;
				
				free(file);
				
			}
		}
	}
	else
		fprintf(stderr, "hash table vuota\n");

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
   
	#ifdef DEBUG5
		fprintf(stderr,"---SEND N FILES\n");
		print_deb();
	#endif
	
	
	return 0;			
}

int clean_storage(int fd){
	icl_entry_t *entry;
	char *key, *value;
	int k;
	

	Pthread_mutex_lock(&ht_mtx);
	if (hash_table) {
		for (k = 0; k < hash_table->nbuckets; k++)  {
			for (entry = hash_table->buckets[k]; entry != NULL && ((key = entry->key) != NULL) && ((value = entry->data) != NULL); entry = entry->next){
				remove_elm(&(((file_info *)value)->open_owners), fd);
				if(((file_info *)value)->lock_owner == fd){
					((file_info *)value)->lock_owner = -1; //guarda se devi fare qualche unlock particolare
					pthread_cond_signal(&((file_info *)value)->cond);
									#ifdef DEBUG2
					Pthread_mutex_lock(&log_mtx);
					fprintf(log_file,"%s %s -1 -1 %lu\n", "SIGNAL",  (char *) key, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
				#endif
				}
			}
		}
	}
	Pthread_mutex_unlock(&ht_mtx); //guarda se va bene fare l'unlock solo ora 
	
	return 0;
	
}
	
//terrò la lock dei file da eliminare fino all'uscita, e poi la rilascerò dopo averli eliminati
//accedo a questa funzione con le dimensioni lockkate 
//questo mi permette di agire concorrentemente su file (lock, read, unlock, open)
//ma non di agire sulle dimensioni dello storage (write, create, remove)
//[attenzione deadlock, che succede se un file prende la lock
//prova a scrivere (non può perchè dim lock) e si blocca
//poi io cerco di rimuovere quel file e mi blocco perchè ho la lock!!]
//altrimenti c'è il rischio che si pensi di aver liberato sufficiente spazio
//ma vengono messi dentro altri files nel mentre 
//però questo implica che PRIMA di inserire/aggiornare file
//io abbia locckato le dimensioni dello storage
//perchè per aggiungere/modificare un file voglio che non solo il file sia libero
//ma anche che non sia in atto in quel momento un rimpiazzamento 




//NB: per come sono le api, non è possibile salvare i file espulsi a causa di
//superamento numero massimo file
//questo perchè i file vengono inseriti tramite le openc/opencl 
//che a loro interno chiameranno l'algo di rimpiazzamento
//queste però non hanno fra i parametri una directory dove salvare i file espulsi

//SCRIVILO NELLA RELAZIONE: 
//"non è stato possibile salvare i file espulsi a seguito di writeFile perchè n_files >= n_files_MAX
//"writeFile infatti scrive file solo se non presenti nello storage, ergo li crea ogni volta
//"tuttavia la creazione di file avviene esclusivamente attraverso openfc,openfcl
//"le quali non possiedono un parametro dirname dove poter salvare i file espulsi

//(una possibilità è creare una directory temporanea con un nome specifico e 
// salvarli lì, poi la writeF li prenderà da lì e salverà in dirname
// NO, NON FUNZIONA NEL CASO MULTICLIENT 
// client diversi inseriscono e pescano dalla stessa cartella)

//potrei chiamare openfcl direttamente in writeFile invece che nel client,
//ma questo renderebbe poco sensata la specifica che chiede di mandare a buonfine writeFile
//solo se la precedente op su quel file openfcl 

int st_repl(int dim, int fd, char *path, file_repl **list, int isCreate){
	char *exp;
	int found = 0;
	int found_self = 0;
	int tmp_st_dim = st_dim;
	file_info *tmp;
	file_repl *new;

	if(isCreate){
		if(n_files >=  n_files_MAX){ //c'è almeno un posto?
			while(!found){
				exp = pop(files_queue); //dovrei fare un controllo per quando finisco la coda forse
				if((tmp = icl_hash_find(hash_table, exp)) == NULL) //è stato eliminato dallo storage 
					free(exp);
				else{
					found = 1;
					//free(exp);
					if(*list == NULL){
						*list = malloc(sizeof(file_repl));
						(*list)->path = exp;
						(*list)->pnt =  tmp;
						(*list)->next = NULL;
					}
					else{
						fprintf(stderr, "Errore rimpiazzamneto");
						return -1;
					}
				}
			}
		}
	}
	else{
		if(dim > st_dim_MAX){
			fprintf(stderr, "Impossibile rimpiazzare file: file troppo grande");
			return -2;
		}
		else{
			while(dim + tmp_st_dim > st_dim_MAX){
				found = 0;
				while(!found){
					exp = pop(files_queue); //dovrei fare un controllo per quando finisco la coda forse
					if((tmp = icl_hash_find(hash_table, exp)) == NULL){ //è stato eliminato dallo storage 
						free(exp);
						continue;
					}
					if(strncmp(exp, path, UNIX_PATH_MAX) == 0){
						if(!found_self){
							push(files_queue, exp);
							found_self = 1;
							continue;
						}
						else{
							fprintf(stderr, "impossibile rimpiazzare file\n");
							push(files_queue, exp);
							return -2;
						}
					}
					else{
						found = 1;
						//free(exp);
						//fprintf(stderr, "ESPULSO: %s\n", exp);
						if(*list == NULL){
							*list = malloc(sizeof(file_repl));
							(*list)->path = exp;
							(*list)->pnt =  tmp;
							(*list)->next = NULL;
						}
						else{
							new = malloc(sizeof(file_repl));
							new->path = exp;
							new->pnt = tmp;
							new->next =  *list;
							*list = new;
						}
						tmp_st_dim -= tmp->cnt_sz;
					}
				}
			}
		}
	}
	
	return 0;	
}

int file_sender(file_repl *file, int fd){
		
	int msg_sz;
	//fprintf(stderr, "path: %s\n", file->path);
	msg_sz = strlen(file->path) +1;
	
	//fprintf(stderr, "size_path: %d\n", msg_sz);
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
	if(writen(fd, &(file->pnt->cnt_sz), sizeof(int)) == -1){
		perror("Errore nella write3");
		return -1;
	}
	//fprintf(stderr, "Tutto bene: file: %s,  %d\n", file->path, file->cnt_sz);
	if(file->pnt->cnt_sz > 0){
		//fprintf(stderr, "path: %s, cnt_sz: %d, cnt: %s\n", file->path, msg_sz, file->cnt);
	
		if(writen(fd, file->pnt->cnt , file->pnt->cnt_sz) == -1){
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