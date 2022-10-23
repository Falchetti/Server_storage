#ATTENZIONE: se scrivo 'make test1' non mi rimuove mysock e non funziona bene (inoltre non aggiorna i file se modificati) a differenza di make
SHELL = /bin/bash

CC = gcc 

#qui da aggiungere c99 
CFLAGS = -Wall -Werror -pedantic -g 

#TARGETS = server client clean test
TARGETS = server client clean test3

#genera tutti gli eseguibili
all : $(TARGETS) 

.PHONY : clean, test1, test2, test3, cleanall

server : server.o icl_hash.o queue.o
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

server.o : server.c icl_hash.h queue.h
	$(CC) $(CFLAGS) -c -g $< -o $@ -lpthread

#così se modifico solo icl_hash questo modulo oggetto non viene ricreato 
#MA questo sarebbe vero solo se non cancellassi con clean i moduli oggetto 
	
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
#lo fa all'inizio invece che alla fine, vedi come risolvere!
clean :	
	-rm -f mysock  

cleanall : 
	-rm -f *.o *.~ 
	
test5 :
	./client -p -f /home/giulia/File_Server_storage/Server_storage/mysock -D exp -d savings -W sasuke,alice,itachi,carl -R -3 -l carl -u carl -h & valgrind --leak-check=full ./server
	
test4 : 
	./client -f /home/giulia/File_Server_storage/Server_storage/mysock -W pippo,sasuke -r pluto -f sock -r paperondepaperoni -l pippo,minnie -l pluto -u pluto -h & ./server
# --tool=memcheck --track-origins=yes	--show-leak-kinds=all
	
test1 : 
	valgrind --leak-check=full ./server -k config_1.txt &
	chmod +x test_1.sh 
	./test_1.sh
	killall -s SIGHUP memcheck-amd64-

test2 : 
	./server -k config_2.txt  &
	chmod +x test_2.sh 
	./test_2.sh
	kill -SIGHUP `pidof server`
	
test3 : 
	./server -k config_3.txt  &
	chmod +x test_3.sh 
	./test_3.sh
	kill -SIGINT `pidof server`
	

	
