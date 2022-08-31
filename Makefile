CC = gcc 

#qui da aggiungere c99 
CFLAGS = -Wall -pedantic 

#TARGETS = server client clean test
TARGETS = server_3 client clean test 

#genera tutti gli eseguibili
all	: $(TARGETS) 

.PHONY : clean, test, cleanall

#server : server.o icl_hash.o 
#	$(CC) $(CFLAGS) $^ -o $@
#server_mw : server_mw.o icl_hash.o 
#	$(CC) $(CFLAGS) $^ -o $@ -lpthread
#server_q : server_q.o icl_hash.o queue.o
#	$(CC) $(CFLAGS) $^ -o $@ -lpthread
server_3 : server_3.o icl_hash.o queue.o
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

#così se modifico solo icl_hash questo modulo oggetto non viene ricreato 
#MA questo sarebbe vero solo se non cancellassi con clean i moduli oggetto 

#server.o : server.c icl_hash.h 
#	$(CC) $(CFLAGS) -c $< -o $@
#server_mw.o : server_mw.c icl_hash.h 
#	$(CC) $(CFLAGS) -c $< -o $@ -lpthread
#server_q.o : server_q.c icl_hash.h queue.h
#	$(CC) $(CFLAGS) -c $< -o $@ -lpthread
server_3.o : server_3.c icl_hash.h queue.h
	$(CC) $(CFLAGS) -c $< -o $@ -lpthread
	
queue.o : queue.c queue.h
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
	./client -f /home/giulia/Server_storage/mysock -W itachi,sasuke,sasuke,sasuke -c pippo  & ./server_3 -k config.txt