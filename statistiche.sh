#!/bin/bash

#per semplicità inserisco nel file di log solo le operazioni con esito positivo 

if [ $# != 1 ]; then
    echo "Deve essere passato 1 file .log come argomento" 
    exit 0
fi

#seleziono righe che contengono readF o readNF dal file passato come argomento 
#e le conto 
cnt=$(grep "readF\|readNF" $1 | wc -l) #il \ è il carattere di escape per far riconoscere | come carattere speciale
echo "N. di read: " $cnt 


tmpfile=./file
grep "readF\|readNF" $1 > $tmpfile 
sum=0
exec 3<$tmpfile    # apro in lettura il file contenente le righe selezionate e gli assegno il descrittore 3 
while IFS=" " read -u 3 line; do #leggo riga per riga, mettendo la riga in line (IFS=" " dice a read che il delimitatore è " ")
    elem=$(echo $line| cut -d" " -f 4) #prendo il quarto campo (byte letti)
  	sum=$(echo "$sum+$elem" | bc -lq)  #sommo i due numeri con bc (basic calculator)
done
exec 3<&-  

if [[ "$cnt" -ne 0 ]]; then 
	avg=$(echo "$sum/$cnt" | bc -lq)
else
	avg=0
fi

echo "$avg"| LC_ALL="C" awk '{printf "Size media delle letture in bytes:  %.2f \n", $1}'


cnt=$(grep "writeF\|append" $1 | wc -l) 
echo "N. di write: " $cnt 


grep "writeF\|append" $1 > $tmpfile 
sum=0
exec 3<$tmpfile    # apro il file in lettura e gli assegno il descrittore 3 
while IFS=" " read -u 3 line; do #leggo riga per riga
    elem=$(echo $line| cut -d" " -f 3) #prendo il terzo campo
  	sum=$(echo "$sum+$elem" | bc -lq)  
done
exec 3<&-  

if [[ "$cnt" -ne 0 ]]; then 
	avg=$(echo "$sum/$cnt" | bc -lq)
else
	avg=0
fi
echo "$avg"| LC_ALL="C" awk '{printf "Size media delle scritture in bytes:  %.2f \n", $1}'

cnt=$(grep '^lock' $1 | wc -l) #^ mi dice che lock deve trovarsi esattamente all'inizio della linea (fondamentale per non contare anche gli unlock come lock)
echo "N. di operazioni di lock: " $cnt  


cnt=$(grep '^open_l' $1 | wc -l)
echo "N. di operazioni di open-lock: " $cnt  


cnt=$(grep '^unlock' $1 | wc -l)
echo "N. di operazioni di unlock: " $cnt  


cnt=$(grep '^close ' $1 | wc -l)
echo "N. di operazioni di close: " $cnt  


cat $1 | grep "writeF\|append\|remove\|rimpiazzamento" | cut -d" " -f 1,3,4 > $tmpfile
sum=0
cnt=0
cnt_max=0
exec 3<$tmpfile    # apro il file in lettura e gli assegno il descrittore 3 
while IFS=" " read -u 3 line; do #leggo riga per riga
	op=$(echo $line | tr -s " " | cut -d" " -f 1) #elimino spazi duplicati e prendo la prima colonna
	nBW=$(echo $line | tr -s " " | cut -d" " -f 2)
	nBR=$(echo $line | tr -s " " | cut -d" " -f 3)
	if [[ "$op" = "writeF" || "$op" = "append" ]]; then 
		(( cnt += nBW))
		if [[ "$cnt" -ge "$cnt_max" ]]; then 
			cnt_max=$cnt
		fi
	elif [[ "$op" = "remove" || "$op" = "rimpiazzamento" ]];then
		(( cnt -= nBR))
	fi
done
exec 3<&-  
cnt_max=$(echo "$cnt_max/1000" | bc -lq)
echo "$cnt_max"| LC_ALL="C" awk '{printf "Dimensione massima in Mbytes raggiunta dallo storage:  %.3f \n", $1}'


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
exec 3<&- 
echo "Dimensione massima in numero di file raggiunta dallo storage " $cnt_max


cnt=$(grep '^rimpiazzamento' $1 | wc -l)
echo "Numero volte in cui è stato usato l'algoritmo di rimpiazzamento: " $cnt  

#tolgo intestazione, righe di rimpiazzamento, prendo la colonna thread, la ordino, e unisco le righe uguali mettendo loro come prefisso il numero di occorrenze
cat $1 | tail -n +3 $1 | grep -v "rimpiazzamento" | cut -d" " -f 5  | sort -g | uniq -c > $tmpfile #conto anche le richieste di connessione/disconnessione
exec 3<$tmpfile    # apro il file in lettura e gli assegno il descrittore 3 
while IFS=" " read -u 3 line; do #leggo riga per riga
	thread=$(echo $line| cut -d" " -f 2) #tolgo spazi duplicati e prendo il terzo campo
	n_req=$(echo $line| cut -d" " -f 1) 
	echo "Il thread $thread ha servito $n_req richieste"
done
exec 3<&-  


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
exec 3<&- 
echo "Numero massimo di connessioni contemporanee: " $cnt_max

rm $tmpfile 


