#!/bin/bash

valgrind --leak-check=full ./server_4s -k config.txt &
echo "Avvio del Server:"
# Get its PID
PID=$!
export PID
sleep 2 #attesa post avvio server
./client -p -f /home/giulia/Server_storage/mysock -t 2 -D exp -d savings -w dirw -W sasuke,itachi,sasuke -r sasuke,itachi -R 6 -l itachi -c itachi -l sasuke -u sasuke -h 
#invio di sighup al server

kill -s SIGINT $PID

