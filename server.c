#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#define UNIX_PATH_MAX 108
#define SOCKNAME "./mysock"

int main(int argc, char *argv[]){
	int  fd_skt, fd_c; //socket passivo che accetta, socket per la comunicazione
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(struct sockaddr_un));
	strncpy(sa.sun_path, SOCKNAME, UNIX_PATH_MAX);
	sa.sun_family = AF_UNIX;
	
	
	
	
	if((fd_skt = socket(AF_UNIX, SOCK_STREAM, 0)) == -1){
		perror("ERRORE nella socket");
		return -1;
	}
	
	if(bind(fd_skt, (struct sockaddr *) &sa, sizeof(sa)) == -1){
		perror("ERRORE nella bind");
		return -1;
	} 
	fprintf(stderr, "Ci sono\n");

	listen(fd_skt, 20); //-1, 20 richieste in coda max
	fd_c = accept(fd_skt, NULL, 0); //-1, crea un nuoco socket per la comunicazione e ne restituisce il file descriptor
	
	write(fd_c, "Bye", 4);
	
	
	close(fd_skt);
	close(fd_c);
	
	return 0;
}