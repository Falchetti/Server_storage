
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
//#include <signal.h>
#include <string.h>
//#include <stdarg.h>

#include "storage_f.h"
#include "list.h"
#include "conn.h"
#include "util.h"
#include "icl_hash.h"


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

int reply_sender(char *msg, int fd){
	int n = strlen(msg) + 1;
	if(writen(fd, &n, sizeof(int)) == -1)
		return -1;
	if(writen(fd, msg, n) == -1)
		return -1;
	return 0;
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
