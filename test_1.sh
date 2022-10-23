#!/bin/bash

./client -p -f /home/giulia/File_server_storage/Server_storage/mysock -t 200 -D exp -d savings -w folder_1 -W alice,bobby,carl,alice -r alice,carl -R 5 -l alice,bobby -u alice -c bobby,danny -h & 
PID=$! 

wait $PID