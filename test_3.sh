#!/bin/bash

k=0
./client -t 0 -f /home/giulia/Server_storage/mysock -W alice,bobby,carl,danny,elvis,frank -h &
PID[k]=$! 
((k++))
./client -t 0 -f /home/giulia/Server_storage/mysock -W ginny,harry,itachi,lenny,minnie,nancy -h & 
PID[k]=$! 
((k++))
./client -t 0 f /home/giulia/Server_storage/mysock -W alice,bobby,carl,danny,elvis,frank -h &
PID[k]=$! 
((k++))
./client -t 0 -f /home/giulia/Server_storage/mysock -W ginny,harry,itachi,lenny,minnie,nancy -h & 
PID[k]=$! 
((k++))
./client -t 0 -f /home/giulia/Server_storage/mysock -W alice,bobby,carl,danny,elvis,frank -h &
PID[k]=$! 
((k++))
./client -t 0 -f /home/giulia/Server_storage/mysock -W ginny,harry,itachi,lenny,minnie,nancy -h & 
PID[k]=$! 
((k++))
./client -t 0 -f /home/giulia/Server_storage/mysock -W alice,bobby,carl,danny,elvis,frank -h &
PID[k]=$! 
((k++))
./client -t 0 -f /home/giulia/Server_storage/mysock -W ginny,harry,itachi,lenny,minnie,nancy -h & 
PID[k]=$! 
((k++))
./client -t 0 -f /home/giulia/Server_storage/mysock -W ginny,harry,itachi,lenny,minnie,nancy -h & 
PID[k]=$! 
((k++))
./client -t 0 -f /home/giulia/Server_storage/mysock -W ginny,harry,itachi,lenny,minnie,nancy -h & 
PID[k]=$! 
((k++))



for((i=0;i<k;++i)); do
    wait ${PID[i]}
done

