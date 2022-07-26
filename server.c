#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

#define UNIX_PATH_MAX 108
#define SOCKNAME "/home/giulia/Server_storage/mysock"
#define DEBUG
//#undef DEBUG

int main(int argc, char *argv[]){
	int  fd_skt, fd_c; //socket passivo che accetta, socket per la comunicazione
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(struct sockaddr_un));
	strncpy(sa.sun_path, SOCKNAME, UNIX_PATH_MAX);
	sa.sun_family = AF_UNIX;
	
	
	if((fd_skt = socket(AF_UNIX, SOCK_STREAM, 0)) == -1){
		perror("Errore nella socket");
		return -1;
	}
	
	if(bind(fd_skt, (struct sockaddr *) &sa, sizeof(sa)) == -1){
		perror("Errore nella bind");
		return -1;
	} 

	listen(fd_skt, 20); //gestire errore, 20 richieste in coda max
	
	fd_c = accept(fd_skt, NULL, 0); //gestire errore, crea un nuovo socket per la comunicazione e ne restituisce il file descriptor

	write(fd_c, "Prova", 5); //scrivo nel canale di comunicazione, controlla se 5 è corretto o serve spazio per il terminatore (idem in openconnection)
	
	#ifdef DEBUG
		char *buf = malloc(13*sizeof(char));
		read(fd_c, buf, 12);
		fprintf(stderr, "Contenuto canale di comunicazione: %s\n", buf);
	    free(buf);
	#endif
	close(fd_skt);
	close(fd_c);
	
	return 0;
}