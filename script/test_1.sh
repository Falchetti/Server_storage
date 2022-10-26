#!/bin/bash

P="./test_files/"
D="./test_files/output_dir/"

./bin/client -p -f ./mysock.sk -t 200 -D ${D}exp -d ${D}savings -w ${P}folder_1 -W ${P}alice,${P}bobby,${P}carl,${P}alice -r ${P}alice,${P}carl -R 5 -l ${P}alice,${P}bobby -u ${P}alice -c ${P}bobby,${P}danny -h & 
PID=$! 

wait $PID