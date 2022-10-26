#!/bin/bash

P="./test_files/"
end=$((SECONDS+30))

while [ $SECONDS -lt $end ]; do

./bin/client -t 0 -f ./mysock.sk -W ${P}alice,${P}bobby,${P}carl -l ${P}alice -u ${P}alice &
./bin/client -t 0 -f ./mysock.sk -W ${P}alice,${P}bobby,${P}carl -l ${P}alice,${P}bobby & 
./bin/client -t 0 -f ./mysock.sk -W ${P}lenny,${P}danny,${P}bobby,${P}ginny,${P}harry &
./bin/client -t 0 -f ./mysock.sk -W ${P}ginny,${P}harry,${P}itachi -r ${P}harry,${P}itachi & 
./bin/client -t 0 -f ./mysock.sk -W ${P}alice,${P}carl,${P}ginny -w ${P}folder_1 &
./bin/client -t 0 -f ./mysock.sk -W ${P}harry,${P}itachi,${P}alice,${P}bobby,${P}carl & 
./bin/client -t 0 -f ./mysock.sk -W ${P}itachi,${P}lenny,${P}lenny,${P}lenny,${P}lenny &
./bin/client -t 0 -f ./mysock.sk -W ${P}lenny,${P}minnie,${P}itachi -l ${P}lenny -u ${P}lenny &
./bin/client -t 0 -f ./mysock.sk -W ${P}minnie,${P}alice,${P}bobby,${P}carl,${P}danny &
./bin/client -t 0 -f ./mysock.sk -W ${P}lenny,${P}minnie,${P}alice -R 2 & 

sleep 1

done 

