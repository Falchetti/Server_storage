SHELL = /bin/bash

CC = gcc 

CFLAGS = -Wall -pedantic -g 

#TARGETS = server client clean test
TARGETS = server_4s client clean test1

#genera tutti gli eseguibili
all : $(TARGETS) 

.PHONY : clean, test1, test2, test3, cleanall

server_4s : server_4s.o icl_hash.o queue.o
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

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
clean :	
	-rm -f mysock  

cleanall : 
	-rm -f *.o *.~ 
	
test1 : 
	valgrind --leak-check=full ./server_4s -k config1.txt &
	chmod +x test_1.sh 
	./test_1.sh
	killall -s SIGHUP memcheck-amd64-

test2 : 
	./server_4s -k config2.txt  &
	chmod +x test_2.sh 
	./test_2.sh
	kill -SIGHUP `pidof server_4s`
	
test3 : 
	./server_4s -k config3.txt  &
	chmod +x test_3.sh 
	./test_3.sh
	kill -SIGINT `pidof server_4s`
	
