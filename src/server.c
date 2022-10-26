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
#define NBUCKETS  100 // n. di file max nello storage 
#define ST_DIM 1024   //capacità storage  
#define THREAD_NUM 4  //numero di thread workers 
#define LOGNAME "./log.txt" 

#define EOS (void*)0x1 //valore speciale usato per indicare End-Of-Stream (EOS)


//***strutture dati utilizzate***//

//struttura utilizzata nella funzione di cleanup del server 
typedef enum type { 
    T_PNT,
    T_INT,
	T_QUEUE
} type;

//struttura dei valori nell'hashtable (storage)
typedef struct file_info{ 
	int lock_owner;      //detentore della lock
	open_node *open_owners;  //lista dei client che tengono il file aperto
	int lst_op;         //flag di controllo per la writeFile
	char cnt[MAX_SIZE]; //contenuto file
	int cnt_sz;         //taglia contenuto 
	pthread_cond_t cond;
} file_info;

//entry della coda per i rimpiazzamenti 
typedef struct file_repl{ 
	char *path;
	file_info *pnt;
	struct file_repl *next;
} file_repl;

//argomento thread signal_handler 
typedef struct th_sig_arg {  
    sigset_t *mask;
    int pipe_s;
} th_sig_arg;

//argomento thread workers
typedef struct th_w_arg {  
    Queue_t *task_queue;
    int pipe_w;
} th_w_arg;

//***funzioni di supporto al server e di gestione dello storage***//

//funzioni per la comunicazione con il client
int file_sender(file_repl *list, int fd);
int reply_sender(char *msg, int fd);

//funzioni di stampa 
int print_summ(char *log);
void print_files();

//funzioni di supporto allo storage 
file_info *init_file(int lock_owner, int fd, int lst_op, char *cnt, int sz);
int st_repl(int dim, char *path, file_repl **list, int isCreate);

//funzioni di pulizia 
void free_data_ht(void *data);
int clean_storage (int fd);
int cleanup_server(void (func)(void *), type t, void *args, ...);

//funzioni che realizzano l'interfaccia dello storage 
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


//***variabili globali***//

//coda per gestire il rimpiazzamento 
Queue_t *files_queue; 

//hashtable per implementare lo storage 
icl_hash_t *hash_table = NULL;
pthread_mutex_t ht_mtx = PTHREAD_MUTEX_INITIALIZER;

//variabili di dimensione dello storage 
int st_dim;
int st_dim_MAX;
int n_files;
int n_files_MAX;
pthread_mutex_t server_info_mtx = PTHREAD_MUTEX_INITIALIZER;

//puntatore al file di log
FILE *log_file; 
pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;

/** Funzione di gestione dei segnali eseguita dal signal-handler thread.
 *  In caso di segnale SIGINT/SIGQUIT comunica al master thread un valore positivo,
 *  nel caso di ricezione del segnale SIGHUP un valore negativo
 *
 *   \param struttura contente maschera dei segnali da attendere e
 *          pipe di comunicazione con il master thread 
 *  
 *   \retval NULL 
 **/
static void *sigHandler(void *arg) { 
    sigset_t *set = ((th_sig_arg *)arg)->mask; 
	int pipe_s = ((th_sig_arg *)arg)->pipe_s;

    for( ;; ) {
		int sig;
		int rit;
		int r = sigwait(set, &sig); 
		if (r != 0) {
			errno = r;
			perror("FATAL ERROR 'sigwait'");
			return NULL;
		}
		//segnale ricevuto 
		switch(sig) { 
			case SIGINT: 
			case SIGQUIT:

				rit = 1;
				if(writen(pipe_s, &rit, sizeof(int)) == -1)
					perror("Write sig_handler");   	    
				return NULL;
			case SIGHUP: 
				rit = -1;
				if(writen(pipe_s, &rit, sizeof(int)) == -1)
					perror("Write sig_handler");
				
				return NULL;  
			default:  ; 
		}
    }
    return NULL;	   
}

/** Funzione di ricezione e decodifica di una richiesta da un dato client. 
 *
 *   \param descrittore dal client da cui si aspetta una richiesta 
 *
 *   \retval 0 in caso di successo
 *   \retval -1 in caso di errore 
 *   \retval -2 in caso di disconnessione del client 
 **/
int executeTask(int fd_c){ 
		
	char *msg; 
	char *tmpstr = NULL;
    char *token, *token2;
	int n, sz_msg;
	
	//lettura richiesta client [cmd;path;sz_cnt]
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
		return -1;    
	}
	if(readn(fd_c, msg, sz_msg) == -1){
		perror("read socket lato server");
		free(msg);
		return -1;   
	}
	
	#ifdef DEBUG
		fprintf(stderr, "N_FILES: %d / %d, ST_DIM: %d / %d\n", n_files, n_files_MAX, st_dim, st_dim_MAX);
		fprintf(stderr, "\n********Contenuto canale di comunicazione: %s********\n\n", msg);
	#endif
	
	token = strtok_r(msg, ";", &tmpstr);
	
    //decodifica richiesta client e chiamata corretta funzione di gestione della richiesta 
	if(token != NULL){
		
		if(strcmp(token, "openfc") == 0){
			token = strtok_r(NULL, ";", &tmpstr);
			
			if(openFC(token, fd_c) == -1){
				free(msg);
				return -1;
			} 
			//inserisco nel file di log solo le operazioni andate a buon fine 
			Pthread_mutex_lock(&log_mtx); 
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
		else if(strcmp(token, "disconnesso") == 0){
			//elimina lock ownership e open ownership che il client aveva sui file 			
			clean_storage(fd_c); 
			
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
		else if(strcmp(token, "lockf") == 0){ 
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
			else{
				fprintf(stderr, "Il parametro di readNFiles deve essere un numero [server]\n"); 
				if (reply_sender("Err:ParametroErrato", fd_c) == -1){
					perror("write parametro errato readF [server]");
					free(msg);
					return -1;
				}
			}
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

/** Funzione di estrazione dei task da parte dei worker threads.
 *  Il thread estrae dalla coda un descrittore. Se è il valore speciale EOS termina,
 *  se è un client attivo chiama la funzione di esecuzione del task.
 *  In base al valore di ritorno di quest'ultima scrive sulla pipe il descrittore
 *  in forma negativa (il client si è disconnesso) o positiva.
 *
 *   \param struttura contenente coda di task e 
 *          pipe di comunicazione con il master thread
 *
 *   \retval 0 in caso di successo
 *   \retval NULL in caso di errore 
 **/
static void *init_thread(void *arg){ 
	int res, descr;
	Queue_t *task_queue = ((th_w_arg *)arg)->task_queue; 
	int pipe = ((th_w_arg *)arg)->pipe_w;
	
	while(1){ 
		int  *fd = pop(task_queue);	
		
		if(fd == NULL){
			fprintf(stderr,"Errore pop, init thread\n");
			pthread_exit(NULL);  
		}
		//valore di terminazione
		if(fd == EOS) 
            return (void*) 0;
		
		descr = *fd;
		res = executeTask(*fd);

		free(fd);
		if(res == -1){ //errori 
			#ifdef DEBUG
				fprintf(stderr, "Impossibile servire richiesta client %d\n", descr);
			#endif
		}
		if(res == -2 ){ //disconnessione client fd 
			descr = descr*-1;
		}
		//comunicazione con il master thread 
		if(write(pipe, &descr, sizeof(int)) == -1){
			perror("write init_thread su pipe");
			return NULL;  
		}
	}
	
	return (void *) 0; 
}

/** Inserisce nella coda task un nuovo descrittore
 *
 *   \param descrittore da inserire nella coda dei task 
 *   \param coda dei task
 *
 *   \retval 0 in caso di successo
 *   \retval -1 in caso di errore 
 **/
void *submitTask(int descr, Queue_t *task_queue){ 
	int *fd = malloc(sizeof(int));
	*fd = descr;
	
    if (push(task_queue, fd) == -1) {
	    fprintf(stderr, "Errore: push\n");
	    return (void *) -1;
	}
	
	return (void *) 0;
}

/** Aggiorna il il descrittore di indice massimo 
 *
 *   \param descrittore massimo attuale 
 *   \param set di descrittori 
 *
 *   \retval nuovo descrittore di valore massimo 
 *   \retval -1 in caso di errore 
 **/
int aggiorna_max(int fd_max, fd_set set){
	int i;
    for(i = fd_max - 1; i >= 0; i--)
        if (FD_ISSET(i, &set)) 
			return i;
 
    return -1;
}

/** Apre connessione socket e si mette in ascolto su di essa e 
 *  sulle pipe condivise con i thread. 
 *  In questo modo il master thread gestisce la distribuzione dei task ai thread workers,
 *  riceve l'avviso dell'arrivo di un segnale dal thread signal-handler e 
 *  attiva nuove connessioni con i client.
 *
 *   \param struttura dati su cui è definito il nome del socket  
 *   \param descrittore pipe comunicazione con i thread workers
 *   \param descrittore pipe comunicazione con il thread signal-handler  
 *   \param coda dei task
 *
 *   \retval 0 in caso di successo
 *   \retval -1 in caso di errore 
 **/
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
	
	FD_ZERO(&set); //per evitare errori viene azzerato 
	FD_ZERO(&rdset); 

	FD_SET(fd_skt, &set);   //descrittore listen socket pronto
	FD_SET(pipe, &set);     //descrittore pipe comunicazione master-workers pronto
	FD_SET(sig_pipe, &set); //descrittore pipe comunicazione master-signal_handler pronto 
    	
	
	//ciclo di comunicazione con i workers 
	int n_conn = 0;
	int sighup = 0;
	
	while(!stop && !err){ //si interrompe in caso di errore o ricezione dei segnali sigint/sigquit 
		int res;
		
		rdset = set; 
		if(sighup)  //interrompe il ciclo quando dopo aver ricevuto il segnale sighup non ci sono più connessioni attive 
			if(n_conn == 0)
				break;
		
		if((res = select(fd_num+1, &rdset, NULL, NULL, NULL)) == -1){ 
			perror("select");
			err = 1;
			break;
		}

		//scorre i descrittori pronti nel rdset aggiornato da select 
		for(fd = 0; fd <= fd_num; fd++){ 
			
			if(FD_ISSET(fd, &rdset)){ //controlla quali di questi è pronto in lettura
			
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
					    //aggiunge alla maschera il descrittore per la comunicazione con il nuovo client 
						FD_SET(fd_c, &set); 
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
					
					FD_CLR(fd, &set);  //lo riaggiungerà quando la richiesta sarà stata adempiuta 
					
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
		perror("close fd listen socket"); //pipe e sig_pipe vengono chiuse nel main 
	for(fd = 0; fd < fd_num+1; fd++){
		if(FD_ISSET(fd, &rdset) && fd != pipe && fd != sig_pipe && fd != fd_skt){
			if(close(fd) == -1)
				perror("close descrittori in seguito a interruzione");
		}
	}
	
	if(err)
		return -1;
	
	return 0;
}
	
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
					if(isNumber(token, &n) == 0 && n > 0) 
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
	
	//inizializzazione variabili di storage (inizialmente vuoto)
	st_dim = 0;
	n_files = 0;
	
	//aperura log_file 
	if((log_file = fopen(log,"w")) == NULL){ 
		perror("opening log file");
		cleanup_server(&free, T_PNT, log, socket, NULL); 
		return -1; 
	}
	
	fprintf(log_file, "Operazione - File - byte scritti - byte letti - thread\n\n");
    fflush(log_file);
	
	//inizializzazione pipe m-ws [comunicazione master - workers]
	int pfd[2];
	
	if(pipe(pfd) == -1){ 
		perror("creazione pipe");
		cleanup_server(&free, T_PNT, log, socket, NULL); 
		fclose(log_file);
		return -1;
	}
	
	//inizializzazione pipe signal [comunicazione master - signal handler]
	int sig_pfd[2];
	
	if(pipe(sig_pfd) == -1){
		perror("creazione signal pipe");
		cleanup_server(&free, T_PNT, log, socket, NULL); 
		cleanup_server((void (*)(void *))&close, T_INT, &pfd[0], &pfd[1], NULL);
		fclose(log_file);
		return -1;
	}
	
	//inizializzazione socket [comunicazione server - clients]
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
		exit(errno); 
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
		exit(errno); 
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
	
	
	sigset_t     mask_s;
    sigemptyset(&mask_s); 
    sigaddset(&mask_s, SIGINT);  
    sigaddset(&mask_s, SIGQUIT);
    sigaddset(&mask_s, SIGHUP);    
	
    if (pthread_sigmask(SIG_BLOCK, &mask_s,NULL) != 0) {  
		fprintf(stderr, "FATAL ERROR\n");  
		goto _cleanup;
    }
    
	//ignora SIGPIPE per evitare di essere terminato da una scrittura su un socket senza lettori 
    
	struct sigaction s;
    memset(&s,0,sizeof(s));    
    
	s.sa_handler = SIG_IGN;

    if ((sigaction(SIGPIPE, &s, NULL)) == -1) {   
		perror("sigaction");
		goto _cleanup;
    } 
	
	//inizializzazione thread gestore dei segnali
	pthread_t sighandler_thread;
	    
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
	
	for(int i = 0; i < n_workers; i++){ 
	
		sigset_t mask;
		sigemptyset(&mask);
		sigaddset(&mask, SIGINT); 
		sigaddset(&mask, SIGQUIT);
		sigaddset(&mask, SIGHUP);

		if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) { 
			fprintf(stderr, "FATAL ERROR\n");
			goto _cleanup;
		}
		
		if(pthread_create(&th[i], NULL, &init_thread, w_args) != 0){ 
			perror("Err in creazione thread");
			goto _cleanup;
		}
		
	}
	//avvio server 
	run_server(&sa, pfd[0], sig_pfd[0], task_queue); 
   
    for (int i = 0; i < n_workers; i++) 
        push(task_queue, EOS);
	
	//attesa di terminazione del signal handler thread
    if(pthread_join(sighandler_thread, NULL) != 0){
		perror("Err in join sighandler thread");
		goto _cleanup;
	}
    //attesa di terminazione dei thread workers 
	for(int i = 0; i < n_workers; i++){ 
		if(pthread_join(th[i], NULL) != 0){ 
			perror("Err in join thread");
			goto _cleanup;
		}
	}
	
	//stampa sunto operazioni effettutate dal server
	fprintf(stdout, "\n\nLISTA FILE:\n\n");
	fflush(stdout);
	print_files();
	
	print_summ(log);  
	
	ret_val = 0;
	
	//pulizia server
_cleanup:	
	icl_hash_destroy(hash_table, &free, &free_data_ht);
	cleanup_server((void (*)(void *))&deleteQueue, T_QUEUE, files_queue, task_queue, NULL);
	cleanup_server(&free, T_PNT, log, socket, w_args, sig_args, NULL); //attenzione alla storia log/socket di default 
	cleanup_server((void (*)(void *))&close, T_INT, &pfd[0], &pfd[1], &sig_pfd[0], &sig_pfd[1], NULL);
	fclose(log_file);
	
	return ret_val;
}

//***funzioni di supporto al server e di gestione dello storage***//

/** Funzione generica di pulizia del server. Chiude file e libera memoria allocata.
 *
 *   \param funzione da utilizzare  
 *   \param tipo degli argomenti della funzione 
 *   \param argomenti della funzione
 *
 *   \retval 0 in caso di successo
 *   \retval -1 in caso di errore 
 **/
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

/** Funzione di pulizia del campo valore dell'hashtable.
 *
 *   \param dato da deallocare
 **/
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

/** Stampa il sunto delle operazioni effettuate dal server.
 *  Analizza il file di log e ne estrapola alcune informazioni.
 *
 *   \param pathname del file di log 
 *
 *   \retval 0 in caso di successo
 *   \retval -1 in caso di errore 
 **/	
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
	//eliminazione dell'intestazione 
	fgets(buffer, MAX_SIZE, fd);
	fgets(buffer, MAX_SIZE, fd);
	
	while(fgets(buffer, MAX_SIZE, fd) != NULL) {
		//controlla di aver letto tutta una riga
		if ((newline = strchr(buffer, '\n')) == NULL) {
			fprintf(stderr, "buffer di linea troppo piccolo");
			fclose(fd);
			free(buffer);
			return -1;
		}
		*newline = '\0';  //elimina lo '\n'
		
		cmd = strtok_r(buffer, " ", &tmpstr);
		strtok_r(NULL, " ", &tmpstr);  //il file di riferimento non serve
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

/** Inizializza il campo valore di un file da inserire nello storage 
 *
 *   \param descrittore del client detentore della lock sul file   
 *   \param descrittore del client che ha aperto il file 
 *   \param flag di controllo sull'ultima operazione effettuata sul file 
 *   \param contenuto file
 *   \param taglia contenuto file 
 *
 *   \retval puntatore alla struttura creata in caso di successo
 *   \retval NULL in caso di errore 
 **/
file_info *init_file(int lock_owner, int fd, int lst_op, char *cnt, int sz){
	
	file_info *file;
	open_node *client;
	  
	if((file = malloc(sizeof(file_info))) == NULL){
		perror("malloc");
		int errno_copy = errno;
		fprintf(stderr,"FATAL ERROR: malloc\n");
		exit(errno_copy);
	}
	
	//inizializzazione della lista degli open_owners  
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

	return file; 
}

/** Stampa i file presenti nello storage   
 **/
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

/** Invio di un messaggio secondo il protocollo sz-msg al client.
 *
 *   \param messaggio da inviare
 *   \param descrittore del canale di comunicazione su cui inviarlo 
 *
 *   \retval 0 caso di successo
 *   \retval -1 in caso di errore 
 **/
int reply_sender(char *msg, int fd){
	int n = strlen(msg) + 1;
	if(writen(fd, &n, sizeof(int)) == -1)
		return -1;
	if(writen(fd, msg, n) == -1)
		return -1;
	return 0;
}

/** Funzione di pulizia dello storage.
 *  Libera le strutture dati da ogni traccia lasciata da un determinato client.
 *
 *   \param descrittore relativo ad un client 
 *
 *   \retval 0 caso di successo
 **/
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

/** Funzione di rimpiazzamento utilizzata in caso di capacity misses.
 *
 *   \param taglia del contenuto del file che ha causato l'avvio dell'algoritmo di rimpiazzamento  
 *   \param file che ha causato l'avvio dell'algoritmo di rimpiazzamento  
 *   \param lista su cui verranno salvati i file da espellere  
 *   \param flag per distinguere richieste di creazione o scrittura  
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 *   \retval -2 nel caso di errore dovuto a file troppo grande 
 **/
int st_repl(int dim, char *path, file_repl **list, int isCreate){
	char *exp;
	int found = 0;
	int found_self = 0;
	int tmp_st_dim = st_dim;
	file_info *tmp;
	file_repl *new;

	if(isCreate){ //algoritmo di rimpiazzamento invocato da funzione che crea un nuovo file
		if(n_files >=  n_files_MAX){ //esiste almeno un posto 
			while(!found){
				exp = pop(files_queue); 
				if((tmp = icl_hash_find(hash_table, exp)) == NULL) //il file è stato eliminato dallo storage 
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
	else{  //algoritmo di rimpiazzamento invocato da funzione che scrive un file 
		if(dim > st_dim_MAX){ //file troppo grande 
			return -2;
		}
		else{
			while(dim + tmp_st_dim > st_dim_MAX){
				found = 0;
				//estraggo i file dalla coda finchè non ne trovo uno ancora presente nello storage 
				while(!found){ 
					exp = pop(files_queue); 
					if((tmp = icl_hash_find(hash_table, exp)) == NULL){ 
						free(exp);
						continue;
					}
					if(strncmp(exp, path, UNIX_PATH_MAX) == 0){ //non deve rimuovere se stesso dalla coda 
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

/** Invio di file da parte del server al client.
 *
 *   \param file da inviare 
 *   \param descrittore del canale di comunicazione su cui inviarlo 
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 **/
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

//***funzioni che realizzano l'interfaccia dello storage***//

/** Creazione e apertura di un file.
 *  Necessita l'esecuzione dell'algoritmo di rimpiazzamento perchè può causare capacity misses. 
 *
 *   \param pathname del file
 *   \param descrittore del client di riferimento  
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 **/
int openFC(char *path, int fd){ 
	file_info *data;
	char *mess;
	int errno_copy;
	char *path_storage;
	char *path_queue;
	
	Pthread_mutex_lock(&ht_mtx);
	
	if(icl_hash_find(hash_table, path) != NULL){ //controlla se il file esiste 
		if(reply_sender("Err:FileEsistente", fd) == -1)
			perror("openFC [server]");
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
		
		data = init_file(-1, fd, 0, NULL, -1);  //inizializza il file 
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
		
		Pthread_mutex_lock(&server_info_mtx);
		res = st_repl(0, path, &aux, 1);  //controlla se il suo inserimento comporta capacity misses 
		
		if(res == -1) 
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
			while(aux != NULL){ //elimina dallo storage file espulsi 
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
        //inserisce il file nello storage e aggiorna le sue variabili di dimensione 
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
		
		//inserisce il file nella coda 
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
		//invia esito al client 
		if(reply_sender("Ok", fd) == -1){
			perror("openFC [server]");
			return -1;
		}
	}
	return 0;
}

/** Apertura e lock di un file. 
 *
 *   \param pathname del file
 *   \param descrittore del client di riferimento  
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 **/
int openFL(char *path, int fd){
	file_info *tmp;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ //controlla se il file esiste 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("openFL [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	} 
	else{ 
		while(tmp->lock_owner != -1 && tmp->lock_owner != fd){ //attende che la lock sul file sia rilasciata
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
		//aggiorna le informazioni del file 
		tmp->lock_owner = fd;                
		insert_head(&(tmp->open_owners), fd);
		tmp->lst_op = 0;
		
		Pthread_mutex_unlock(&ht_mtx);
		//invia esito al client 
		if(reply_sender("Ok", fd) == -1){
			perror("openFL [server]");
			return -1;
		}	
	}
	
	return 0;
}

/** Apertura di un file. 
 *
 *   \param pathname del file
 *   \param descrittore del client di riferimento  
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 **/
int openFO(char *path, int fd){
	file_info *tmp;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ //controlla se il file esiste 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("openFO [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	} 
	else{ 
		//aggiorna le informazioni del file 
		insert_head(&(tmp->open_owners), fd); 
		tmp->lst_op = 0;
		
		Pthread_mutex_unlock(&ht_mtx);
		//invia esito al client 
		if(reply_sender("Ok", fd) == -1){
			perror("openFO [server]");
			return -1;
		}
	}
	
	return 0;
	
}

/** Creazione, apertura e lock di un file.
 *  Necessita l'esecuzione dell'algoritmo di rimpiazzamento perchè può causare capacity misses. 
 *
 *   \param pathname del file
 *   \param descrittore del client di riferimento  
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 **/
int openFCL(char *path, int fd){
		file_info *data;
		char *path_storage;
	    char *path_queue;
		char * mess;
		int errno_copy;
		
		Pthread_mutex_lock(&ht_mtx);
		if(icl_hash_find(hash_table, path) != NULL){ //controlla se il file esiste 
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
			//crea il valore del file da inserire nello storage
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
		
			Pthread_mutex_lock(&server_info_mtx);
			//esegue algoritmo di rimpiazzamento per controllare se ci sono capacity misses 
			int res = st_repl(0, path, &aux, 1);

			if(res == -1) 
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
			if(aux != NULL){ //elimina file espulsi 
				while(aux != NULL){
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
						if(reply_sender("Err:RimozioneFile", fd) == -1)
							perror("openFCL [server]");
						Pthread_mutex_unlock(&server_info_mtx);
						Pthread_mutex_unlock(&ht_mtx);
						free_data_ht(data);
						free(path_storage);
						return -1;
					}	
					//aggiorna le informazioni di dimensione dello storage 
					st_dim -= sz_cpy;
					n_files--;
					aux = aux->next;
					free(prec->path);
					free(prec);
				}
			}
			//inserisce il nuovo file nello storage
			if(icl_hash_insert(hash_table, path_storage, data) == NULL){
				if(reply_sender("Err:InserimentoFile", fd) == -1)
							perror("openFCL [server]");
				Pthread_mutex_unlock(&server_info_mtx);
				Pthread_mutex_unlock(&ht_mtx);
				free_data_ht(data);
				return -1;
			}	
			//aggiorna le informazioni di dimensione dello storage
			n_files++; 
			Pthread_mutex_unlock(&server_info_mtx);
			Pthread_mutex_unlock(&ht_mtx);			
			
			//inserisce il file nella coda 
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
			//invia esito al client 
			if(reply_sender("Ok", fd) == -1){
				perror("openFC [server]");
				return -1;
			}	
		}
		
		return 0;
	}

/** Chiusura di un file. 
 *  Rilascia la lock se la possiede.
 *
 *   \param pathname del file
 *   \param descrittore del client di riferimento  
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 **/
int closeF(char *path, int fd){ 
	file_info *tmp;
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ //controlla se il file esiste 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("closeF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->open_owners == NULL || find_elm(tmp->open_owners, fd) != 0){ //controlla se il file è aperto
		if(reply_sender("Err:FileNonAperto", fd) == -1)
			perror("closeF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
		//aggiorna le informazioni del file (lo chiude e rilascia la lock se la possiede)
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
		//invia esito al client 
		if(reply_sender("Ok", fd) == -1){
			perror("closeF [server]");
			return -1;
		}
	}
	
	return 0;
}

/** Lettura di un file. 
 *
 *   \param pathname del file
 *   \param descrittore del client di riferimento  
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 **/
int readF(char *path, int fd){
	file_info *tmp;

	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ //controlla se il file esiste 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("readF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->open_owners == NULL || find_elm(tmp->open_owners, fd) != 0){ //controlla se il file è aperto
		if(reply_sender("Err:FileNonAperto", fd) == -1)
			perror("readF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->lock_owner != fd && tmp->lock_owner != -1 ){ //controlla se qualcun'altro ne possiede la lock 
		if(reply_sender("Err:FileLocked", fd) == -1)
			perror("readF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
		//invia il contenuto del file al client 
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
	
	return 0;
			
}

/** Scrittura di un file. 
 *  Necessita l'esecuzione dell'algoritmo di rimpiazzamento perchè può causare capacity misses.
 *
 *   \param pathname del file
 *   \param contenuto del file
 *   \param taglia contenuto del file
 *   \param descrittore del client di riferimento  
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 **/
int writeF(char *path, char *cnt, int sz, int fd){
	file_info *tmp;
	char *mess;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ //controlla se il file esiste 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("writeF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->open_owners == NULL || find_elm(tmp->open_owners, fd) != 0){ //controlla se il file è aperto
		if(reply_sender("Err:FileNonAperto", fd) == -1)
			perror("writeF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if (tmp->lst_op == 0) { //controlla se l'ultima operazione sul file è stata openFCL 
		if(reply_sender("Err:InvalidLastOperation", fd) == -1)
			perror("writeF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->lock_owner != fd){  //controlla se ne possiede la lock 
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
			//avvia algoritmo di rimpiazzamento per verificare possibili capacity misses 
			int res = st_repl(sz, path, &aux, 0);
			
			if(res == -1) 
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
				//elimina il file dallo storage se non è riuscito a scriverlo 
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
			//invia al client i file espulsi e li elimina dallo storage 
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
			//scrive file e aggiorna le informazioni di dimensione dello storage 
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
		//avvisa il client che ha terminato di inviare i file espulsi 
		if(reply_sender("$", fd) == -1){
			perror("writeF [server]");
			return -1;
		}
		//invia esito al client 
		if(reply_sender("Ok", fd) == -1){
			perror("writeF [server]");
			return -1;
		}
	}
	
	return 0;
}

/** Lock di un file. 
 *
 *   \param pathname del file
 *   \param descrittore del client di riferimento  
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 **/
int lockF(char *path, int fd){
	file_info *tmp;	
    
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ //controlla se il file esiste
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("lockF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
		while(tmp->lock_owner != -1 && tmp->lock_owner != fd){ //controlla se qualcun'altro ne possiede la lock 
			#ifdef DEBUG2
				Pthread_mutex_lock(&log_mtx);
				fprintf(log_file,"%s %s -1 -1 %lu\n", "WAIT",  path, pthread_self());
				fflush(log_file);
				Pthread_mutex_unlock(&log_mtx);
			#endif
			pthread_cond_wait(&tmp->cond,&ht_mtx);
			if((tmp = icl_hash_find(hash_table, path)) == NULL){ //controlla se il file esiste ancora 
				if(reply_sender("Err:FileNonEsistente", fd) == -1)
					perror("lockF [server]");
				Pthread_mutex_unlock(&ht_mtx);
				return -1;
			}
		}
		//aggiorna le informazioni del file (prenda la lock)
		tmp->lock_owner = fd;
		tmp->lst_op = 0; 
	}
	Pthread_mutex_unlock(&ht_mtx);
	//invia esito al client 
	if(reply_sender("Ok", fd) == -1){
		perror("lockF [server]");
		return -1;
	}
	
	return 0;

}
 
/** Unlock di un file. 
 *
 *   \param pathname del file
 *   \param descrittore del client di riferimento  
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 **/
int unlockF(char *path, int fd){
	file_info *tmp;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ //controlla se il file esiste 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("unlockF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
    }
	else if(tmp->lock_owner != fd){ //controlla se ne possiede la lock 
		if(reply_sender("Err:FileNoLocked", fd) == -1)
			perror("unlockF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
		//aggiorna le informazioni del file (rilascia la lock)
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
		//invia esito al client 
		if(reply_sender("Ok", fd) == -1){
			perror("unlockF [server]");
			return -1;
		}
	}
	
	return 0;
}

/** Scrittura in append di un file. 
 *  Necessita l'esecuzione dell'algoritmo di rimpiazzamento perchè può causare capacity misses.
 *
 *   \param pathname del file
 *   \param contenuto del file
 *   \param taglia contenuto del file
 *   \param descrittore del client di riferimento  
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 **/
int appendToF(char *path, char *cnt, int sz, int fd){
	file_info *tmp;	
	int res;
	char *mess;
	
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ //controlla se il file esiste 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("appendToF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->open_owners == NULL || find_elm(tmp->open_owners, fd) != 0){ //controlla se ha aperto il file 
			if(reply_sender("Err:FileNonAperto", fd) == -1)
				perror("appendToF [server]");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
	else if(tmp->lock_owner != fd){ //controlla di possedere la lock 
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
			//avvia l'algoritmo di rimpiazzamento per verificare possibili capacity misses
			res = st_repl(sz, path, &aux, 0);
			
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
			
			//invia al client i file espulsi e li elimina dallo storage 
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
			//scrive file e aggiorna le informazioni di dimensione dello storage 
			if(sz > 0){
				memcpy(tmp->cnt + tmp->cnt_sz, cnt, sz);
				tmp->cnt_sz += sz;
				st_dim +=sz; 
			}
			Pthread_mutex_unlock(&server_info_mtx); 
	    }
		Pthread_mutex_unlock(&ht_mtx);
		
		Pthread_mutex_lock(&log_mtx); 
		fprintf(log_file,"%s %s %d -1 %lu\n", "append", path, sz, pthread_self());
		fflush(log_file);
		Pthread_mutex_unlock(&log_mtx);
		
		//avvisa il client che ha terminato di inviare i file espulsi 
		if(reply_sender("$", fd) == -1){
			perror("appendToF [server]");
			return -1;
		}
		//invia esito al client 
		if(reply_sender("Ok", fd) == -1){
			perror("appendToF [server]");
			return -1;
		}		
	}

	return 0;
}

/** Rimozione di un file. 
 *
 *   \param pathname del file
 *   \param descrittore del client di riferimento  
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 **/
int removeF(char *path, int fd){ 
	file_info *tmp;
	int old_sz;
	Pthread_mutex_lock(&ht_mtx);
	if((tmp = icl_hash_find(hash_table, path)) == NULL){ //controlla se il file esiste 
		if(reply_sender("Err:FileNonEsistente", fd) == -1)
			perror("removeF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else if(tmp->lock_owner != fd){ //controlla se detiene la lock 
		if(reply_sender("Err:FileNoLocked", fd) == -1)
			perror("removeF [server]");
		Pthread_mutex_unlock(&ht_mtx);
		return -1;
	}
	else{
		old_sz = tmp->cnt_sz; //serve per inserire i byte rimossi nel file di log
		
        pthread_cond_signal(&tmp->cond); 
		#ifdef DEBUG2
			Pthread_mutex_lock(&log_mtx);
			fprintf(log_file,"%s %s -1 -1 %lu\n", "SIGNAL",  path, pthread_self());
			fflush(log_file);
			Pthread_mutex_unlock(&log_mtx);
		#endif      
		//elimina il file 
	    if(icl_hash_delete(hash_table, path, &free, &free_data_ht) == -1){ 
			if(reply_sender("Err:RimozioneFile", fd) == -1)
				perror("removeF [server]");
			Pthread_mutex_unlock(&ht_mtx);
			return -1;
		}
		
		Pthread_mutex_unlock(&ht_mtx);
		//aggiorna le informazioni di dimensione dello storage 
		Pthread_mutex_lock(&server_info_mtx);
		n_files--; 
		st_dim -= old_sz; 
		Pthread_mutex_unlock(&server_info_mtx);
		
		Pthread_mutex_lock(&log_mtx); 
		fprintf(log_file,"%s %s -1 %d %lu\n", "remove", path, old_sz, pthread_self());
		fflush(log_file);
		Pthread_mutex_unlock(&log_mtx);
		//invia esito al client 
		if(reply_sender("Ok", fd) == -1){
			perror("removeF [server]");
			return -1;
		}
	}
	return 0;
}

/** Lettura di N file.
 *  Se n <= 0 legge tutti i file presenti nello storage.  
 *
 *   \param descrittore del client di riferimento 
 *   \param numero di file da leggere  
 *
 *   \retval 0 nel caso di successo
 *   \retval -1 nel caso di errore 
 **/
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
	//scorre lo storage 
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
				//invia file al client 
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
	//avvisa il client che ha terminato di inviare i file espulsi 
    if(reply_sender("$", fd) == -1){
		perror("readNF [server]");
		return -1;
	}
	//invia esito al client 
    if(reply_sender("Ok", fd) == -1){
		perror("readNF [server]");
		return -1;
	}
	
	return 0;			
}

