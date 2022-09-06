CC = gcc 

#qui da aggiungere c99 
CFLAGS = -Wall -pedantic 

#TARGETS = server client clean test
TARGETS = server_4s client clean test 

#genera tutti gli eseguibili
all	: $(TARGETS) 

.PHONY : clean, test, cleanall

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
test : 
	./client -p -f /home/giulia/Server_storage/mysock -t 200 -D exp -d savings -w dirw -W sasuke,itachi,sasuke -r sasuke,itachi -R 6 -l itachi -c itachi -l sasuke -u sasuke -h  & valgrind --tool=memcheck --track-origins=yes --leak-check=full --show-leak-kinds=all ./server_4s -k config.txt  
	
test2 :
	./client -p -f /home/giulia/Server_storage/mysock -D exp -d savings -w dirw -W sasuke,itachi,sasuke -t 5 -r sasuke,itachi -R 6 -h  & valgrind --tool=memcheck --track-origins=yes --leak-check=full --show-leak-kinds=all ./server_4s -k config.txt 
	
test1 : $(TARGETS)
	#chmod +x ./script_t1.sh 
	bash ./script_t1.sh &