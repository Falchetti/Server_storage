SHELL = /bin/bash

CC = gcc 

CFLAGS = -Wall -pedantic -I $(INC)
#da usare nei compilatori dove è necessario specificare -std=c99:
#CFLAGS = -Wall -pedantic -std=c99 -D_GNU_SOURCE=1 -I $(INC)

INC = ./includes/
LIB =./lib/
EXE = bin/server bin/client

#genera tutti gli eseguibili
all : $(EXE) 

.PHONY : clean, cleanall, test1, test2, test3, statistiche

bin/server : objs/server.o objs/icl_hash.o objs/queue.o objs/list.o
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

objs/server.o : src/server.c $(INC)icl_hash.h $(INC)queue.h $(INC)conn.h $(INC)util.h $(INC)defines.h $(INC)list.h 
	$(CC) $(CFLAGS) -c $< -o $@ -lpthread
	
objs/queue.o : src/queue.c $(INC)queue.h $(INC)util.h 
	$(CC) $(CFLAGS) -c $< -o $@
	
objs/list.o : src/list.c $(INC)list.h 
	$(CC) $(CFLAGS) -c $< -o $@

bin/client : objs/client.o lib/libapi.a
	$(CC) $(CFLAGS) $^ -o $@ -L$(LIB)
	
objs/client.o : src/client.c $(INC)api.h $(INC)util.h $(INC)defines.h
	$(CC) $(CFLAGS) -c $< -o $@ 
	
objs/icl_hash.o : src/icl_hash.c $(INC)icl_hash.h 
	$(CC) $(CFLAGS) -c $< -o $@ 
	
lib/libapi.a : objs/api.o 
		ar rvs $@ $^ 
	
objs/api.o : src/api.c $(INC)api.h $(INC)conn.h $(INC)defines.h
	$(CC) $(CFLAGS) -c $< -o $@
	

#phony target
clean :	
	-rm -f *.sk 

cleanall : 
	-rm -f *.sk objs/*.o $(EXE) log.txt lib/*.a 
	-rm -r -f test_files/output_dir/*
	
test1 : $(EXE)
	valgrind --leak-check=full ./bin/server -k config/config_1.txt &
	chmod +x ./script/test_1.sh 
	./script/test_1.sh
	killall -s SIGHUP memcheck-amd64-

test2 : $(EXE)
	./bin/server -k config/config_2.txt  &
	chmod +x ./script/test_2.sh 
	./script/test_2.sh
	kill -SIGHUP `pidof server`
	
test3 : $(EXE)
	./bin/server -k config/config_3.txt  &
	chmod +x ./script/test_3.sh 
	./script/test_3.sh
	kill -SIGINT `pidof server`
	
statistiche : 
	chmod +x ./script/statistiche.sh 
	./script/statistiche.sh log.txt 
