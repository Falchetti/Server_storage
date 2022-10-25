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
#include <stdarg.h>

#include <icl_hash.h>
#include <queue.h>
#include <conn.h>
#include <util.h>
#include <defines.h> 
#include <list.h> 

#define DEBUG
#undef DEBUG
#define DEBUG2
#undef DEBUG2

#define SOCKNAME "./mysock.sk" 
#define NBUCKETS  10 // n. di file max nello storage 
#define ST_DIM 256 //capacità storage  
#define THREAD_NUM 4
#define LOGNAME "./log.txt" 

#define MAX_REQ   20 //n. max di richieste in coda 
#define EOS (void*)0x1 //valore speciale usato per indicare End-Of-Stream (EOS)

//vedi se usare exit o pthread exit, nel caso di malloc uccido tutto il programma o solo il thread?
//guarda valori di ritorno queue 

//NB: quando client si disconnette non posso fare a meno di fare un controllo in tutto lo storage 
//    per ripulirlo dalle lock/open ownership di quel client 
//    non sono riuscita a trovare un modo più efficiente
//    potrei fare che ogni client ha una struttura dati dove al suo descr
//    è associata lista di file aperti/lockati e alla sua disconnessione scorro
//    quella lista.. tuttavia devo gestire questa struttura 

//strutture utilizzate 

typedef enum type { 
    T_PNT,
    T_INT,
	T_QUEUE
} type;

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

//funzioni di supporto al server e di gestione dello storage 

int file_sender(file_repl *list, int fd);
int reply_sender(char *msg, int fd);

int print_summ(char *log);
void print_files();

file_info *init_file(int lock_owner, int fd, int lst_op, char *cnt, int sz);
int st_repl(int dim, int fd, char *path, file_repl **list, int isCreate);

void free_data_ht(void *data);
int clean_storage (int fd);
int cleanup_server(void (func)(void *), type t, void *args, ...);

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

//!
static void *sigHandler(void *arg) { 
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
				
				return NULL;  //!va bene? O devo rimanere in attesa di un possibile sigint?
			default:  ; 
		}
    }
    return NULL;	   
}

//!
int executeTask(int fd_c){ //qui faccio la read, capisco cosa devo fare, lo faccio chiamando la giusta funzione, rispondo al client, scrivo fd sulla pipe per il server  
		
	char *msg; 
	char *tmpstr = NULL;
    char *token, *token2;
	int n, sz_msg;
	
	//lettura richiesta client [cmd;path;sz_cnt]
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
		fprintf(stderr, "\n********Contenuto canale di comunicazione: %s********\n\n", msg);
	#endif
	
	token = strtok_r(msg, ";", &tmpstr);
		
	if(token != NULL){
		
		if(strcmp(token, "openfc") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			
			if(openFC(token, fd_c) == -1){
				free(msg);
				return -1;
			} 
			Pthread_mutex_lock(&log_mtx); //inserisco nel file di log solo le operazioni andate a buon fine 
			fprintf(log_file,"%s %s -1 -1 %lu\n", "open_c", token, pthread_self());
			fflush(log_file);
			Pthread_mutex_unlock(&log_mtx);				   	
		}
		else if(strcmp(token, "openfl") == 0){
			token = strtok_r(NULL, ";", &tmpstr);

			if(openFL(token, fd_c) == -1){
				free(msg);
				return -1;
			}
			Pthread_mutex_lock(&log_mtx); 
			fprintf(log_file,"%s %s -1 -1 %lu\n", "open_l", token, pthread_self());
			fflush(log_file);
			Pthread_mutex_unlock(&log_mtx);			
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
						
			clean_storage(fd_c); //elimina lock ownership e open ownership che il client fd_c aveva sui file 
			
			Pthread_mutex_lock(&log_mtx);
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
			//l'aggiornamento del file di log avviene all'interno di readF
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
			//l'aggiornamento del file di log avviene all'interno di writeF
		}
		else if(strcmp(token, "lockf") == 0){ //!da gestire 
			token = strtok_r(NULL, ";", &tmpstr);
			
			if(lockF(token, fd_c) == -1){
				free(msg);
				return -1;
			}
			Pthread_mutex_lock(&log_mtx); 
			fprintf(log_file,"%s %s -1 -1 %lu\n", "lock", token, pthread_self());
			fflush(log_file);
			Pthread_mutex_unlock(&log_mtx);
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
			//l'aggiornamento del file di log avviene all'interno di appendToF
		}
		else if(strcmp(token, "removef") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			
			if(removeF(token, fd_c) == -1){
				free(msg);
				return -1;
			}
			//l'aggiornamento del file di log avviene all'interno di removeF
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
				fprintf(stderr, "Il parametro di readNFiles deve essere un numero [server]\n"); //!!probabilmente devi inviare messaggio d'errore al client che aspetta su una read
			
			//l'aggiornamento del file di log avviene all'interno di readNF
		}
		else{
			fprintf(stderr, "Richiesta non supportata dal server: %s\n", token);
			if(reply_sender("Err:PathnameTroppoGrande", fd_c) == -1){
				perror("write richiesta non supportata [server]");
				free(msg);
				return -1;
			}
		}	
	}
	#ifdef DEBUG
		fprintf(stderr,"\n---STORAGE:\n");
		print_files();
	#endif	
	
	free(msg); 

	return 0;
}

//!
static void *init_thread(void *arg){ //CONSUMATORE
	int res, descr;
	Queue_t *task_queue = ((th_w_arg *)arg)->task_queue; 
	int pipe = ((th_w_arg *)arg)->pipe_w;
	
	while(1){ //!considera bene questa storia degli errori in executeTask, soprattutto perchè devi assicurarti di chiudere la connessione con il client se necessario (attualmente credo venga chiusa solo in caso di disconnect)
		int  *fd = pop(task_queue);	
		
		if(fd == NULL){
			fprintf(stderr,"Errore pop, init thread\n");
			pthread_exit(NULL);  
		}
		if(fd == EOS) //segnale di terminazione
            return (void*) 0;
		
		descr = *fd;
		res = executeTask(*fd);

		free(fd);
		if(res == -1){ //errori non fatali
			#ifdef DEBUG2
				fprintf(stderr, "Impossibile servire richiesta client %d\n", descr);
			#endif
		}
		if(res == -2 ){ //disconnessione client fd 
			descr = descr*-1;
		}
		if(res == -3){ //!errori fatali, attualmente nemmeno uno in executeTask, vedi se eliminarlo 
			descr = descr*-1;
			if(write(pipe, &descr, sizeof(int)) == -1){
				perror("write init_thread su pipe");
				return NULL; 
			}
			pthread_exit(NULL); //!perchè p_thread exit? vorrei chiudere proprio processo forse..
		}

		if(write(pipe, &descr, sizeof(int)) == -1){
			perror("write init_thread su pipe");
			return NULL;  
		}
	}
	
	return (void *) 0; 
}

void *submitTask(int descr, Queue_t *task_queue){ //PRODUTTORE
	int *fd = malloc(sizeof(int));
	*fd = descr;
	
    if (push(task_queue, fd) == -1) {
	    fprintf(stderr, "Errore: push\n");
	    return (void *) -1;
	}
	
	return (void *) 0;
}

int aggiorna_max(int fd_max, fd_set set){
	int i;
    for(i = fd_max - 1; i >= 0; i--)
        if (FD_ISSET(i, &set)) 
			return i;
 
    return -1;
}

int run_server(struct sockaddr_un *sa, int pipe, int sig_pipe, Queue_t *task_queue){
	
	int stop = 0;
	int fd_skt, fd_c, fd_r = 0;
	int fd_num = 0; //max fd attivo 
    int	fd; //indice per select
	int err = 0;
	
	fd_set set;   //fd attivi
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

	//aggiornamento fd_num e set 
	
	if(fd_skt > fd_num)
		fd_num = fd_skt;
		
	if(pipe > fd_num)
		fd_num = pipe;
	
	if(sig_pipe > fd_num)
		fd_num = sig_pipe;
	
	FD_ZERO(&set); //per evitare errori prima lo azzero
	FD_ZERO(&rdset); 

	FD_SET(fd_skt, &set);   //descrittore listen socket pronto
	FD_SET(pipe, &set);     //descrittore pipe comunicazione master-workers pronto
	FD_SET(sig_pipe, &set); //descrittore pipe comunicazione master-signal_handler pronto 
    	
	
	//ciclo di comunicazione con i workers 
	int n_conn = 0;
	int sighup = 0;
	
	while(!stop && !err){
		int res;
		
		rdset = set; //aggiorno ogni volta la maschera per la select, ne servono due perchè select modifica quello che gli passi
		if(sighup)  //interrompo il ciclo quando dopo aver ricevuto il segnale sighup non ci sono più connessioni attive 
			if(n_conn == 0)
				break;
		
		if((res = select(fd_num+1, &rdset, NULL, NULL, NULL)) == -1){ //si blocca finchè uno tra i descrittori in rdset non si blocca più su una read
			perror("select");
			err = 1;
			break;
		}

		//scorro i descrittori pronti nel rdset aggiornato da select 
				
		for(fd = 0; fd <= fd_num; fd++){ 
			
			if(FD_ISSET(fd, &rdset)){ //controllo quali di questi è pronto in lettura
			
				if(fd == fd_skt){ //listen socket pronto [richiesta connessione da nuovo client]
					
					if(!sighup){  //se non c'è stato il segnale sighup si accettano nuove connessioni 
						
						if((fd_c = accept(fd_skt, NULL, 0)) == -1){ 
							perror("accept");
							continue;
						}	
						Pthread_mutex_lock(&log_mtx); 
						fprintf(log_file,"%s %d -1 -1 %lu\n", "open_connection", fd_c, pthread_self());
						fflush(log_file);
						Pthread_mutex_unlock(&log_mtx);
					   
						FD_SET(fd_c, &set); //aggiungo alla maschera il descrittore per la comunicazione con il nuovo client 
						if(fd_c > fd_num) 
							fd_num = fd_c;
						
						n_conn++;
					}
				}
				else if(fd == pipe){ //pipe m-w pronta [comunicazione master-workers]
				
					if(readn(pipe, &fd_r, sizeof(int)) == -1){
						perror("READ PIPE MASTER:");
						err = 1;
						break;
					}
						
					if(fd_r < 0){ //connessione client fd_r chiusa 
						n_conn--; 
						fd_r = fd_r*-1;
					    if(close(fd_r) == -1)
							perror("close fd client");
						
						if(fd_r == fd_num) {
							if((fd_num = aggiorna_max(fd_r, set)) == -1)
								err = 1; 
						}
					}
					else{
						FD_SET(fd_r, &set); //descrittore di nuovo pronto a ricevere richieste 
						if(fd_r > fd_num) 
							fd_num = fd_r;
					}					
				}
				else if(fd == sig_pipe){ //pipe m-sig pronta [comunicazione master-signal_handler]
					
					if(readn(sig_pipe, &fd_r, sizeof(int)) != -1){
						if(fd_r > 0) //sigint, sigquit
							stop = 1;
						else
							sighup = 1;
					}
					else{
						perror("READ SIG_PIPE MASTER:");
						err = 1;
						break;
					}
				}
				else{ //descrittore connessione pronto [richiesta client]
					
					FD_CLR(fd, &set);  //lo riaggiungerò quando la richiesta sarà stata adempiuta 
					
					if(fd == fd_num) {
						if((fd_num = aggiorna_max(fd, set)) == -1)
							err = 1; 
					}
					if(submitTask(fd, task_queue) == (void *) -1)
						fprintf(stderr, "impossibile mettere il task %d in coda\n", fd);  
				}					
			}
			
		}
		
	}
	
	
	if(close(fd_skt) == -1)
		perror("close fd listen socket"); //pipe e sig_pipe chiuse nel main 
	for(fd = 0; fd < fd_num+1; fd++){
		if(FD_ISSET(fd, &rdset) && fd != pipe && fd != sig_pipe && fd != fd_skt){
			if(close(fd) == -1)
				perror("close descrittori in seguito a interruzione");
		}
	}

	//ATTENZIONE ci possono essere dei casi in cui il descrittore è stato tolto dalla maschera
	//perchè se ne sta occupando un thread
	//ma così può succere che quando arriva l'interruzione questo non venga chiuso 
	
	if(err)
		return -1;
	
	return 0;
}
	
//!
int main(int argc, char *argv[]){
	
	char *socket, *log, *aux, *token;
	char *tmpstr = NULL;
	int n_workers, n;
	FILE *fp;
	int ret_val = -1;
	
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
		else{
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
						token[strcspn(token, "\n")] = '\0';
						strncpy(log, token, UNIX_PATH_MAX); 
					}
					else{
						fprintf(stderr, "errore in config file: log value errato, verrà usato valore di default\n");
					}
				}
				else
					fprintf(stderr, "formato config file errato, verranno usati valori di default dove necessario\n");
			}
		
			if(ferror(fp))
				perror("errore fgets config file");
			
			if(fclose(fp) != 0)
				perror("chiusura config file");
			
			free(aux);
		}
	}
	
	//inizializzo variabili di storage (inizialmente vuoto)
	st_dim = 0;
	n_files = 0;
	
	//apro log_file 
	if((log_file = fopen(log,"w")) == NULL){ //"w" ok (controllato)
		perror("opening log file");
		cleanup_server(&free, T_PNT, log, socket, NULL); 
		return -1; 
	}
	
	fprintf(log_file, "Operazione - File - byte scritti - byte letti - thread\n\n");
    fflush(log_file);
	
	//inizializzazione pipe m-ws (comunic. master - workers)
	int pfd[2];
	
	if(pipe(pfd) == -1){ 
		perror("creazione pipe");
		cleanup_server(&free, T_PNT, log, socket, NULL); //attenzione alla storia log/socket di default 
		fclose(log_file);
		return -1;
	}
	
	//inizializzazione pipe signal (comunic. master - signal handler)
	int sig_pfd[2];
	
	if(pipe(sig_pfd) == -1){
		perror("creazione signal pipe");
		cleanup_server(&free, T_PNT, log, socket, NULL); 
		cleanup_server((void (*)(void *))&close, T_INT, &pfd[0], &pfd[1], NULL);
		fclose(log_file);
		return -1;
	}
	
	//inizializzazione socket (comunic. server-clients)
	struct sockaddr_un sa;
	
	memset(&sa, 0, sizeof(struct sockaddr_un));
	strncpy(sa.sun_path, socket, UNIX_PATH_MAX);

	sa.sun_family = AF_UNIX;
	
	//inizializzazione storage 
	if((hash_table = icl_hash_create(n_files_MAX, hash_pjw, string_compare)) == NULL){
		fprintf(stderr, "Errore nella creazione della hash table\n");
		cleanup_server(&free, T_PNT, log, socket, NULL); 
		cleanup_server((void (*)(void *))&close, T_INT, &pfd[0], &pfd[1], &sig_pfd[0], &sig_pfd[1], NULL);
		fclose(log_file);
		return -1;
	}
	
	//inizializzazione task_queue
	Queue_t * task_queue = initQueue();
    if (!task_queue) {
		fprintf(stderr, "initQueue su task_q fallita\n");
		cleanup_server(&free, T_PNT, log, socket, NULL); 
		cleanup_server((void (*)(void *))&close, T_INT, &pfd[0], &pfd[1], &sig_pfd[0], &sig_pfd[1], NULL);
		fclose(log_file);
		icl_hash_destroy(hash_table, &free, &free_data_ht);
		exit(errno); //controlla
    }
	
	//inizializzazione files_queue
	files_queue = initQueue();
    if (!files_queue) {
		fprintf(stderr, "initQueue su file_q fallita\n");
		cleanup_server(&free, T_PNT, log, socket, NULL); 
		cleanup_server((void (*)(void *))&close, T_INT, &pfd[0], &pfd[1], &sig_pfd[0], &sig_pfd[1], NULL);
		fclose(log_file);
		icl_hash_destroy(hash_table, &free, &free_data_ht);
		deleteQueue(task_queue);
		exit(errno); //controlla
    }
	
	//Inizializzazione thread pool 
	pthread_t th[n_workers];
	
	th_w_arg *w_args;

	if((w_args = malloc(sizeof(th_w_arg))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		cleanup_server(&free, T_PNT, log, socket, NULL); 
		cleanup_server((void (*)(void *))&close, T_INT, &pfd[0], &pfd[1], &sig_pfd[0], &sig_pfd[1], NULL);
		cleanup_server((void (*)(void *))&deleteQueue, T_QUEUE, files_queue, task_queue, NULL);
		fclose(log_file);
		icl_hash_destroy(hash_table, &free, &free_data_ht);
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
		goto _cleanup;
    }
    
	// ignoro SIGPIPE per evitare di essere terminato da una scrittura su un socket senza lettori 
	// Magari il client fa una richiesta e chiude la comunicazione senza attendere la risposta
	// il server non deve terminare in questo caso (il comportamento di default è quit)
    
	struct sigaction s;
    memset(&s,0,sizeof(s));    
    
	s.sa_handler = SIG_IGN;

    if ((sigaction(SIGPIPE, &s, NULL)) == -1) {   
		perror("sigaction");
		goto _cleanup;
    } 
	
	//inizializzo thread gestore dei segnali
	pthread_t sighandler_thread;
	
	//gli devo passare anche il descrittore di lettura della pipe 
	//la maschera viene ereditata, ma la devo comunque inviare per poter fare la wait 
    
	th_sig_arg *sig_args;

	if((sig_args = malloc(sizeof(th_sig_arg))) == NULL){
		perror("malloc");
		goto _cleanup;
	}
	
	sig_args->mask = &mask_s;
	sig_args->pipe_s = sig_pfd[1];
	
	
    if (pthread_create(&sighandler_thread, NULL, sigHandler, sig_args) != 0) {
		fprintf(stderr, "errore nella creazione del signal handler thread\n");
		goto _cleanup;
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
			goto _cleanup;
		}
		
		if(pthread_create(&th[i], NULL, &init_thread, w_args) != 0){ //creo il thread che inizia ad eseguire init_thread
			perror("Err in creazione thread");
			goto _cleanup;
		}
		
	}
	//avvio server 
	run_server(&sa, pfd[0], sig_pfd[0], task_queue); //devo fare il check del valore di ritorno?
   
    for (int i = 0; i < n_workers; i++) 
        push(task_queue, EOS);
	
	 // aspetto la terminazione de signal handler thread
    if(pthread_join(sighandler_thread, NULL) != 0){
		perror("Err in join sighandler thread");
		goto _cleanup;
	}
  
	for(int i = 0; i < n_workers; i++){ 
		if(pthread_join(th[i], NULL) != 0){ //aspetto terminino tutti i thread (la  join mi permette di liberare la memoria occupata dallo stack privato del thread)
			perror("Err in join thread");
			goto _cleanup;
		}
	}
	
	//stampa sunto operazioni effettutate dal server
	
	fprintf(stdout, "\n\nLISTA FILE:\n\n");
	fflush(stdout);
	print_files();
	
	print_summ(log);  //chiudendo log, faccio di default anche fflush
	
	ret_val = 0;
	
	//pulizia server
_cleanup:	
	icl_hash_destroy(hash_table, &free, &free_data_ht);
	cleanup_server((void (*)(void *))&deleteQueue, T_QUEUE, files_queue, task_queue, NULL);
	cleanup_server(&free, T_PNT, log, socket, w_args, sig_args, NULL); //attenzione alla storia log/socket di default 
	cleanup_server((void (*)(void *))&close, T_INT, &pfd[0], &pfd[1], &sig_pfd[0], &sig_pfd[1], NULL);
	fclose(log_file);
	//socket descriptor chiuso in run_server 
	
	return ret_val;
}

int cleanup_server(void (func)(void *), type t, void *args, ...){
	va_list list;
	va_start(list, args);
	void *elm;
	
	if(args != NULL){
		switch(t){
			case T_INT: (*(int(*)(int))(*func))(*(int *)args);
						while((elm = va_arg(list, void *)) != NULL)
							(*(int(*)(int))(*func))(*(int *)elm);
						break;
			case T_QUEUE: (*(void(*)(Queue_t *))(*func))((Queue_t *)args);
						while((elm = va_arg(list, void *)) != NULL)
							(*(void(*)(Queue_t *))(*func))((Queue_t *)elm);
						break;
			case T_PNT: (*func)(args);
						while((elm = va_arg(list, void *)) != NULL)
							(*func)(elm);
						break;
			default: fprintf(stderr, "tipo errato passato a cleanup_server");
					 return -1;	
		}
	}
	
	va_end(list);
	
	return 0;
}

void free_data_ht(void *data){ //funzione di pulizia del campo valore dell'hashtable 
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
	
int print_summ(char *log){ //stampa sunto operazioni effettutate dal server

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
    fprintf(stdout, "\nIl numero massimo di file memorizzati nel server è: %d\n", cnt_nf_MAX);
	
	fprintf(stdout, "La dimensione massima in MBytes raggiunta dal file storage è: %.3f\n", ((float) cnt_dim_MAX)/1000);
	
	fprintf(stdout, "L'algoritmo di rimpiazzamento è stato eseguito %d volte\n\n", cnt_rimp);

	fclose(fd);
	free(buffer);
	
	return 0;
}

file_info *init_file(int lock_owner, int fd, int lst_op, char *cnt, int sz){
	
	file_info *file;
	open_node *client;
	  
	if((file = malloc(sizeof(file_info))) == NULL){
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
		free(file);
		exit(errno_copy);
	}
	client->fd = fd;	
	client->next = NULL;
	file->open_owners = client; 
	
	file->lock_owner = lock_owner;
	file->lst_op = lst_op;
	
	if(sz <= 0)  
		sz = 0;
	
	file->cnt_sz = sz;
	if(sz > 0)
		memcpy(file->cnt, cnt, sz);  
	
	if (pthread_cond_init(&file->cond, NULL) != 0) {
		perror("mutex cond");
		return NULL;
	}

	return file; //viene liberato quando faccio la delete_ht o destroy_ht 
}

void print_files(){
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

int reply_sender(char *msg, int fd){
	int n = strlen(msg) + 1;
	if(writen(fd, &n, sizeof(int)) == -1)
		return -1;
	if(writen(fd, msg, n) == -1)
		return -1;
	return 0;
}

//tra find e insert non posso eliminare la lock, o un thread potrebbe inserirlo
//idem tra rimpiazzamento e delate (lì cerco i file da eliminare e poi li elimino, non posso rischiare qualcuno li modifichi
int openFC(char *path, int fd){ //free(data) viene fatto da delete_ht/destroy_ht
	file_info *data;
	char *mess;
	int errno_copy;
	char *path_storage;
	char *path_queue;
	
	Pthread_mutex_lock(&ht_mtx);
	
	if(icl_hash_find(hash_table, path) != NULL){ 
		if(reply_sender("Err:FileEsistente", fd) == -1)
			perror("openFC [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
//vedi se lo puoi fare fuori dalla sez critica
		//alloca memoria per il path che sarà salvato sullo storage 
		if((path_storage = malloc((strlen(path)+1)*sizeof(char))) == NULL){ 
			perror("malloc");
			errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		strncpy(path_storage, path, strlen(path)+1);
		
		data = init_file(-1, fd, 0, NULL, -1);  
		if(data == NULL){
			if(reply_sender("Err:CreazioneFile", fd) == -1)
				perror("openFC [server]");
			free(path_storage);
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}

		file_repl *aux = NULL;
		file_repl *prec = NULL;
		int res;
		
		//!!!rimpiazzamento 
		Pthread_mutex_lock(&server_info_mtx);
		res = st_repl(0, fd, path, &aux, 1);
		
		if(res == -1) //per ora lo faccio tutto lockato 
			mess = "Err:RimpiazzamentoFile";
		if(res == -2)
			mess = "Err:FileTroppoGrande";
		if(res == -1 || res == -2){
			if(reply_sender(mess, fd) == -1)
				perror("openFC [server]");
			Pthread_mutex_unlock(&server_info_mtx);
			free_data_ht(data);
			free(path_storage);
			Pthread_mutex_unlock(&ht_mtx);;
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
					if(reply_sender("Err:RimozioneFile", fd) == -1)
						perror("openFC [server]");
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

		if(icl_hash_insert(hash_table, path_storage, (void *) data) == NULL){
			if(reply_sender("Err:InserimentoFile", fd) == -1)
				perror("openFC [server]");
			Pthread_mutex_unlock(&server_info_mtx);
			Pthread_mutex_unlock(&ht_mtx);
			free_data_ht(data);
			free(path_storage);
			return -1;
		}
		
		n_files++; 
		Pthread_mutex_unlock(&server_info_mtx);
		Pthread_mutex_unlock(&ht_mtx);
		
		//alloca memoria per il path che sarà salvato sulla coda 
		if((path_queue = malloc((strlen(path)+1)*sizeof(char))) == NULL){ 
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		strncpy(path_queue, path, strlen(path)+1);
		
		if (push(files_queue, path_queue) == -1) {
			errno_copy = errno;
			fprintf(stderr, "Errore: push\n");
			if(reply_sender("Err:PushReplQueue", fd) == -1)
				perror("openFC [server]");
			free(path_queue);
			exit(errno_copy);	
		}
		
		
		if(reply_sender("Ok", fd) == -1){
			perror("openFC [server]");
			return -1;
		}
		
		#ifdef DEBUG3
			fprintf(stderr,"---OPEN CREATE FILE\n");
			print_files();
		#endif 

	}
	return 0;
}

int openFL(char *path, int fd){
	file_info *tmp;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("openFL [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
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
				if(reply_sender("Err:FileNonEsistente", fd) == -1)
					perror("openFL [server]");
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}
		}
		tmp->lock_owner = fd;
		insert_head(&(tmp->open_owners), fd);
		tmp->lst_op = 0;
		
		Pthread_mutex_unlock(&ht_mtx);
		
		if(reply_sender("Ok", fd) == -1){
			perror("openFL [server]");
			return -1;
		}	
	}
	
	#ifdef DEBUG3
		fprintf(stderr,"---OPEN LOCKED FILE\n");
		print_files();
	#endif
	
	return 0;
}

int openFO(char *path, int fd){
	file_info *tmp;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("openFO [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	} 
	else{ //se sta cercando di aprire un file che ha già aperto, lo ignoro (per ora)
		insert_head(&(tmp->open_owners), fd);
		tmp->lst_op = 0;
		
		Pthread_mutex_unlock(&ht_mtx);
		if(reply_sender("Ok", fd) == -1){
			perror("openFO [server]");
			return -1;
		}
	}
	#ifdef DEBUG3
		fprintf(stderr,"---OPEN FILE \n");
		print_files();
	#endif
	
	return 0;
	
}

int openFCL(char *path, int fd){
		file_info *data;
		char *path_storage;
	    char *path_queue;
		char * mess;
		int errno_copy;
		
		Pthread_mutex_lock(&ht_mtx);
		if(icl_hash_find(hash_table, path) != NULL){ 
			if(reply_sender("Err:FileEsistente", fd) == -1)
				perror("openFCL [server]");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		} 
		else{ 
			if((path_storage = malloc((strlen(path)+1)*sizeof(char))) == NULL){ 
				perror("malloc");
				errno_copy = errno;
				fprintf(stderr,"FATAL ERROR: malloc\n");
				exit(errno_copy);
			}
			strncpy(path_storage, path, strlen(path)+1);
			
			data = init_file(fd, fd, 1, NULL, -1);  
			if(data == NULL){
				if(reply_sender("Err:CreazioneFile", fd) == -1)
					perror("openFCL [server]");
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
				mess = "Err:RimpiazzamentoFile";
			if(res == -2)
				mess = "Err:FileTroppoGrande";
			if(res == -1 || res == -2){
				if(reply_sender(mess, fd) == -1)
					perror("openFCL [server]");
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
						if(reply_sender("Err:RimozioneFile", fd) == -1)
							perror("openFCL [server]");
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
				if(reply_sender("Err:InserimentoFile", fd) == -1)
							perror("openFCL [server]");
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
				errno_copy = errno;
				fprintf(stderr, "Errore: push\n");
				if(reply_sender("Err:PushReplQueue", fd) == -1)
					perror("openFC [server]");
				exit(errno_copy);
		    }
			if(reply_sender("Ok", fd) == -1){
				perror("openFC [server]");
				return -1;
			}	
		}
		#ifdef DEBUG3
			fprintf(stderr,"---OPEN CREATE LOCKED FILE\n");
			print_files();
		#endif
		
		return 0;
	}
	
int closeF(char *path, int fd){ //quando chiudo un file rilascio anche la lock se la possiedo
	file_info *tmp;
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("closeF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->open_owners == NULL || find_elm(tmp->open_owners, fd) != 0){
		if(reply_sender("Err:FileNonAperto", fd) == -1)
			perror("closeF [server]");
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
		
		if(reply_sender("Ok", fd) == -1){
			perror("closeF [server]");
			return -1;
		}
	}
	#ifdef DEBUG3
		fprintf(stderr,"---CLOSE FILE \n");
		print_files();
	#endif
	
	return 0;
}

int readF(char *path, int fd){
	file_info *tmp;

	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("readF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->open_owners == NULL || find_elm(tmp->open_owners, fd) != 0){
		if(reply_sender("Err:FileNonAperto", fd) == -1)
			perror("readF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->lock_owner != fd && tmp->lock_owner != -1 ){
		if(reply_sender("Err:FileLocked", fd) == -1)
			perror("readF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
		tmp->lst_op = 0;
		
		if(writen(fd, &tmp->cnt_sz, sizeof(int)) == -1){
			perror("readF [server]");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		if(tmp->cnt_sz > 0){
			if(writen(fd, tmp->cnt, tmp->cnt_sz) == -1){
				perror("readF [server]");
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
			
	#ifdef DEBUG3
		fprintf(stderr,"---READ FILE \n");
		print_files();
	#endif
	
	return 0;
			
}

int writeF(char *path, char *cnt, int sz, int fd){
	file_info *tmp;
	char *mess;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("writeF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->open_owners == NULL || find_elm(tmp->open_owners, fd) != 0){
		if(reply_sender("Err:FileNonAperto", fd) == -1)
			perror("writeF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if (tmp->lst_op == 0) {
		if(reply_sender("Err:InvalidLastOperation", fd) == -1)
			perror("writeF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->lock_owner != fd){ //questo me lo assicura l'if precedente in realtà 
		if(reply_sender("Err:FileNoLocked", fd) == -1)
			perror("writeF [server]");
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
				mess = "Err:RimpiazzamentoFile";
			if(res == -2)
				mess = "Err:FileTroppoGrande";
			if(res == -1 || res == -2){
				pthread_cond_signal(&tmp->cond); 
				#ifdef DEBUG2
					Pthread_mutex_lock(&log_mtx);
					fprintf(log_file,"%s %s -1 -1 %lu\n", "SIGNAL", path, pthread_self());
					fflush(log_file);
					Pthread_mutex_unlock(&log_mtx);
				#endif
				if(icl_hash_delete(hash_table, path, &free, &free_data_ht) == -1){ 
					if(reply_sender("Err:RimozioneFile", fd) == -1)
						perror("writeF [server]");
					Pthread_mutex_unlock(&server_info_mtx);
					Pthread_mutex_unlock(&ht_mtx);
					return -1;
				}
				if(reply_sender(mess, fd) == -1)
					perror("writeF [server]");
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
						if(reply_sender("Err:RimozioneFile", fd) == -1)
							perror("writeF [server]");
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
		else
			tmp->cnt_sz = 0; 
		Pthread_mutex_unlock(&ht_mtx);
		
		Pthread_mutex_lock(&log_mtx); 
		fprintf(log_file,"%s %s %d -1 %lu\n", "writeF", path, sz, pthread_self());
		fflush(log_file);
		Pthread_mutex_unlock(&log_mtx);
		
		if(reply_sender("$", fd) == -1){
			perror("writeF [server]");
			return -1;
		}

		if(reply_sender("Ok", fd) == -1){
			perror("writeF [server]");
			return -1;
		}
	}
	#ifdef DEBUG3
		fprintf(stderr,"---WRITE FILE \n");
		print_files();
	#endif
	
	return 0;
}

int lockF(char *path, int fd){
	file_info *tmp;	
    
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("lockF [server]");
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
				if(reply_sender("Err:FileNonEsistente", fd) == -1)
					perror("lockF [server]");
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}
		}
		tmp->lock_owner = fd;
		tmp->lst_op = 0; 
	}
	Pthread_mutex_unlock(&ht_mtx);
	if(reply_sender("Ok", fd) == -1){
		perror("lockF [server]");
		return -1;
	}
	#ifdef DEBUG5
		fprintf(stderr,"---LOCK FILE \n");
		print_files();
	#endif
	
	return 0;

}
 
int unlockF(char *path, int fd){
	file_info *tmp;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("unlockF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
    }
	else if(tmp->lock_owner != fd){
		if(reply_sender("Err:FileNoLocked", fd) == -1)
			perror("unlockF [server]");
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
		if(reply_sender("Ok", fd) == -1){
			perror("unlockF [server]");
			return -1;
		}
	}
	#ifdef DEBUG3
		fprintf(stderr,"---UNLOCK FILE \n");
		print_files();
	#endif
	
	return 0;
}

int appendToF(char *path, char *cnt, int sz, int fd){
	file_info *tmp;	
	int res;
	char *mess;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("appendToF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->open_owners == NULL || find_elm(tmp->open_owners, fd) != 0){
			if(reply_sender("Err:FileNonAperto", fd) == -1)
				perror("appendToF [server]");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
	else if(tmp->lock_owner != fd){
			if(reply_sender("Err:FileNoLocked", fd) == -1)
				perror("appendToF [server]");
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
				mess = "Err:RimpiazzamentoFile";
			if(res == -2)
				mess = "Err:FileTroppoGrande";							
			
			if(res == -1 || res == -2){
				if(reply_sender(mess, fd) == -1)
					perror("appendToF [server]");
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
						if(reply_sender("Err:RimozioneFile", fd) == -1)
							perror("appendToF [server]");
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
			if(sz > 0){
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
		
		if(reply_sender("$", fd) == -1){
			perror("appendToF [server]");
			return -1;
		}
		if(reply_sender("Ok", fd) == -1){
			perror("appendToF [server]");
			return -1;
		}		
	}
	#ifdef DEBUG
		fprintf(stderr,"---APPEND TO FILE \n");
		print_files();
	#endif

	return 0;
}

int removeF(char *path, int fd){ 
	file_info *tmp;
	int old_sz;
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("removeF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->lock_owner != fd){
		if(reply_sender("Err:FileNoLocked", fd) == -1)
			perror("removeF [server]");
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
			if(reply_sender("Err:RimozioneFile", fd) == -1)
				perror("removeF [server]");
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
		
		if(reply_sender("Ok", fd) == -1){
			perror("removeF [server]");
			return -1;
		}
		#ifdef DEBUG3
			fprintf(stderr,"---REMOVE FILE\n");
			print_files();
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
	
    if(reply_sender("$", fd) == -1){
		perror("readNF [server]");
		return -1;
	}
    if(reply_sender("Ok", fd) == -1){
		perror("readNF [server]");
		return -1;
	}
	#ifdef DEBUG3
		fprintf(stderr,"---SEND N FILES\n");
		print_files();
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
					((file_info *)value)->lock_owner = -1; 
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
	Pthread_mutex_unlock(&ht_mtx); 
	
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
							push(files_queue, exp);
							return -2;
						}
					}
					else{
						found = 1;
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
		
	if(reply_sender(file->path, fd) == -1){
		perror("file_sender [server]");
		return -1;
	}
	if(writen(fd, &(file->pnt->cnt_sz), sizeof(int)) == -1){
		perror("file_sender [server]");
		return -1;
	}
	if(file->pnt->cnt_sz > 0){
		if(writen(fd, file->pnt->cnt , file->pnt->cnt_sz) == -1){
			perror("file_sender [server]");
			return -1;
		}
	}
		
	return 0;
}

/*
int isNumber(void *el, int *n){
	char *e = NULL;
	errno = 0;
	*n = strtol(el, &e, 10);
	if(errno == ERANGE) return 2;
	if(e != NULL && *e == (char) 0)
		return 0; 
	return 1;
}*/