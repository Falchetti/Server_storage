#!/bin/bash

#hai contato molte cose senza considerare l'esito dell'operazione, ma se ha esito negativo la metto o no sul file di log???
#per semplicità metterò nel file di log solo le operazioni con esito positivo 

if [ $# != 1 ]; then
    echo "Deve essere passato 1 file .log come argomento" 
    exit 0
fi

cnt=$(grep "readF\|readNF" $1 | wc -l)
echo "N. di read: " $cnt 


tmpfile=./file
grep "readF\|readNF" $1 > $tmpfile 
sum=0
exec 3<$tmpfile    # apro il file in lettura e gli assegno il descrittore 3 
while IFS=" " read -u 3 line; do #leggo riga per riga
    elem=$(echo $line| cut -d" " -f 4) #tolgo spazi duplicati e prendo il terzo campo
  	sum=$(echo "$sum+$elem" | bc -lq)  #sommo i due numeri con bc
done
exec 3<&-  # chiudo il descrittore 3 
avg=$(echo "$sum/$cnt" | bc -lq)
echo "$avg"| LC_ALL="C" awk '{printf "Size media delle letture in bytes:  %.2f \n", $1}'


cnt=$(grep "writeF\|append" $1 | wc -l)
echo "N. di write: " $cnt 


grep "writeF\|append" $1 > $tmpfile 
sum=0
exec 3<$tmpfile    # apro il file in lettura e gli assegno il descrittore 3 
while IFS=" " read -u 3 line; do #leggo riga per riga
    elem=$(echo $line| cut -d" " -f 3) #tolgo spazi duplicati e prendo il terzo campo
  	sum=$(echo "$sum+$elem" | bc -lq)  #sommo i due numeri con bc
done
exec 3<&-  # chiudo il descrittore 3 
avg=$(echo "$sum/$cnt" | bc -lq)
echo "$avg"| LC_ALL="C" awk '{printf "Size media delle scritture in bytes:  %.2f \n", $1}'

cnt=$(grep '^lock' $1 | wc -l)
echo "N. di operazioni di lock: " $cnt  


cnt=$(grep '^open_l' $1 | wc -l)
echo "N. di operazioni di open-lock: " $cnt  


cnt=$(grep '^unlock' $1 | wc -l)
echo "N. di operazioni di unlock: " $cnt  


cnt=$(grep '^close' $1 | wc -l)
echo "N. di operazioni di close: " $cnt  


cat $1 | grep "writeF\|append\|remove\|rimpiazzamento" | cut -d" " -f 1,3,4 > $tmpfile
sum=0
cnt=0
cnt_max=0
exec 3<$tmpfile    # apro il file in lettura e gli assegno il descrittore 3 
while IFS=" " read -u 3 line; do #leggo riga per riga
	op=$(echo $line | tr -s " " | cut -d" " -f 1)
	nBW=$(echo $line | tr -s " " | cut -d" " -f 2)
	nBR=$(echo $line | tr -s " " | cut -d" " -f 3)
	if [[ "$op" = "writeF" || "$op" = "append" ]]; then 
		#echo $op "nBW: " $nBW
		(( cnt += nBW))
		if [[ "$cnt" -ge "$cnt_max" ]]; then 
			cnt_max=$cnt
		fi
	elif [[ "$op" = "remove" || "$op" = "rimpiazzamento" ]];then
		#echo $op "nBR: " $nBR
		(( cnt -= nBR))
	fi
done
exec 3<&-  # chiudo il descrittore 3 
cnt_max=$(echo "$cnt_max/1000" | bc -lq)
echo "$cnt_max"| LC_ALL="C" awk '{printf "Dimensione massima in Mbytes raggiunta dallo storage:  %.2f \n", $1}'



cat $1 | grep "open_c\|open_cl\|remove\|rimpiazzamento" | cut -d" " -f 1 > $tmpfile
sum=0
cnt=0
cnt_max=0
exec 3<$tmpfile    # apro il file in lettura e gli assegno il descrittore 3 
while IFS=" " read -u 3 line; do #leggo riga per riga
	if [[ "$line" = "open_c" || "$line" = "open_cl" ]]; then 
		(( cnt += 1))
		if [[ "$cnt" -ge "$cnt_max" ]]; then 
			cnt_max=$cnt
		fi
	elif [[ "$line" = "remove" || "$line" = "rimpiazzamento" ]];then
		(( cnt -= 1))
	fi
done
exec 3<&-  # chiudo il descrittore 3 
echo "Dimensione massima in numero di file raggiunta dallo storage " $cnt_max


cnt=$(grep '^rimpiazzamento' $1 | wc -l)
echo "Numero volte in cui è stato usato l'algoritmo di rimpiazzamento: " $cnt  # l'opzione -e ci consente di usare simboli speciali come \t (tab)


cat $1 | grep -v 'rimpiazzamento' | grep -v "connessione" | cut -d" " -f 5  | sort -g | uniq -c > $tmpfile
exec 3<$tmpfile    # apro il file in lettura e gli assegno il descrittore 3 
while IFS=" " read -u 3 line; do #leggo riga per riga
    thread=$(echo $line| cut -d" " -f 2) #tolgo spazi duplicati e prendo il terzo campo
  	n_req=$(echo $line| cut -d" " -f 1)  #sommo i due numeri con bc
	echo -e "Il thread " $thread "ha servito " $n_req "richieste"
done
exec 3<&-  # chiudo il descrittore 3 


cat $1 | grep "open_connection\|close_connection" | cut -d" " -f 1 > $tmpfile
sum=0
cnt=0
cnt_max=0
exec 3<$tmpfile    # apro il file in lettura e gli assegno il descrittore 3 
while IFS=" " read -u 3 line; do #leggo riga per riga
	if [[ "$line" = "open_connection" ]]; then 
		(( cnt += 1))
		if [[ "$cnt" -ge "$cnt_max" ]]; then 
			cnt_max=$cnt
		fi
	elif [[ "$line" = "close_connection" ]];then
		(( cnt -= 1))
	fi
done
exec 3<&-  # chiudo il descrittore 3 
echo "Numero massimo di connessioni contemporanee: " $cnt_max

rm $tmpfile 


