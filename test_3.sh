#!/bin/bash

end=$((SECONDS+30))

while [ $SECONDS -lt $end ]; do

./client -t 0 -f /home/giulia/File_server_storage/Server_storage/mysock -W alice,bobby,carl -l alice -u alice &
./client -t 0 -f /home/giulia/File_server_storage/Server_storage/mysock -W alice,bobby,carl -l alice,bobby & 
./client -t 0 -f /home/giulia/File_server_storage/Server_storage/mysock -W lenny,danny,bobby,ginny,harry &
./client -t 0 -f /home/giulia/File_server_storage/Server_storage/mysock -W ginny,harry,itachi -r harry,itachi & 
./client -t 0 -f /home/giulia/File_server_storage/Server_storage/mysock -W alice,carl,ginny -w folder_1 &
./client -t 0 -f /home/giulia/File_server_storage/Server_storage/mysock -W harry,itachi,alice,bobby,carl & 
./client -t 0 -f /home/giulia/File_server_storage/Server_storage/mysock -W itachi,lenny,lenny,lenny,lenny &
./client -t 0 -f /home/giulia/File_server_storage/Server_storage/mysock -W lenny,minnie,itachi -l lenny -u lenny &
./client -t 0 -f /home/giulia/File_server_storage/Server_storage/mysock -W minnie,alice,bobby,carl,danny &
./client -t 0 -f /home/giulia/File_server_storage/Server_storage/mysock -W lenny,minnie,alice -R 2 & 

sleep 1

done 

