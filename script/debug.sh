#!/bin/bash

if [ $# != 1 ]; then
    echo "Deve essere passato 1 file .log come argomento" 
    exit 0
fi

NAMES[0]='alice'
NAMES[1]='bobby'
NAMES[2]='carl'
NAMES[3]='denny'
NAMES[4]='elvis'
NAMES[5]='frank'
NAMES[6]='ginny'
NAMES[7]='harry'
NAMES[8]='itachi'
NAMES[9]='lenny'
NAMES[10]='minnie'
NAMES[11]='nancy'


tmpfile=./file

for((i=0;i<12;++i)); do
cnt=0
grep -E "SIGNAL.*${NAMES[i]}|WAIT.*${NAMES[i]}" $1 > $tmpfile 
exec 3<$tmpfile    # apro il file in lettura e gli assegno il descrittore 3 
while IFS=" " read -u 3 line; do #leggo riga per riga
    elem=$(echo $line| cut -d" " -f 1) #prendo il primo campo (operazione)
	if [[ "$elem" = "SIGNAL" && "$cnt" != 0 ]]; then 
		(( cnt -= 1))
	elif [[ "$elem" = "WAIT" ]];then
		(( cnt += 1))
	fi
done
exec 3<&- 
echo "cnt ${NAMES[i]} è uguale a $cnt"
done


cnt=$(grep "open_connection" $1 | wc -l) 
echo "N. di open_connection: " $cnt 

cnt=$(grep "close_connection" $1 | wc -l) 
echo "N. di close_connection: " $cnt 

