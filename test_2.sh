#!/bin/bash

sleep 2
k=0
./client -p -f /home/giulia/File_server_storage/Server_storage/mysock -W alice,bobby,carl,danny,elvis,frank -h &
PID[k]=$! 
((k++))
./client -p -f /home/giulia/File_server_storage/Server_storage/mysock -W ginny,harry,itachi,lenny,minnie,nancy -h & 
PID[k]=$! 
((k++))

for((i=0;i<k;++i)); do
    wait ${PID[i]}
done

