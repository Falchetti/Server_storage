CC = gcc 

#qui da aggiungere c99 
CFLAGS = -Wall -pedantic 

TARGETS = server.o client.o clean test

#genera tutti gli eseguibili
all	: $(TARGETS) 

.PHONY : clean 

server.o : server.c icl_hash.c 
	$(CC) $(CFLAGS)  $^ -o $@

#controlla qui se mettere -c e come gestire i file.h 
client.o : client.c api.c 
	$(CC) $(CFLAGS) $^ -o $@

#phony target
clean :	
	@echo "Rimuovo il sock file"
	-rm mysock

#questo test probabilmente sarà sostituito con uno script 
test : 
	./client.o -f /home/giulia/Server_storage/mysock -r pippo -r pluto -f sock -r paperondepaperoni -r qui -r pippo -r quo -h & ./server.o