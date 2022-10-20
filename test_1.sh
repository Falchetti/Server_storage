#!/bin/bash

./client -p -f /home/giulia/File_server_storage/Server_storage/mysock -t 200 -D exp1 -d save1 -w dirw -W alice,bobby,carl -r alice,carl -R 2 -u carl -l bobby -c bobby -h & 
PID=$! 

wait $PID