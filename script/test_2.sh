#!/bin/bash
P="./test_files/"
D="./test_files/output_dir/"
sleep 2
k=0
./bin/client -p -f ./mysock.sk -D ${D}exp -W ${P}alice,${P}bobby,${P}carl,${P}danny,${P}elvis,${P}frank -h &
PID[k]=$! 
((k++))
./bin/client -p -f ./mysock.sk -D ${D}exp -W ${P}ginny,${P}harry,${P}itachi,${P}lenny,${P}minnie,${P}nancy -h & 
PID[k]=$! 
((k++))

for((i=0;i<k;++i)); do
    wait ${PID[i]}
done

