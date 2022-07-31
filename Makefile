CC = gcc 

#qui da aggiungere c99 
CFLAGS = -Wall -pedantic 

TARGETS = server client clean test

#genera tutti gli eseguibili
all	: $(TARGETS) 

.PHONY : clean, test, cleanall

server : server.o icl_hash.o 
	$(CC) $(CFLAGS) $^ -o $@

#così se modifico solo icl_hash questo modulo oggetto non viene ricreato 
#MA questo sarebbe vero solo se non cancellassi con clean i moduli oggetto 
server.o : server.c icl_hash.h 
	$(CC) $(CFLAGS) -c $< -o $@

client : client.o api.o
	$(CC) $(CFLAGS) $^ -o $@
	
client.o : client.c api.h
	$(CC) $(CFLAGS) -c $< -o $@
	
icl_hash.o : icl_hash.c icl_hash.h 
	$(CC) $(CFLAGS) -c $< -o $@
	
api.o : api.c api.h
	$(CC) $(CFLAGS) -c $< -o $@

#phony target
#lo fa all'inizio invece che alla fine, vedi come risolvere 
clean :	
	-rm -f mysock  

cleanall : 
	-rm -f *.o *.~ 
	
#questo test probabilmente sarà sostituito con uno script 
test2 : 
	./client -f /home/giulia/Server_storage/mysock -r pippo,minnie -r pluto -f sock -r paperondepaperoni -l pippo,minnie -l pluto -u pluto -h & ./server
	
test :
	./client -f /home/giulia/Server_storage/mysock -W pippo,minnie -r pippo -r minnie  & ./server