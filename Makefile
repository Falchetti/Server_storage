SHELL = /bin/bash

CC = gcc 

#qui da aggiungere c99 
CFLAGS = -Wall -pedantic -std=c99 -Werror -g

#TARGETS = server client clean test
TARGETS = server_4s client clean test1

#genera tutti gli eseguibili
all : $(TARGETS) 

.PHONY : clean, test1, cleanall

#server : server.o icl_hash.o 
#	$(CC) $(CFLAGS) $^ -o $@
#server_mw : server_mw.o icl_hash.o 
#	$(CC) $(CFLAGS) $^ -o $@ -lpthread
#server_q : server_q.o icl_hash.o queue.o
#	$(CC) $(CFLAGS) $^ -o $@ -lpthread
server_4s : server_4s.o icl_hash.o queue.o
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

#così se modifico solo icl_hash questo modulo oggetto non viene ricreato 
#MA questo sarebbe vero solo se non cancellassi con clean i moduli oggetto 

#server.o : server.c icl_hash.h 
#	$(CC) $(CFLAGS) -c $< -o $@
#server_mw.o : server_mw.c icl_hash.h 
#	$(CC) $(CFLAGS) -c $< -o $@ -lpthread
#server_q.o : server_q.c icl_hash.h queue.h
#	$(CC) $(CFLAGS) -c $< -o $@ -lpthread
server_4s.o : server_4s.c icl_hash.h queue.h
	$(CC) $(CFLAGS) -c -g $< -o $@ -lpthread
	
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
test3 : 
	./client -f /home/giulia/Server_storage/mysock -W pippo,sasuke -r pluto -f sock -r paperondepaperoni -l pippo,minnie -l pluto -u pluto -h & ./server
# --tool=memcheck --track-origins=yes	--show-leak-kinds=all
	
test1 : 
	valgrind --leak-check=full ./server_4s -k config1.txt &
	chmod +x test_1.sh 
	./test_1.sh
	killall -s SIGHUP memcheck-amd64-

test2 : 
	./server_4s -k config.txt  &
	./script_test.sh
	kill -SIGHUP `pidof server_4s`
	
