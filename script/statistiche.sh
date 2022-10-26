#!/bin/bash


if [ $# != 1 ]; then
    echo "Deve essere passato 1 file .log come argomento" 
    exit 0
fi

#seleziona righe che contengono readF o readNF 
#dal file passato come argomento e le conta 
cnt=$(grep "readF\|readNF" $1 | wc -l) 
echo "N. di read: " $cnt 

#seleziona righe che contengono readF o readNF 
#dal file passato come argomento e calcola la media dei bytes letti 
tmpfile=./file
grep "readF\|readNF" $1 > $tmpfile 
sum=0
exec 3<$tmpfile    
while IFS=" " read -u 3 line; do 
    elem=$(echo $line| cut -d" " -f 4) #seleziono il quarto campo (bytes letti)
  	sum=$(echo "$sum+$elem" | bc -lq)  
done
exec 3<&-  

if [[ "$cnt" -ne 0 ]]; then 
	avg=$(echo "$sum/$cnt" | bc -lq)
else
	avg=0
fi

echo "$avg"| LC_ALL="C" awk '{printf "Size media delle letture in bytes:  %.2f \n", $1}'


#seleziona righe che contengono writeF o append 
#dal file passato come argomento e le conta
cnt=$(grep "writeF\|append" $1 | wc -l) 
echo "N. di write: " $cnt 

#seleziona righe che contengono writeF o append
#dal file passato come argomento e calcola la media dei bytes scritti 
grep "writeF\|append" $1 > $tmpfile 
sum=0
exec 3<$tmpfile   
while IFS=" " read -u 3 line; do 
    elem=$(echo $line| cut -d" " -f 3) #prendo il terzo campo (bytes scritti)
  	sum=$(echo "$sum+$elem" | bc -lq)  
done
exec 3<&-  

if [[ "$cnt" -ne 0 ]]; then 
	avg=$(echo "$sum/$cnt" | bc -lq)
else
	avg=0
fi
echo "$avg"| LC_ALL="C" awk '{printf "Size media delle scritture in bytes:  %.2f \n", $1}'

#seleziona righe che contengono lock 
#dal file passato come argomento e le conta
cnt=$(grep '^lock' $1 | wc -l) 
echo "N. di operazioni di lock: " $cnt  

#seleziona righe che contengono open_l 
#dal file passato come argomento e le conta
cnt=$(grep '^open_l' $1 | wc -l)
echo "N. di operazioni di open-lock: " $cnt  

#seleziona righe che contengono unlock 
#dal file passato come argomento e le conta
cnt=$(grep '^unlock' $1 | wc -l)
echo "N. di operazioni di unlock: " $cnt  

#seleziona righe che contengono close 
#dal file passato come argomento e le conta
cnt=$(grep '^close ' $1 | wc -l)
echo "N. di operazioni di close: " $cnt  

#seleziona righe che contengono writeF/append/remove/rimpiazzamento
#dal file passato come argomento e i campi "operazione", "bytes letti", "bytes scritti" 
#da questi estrapola la dimensione massima raggiunta dallo storage
cat $1 | grep "writeF\|append\|remove\|rimpiazzamento" | cut -d" " -f 1,3,4 > $tmpfile
sum=0
cnt=0
cnt_max=0
exec 3<$tmpfile
while IFS=" " read -u 3 line; do #leggo riga per riga
	op=$(echo $line | tr -s " " | cut -d" " -f 1) #(tipo di operazione)
	nBW=$(echo $line | tr -s " " | cut -d" " -f 2) #(bytes scritti)
	nBR=$(echo $line | tr -s " " | cut -d" " -f 3) #(bytes letti)
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

#seleziona righe che contengono open_c/open_cl/remove/rimpiazzamento
#dal file passato come argomento e il campo "operazione"
#da questi estrapola il numero di files massimo raggiunto dallo storage
cat $1 | grep "open_c\|open_cl\|remove\|rimpiazzamento" | cut -d" " -f 1 > $tmpfile
sum=0
cnt=0
cnt_max=0
exec 3<$tmpfile    
while IFS=" " read -u 3 line; do 
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

#seleziona righe che contengono rimpiazzamento  
#dal file passato come argomento e le conta
cnt=$(grep '^rimpiazzamento' $1 | wc -l)
echo "Numero volte in cui è stato usato l'algoritmo di rimpiazzamento: " $cnt  

#dopo aver tolto l'intestazione, seleziona righe che non contengono rimpiazzamento e il campo thread. 
#Le ordina fondendo le righe uguali e mettendo loro come prefisso il numero di occorrenze.
#Da queste estrapola il numero di richieste servite da ciascun thread (considerando anche open e close connection)
cat $1 | tail -n +3 $1 | grep -v "rimpiazzamento" | cut -d" " -f 5  | sort -g | uniq -c > $tmpfile 
exec 3<$tmpfile   
while IFS=" " read -u 3 line; do 
	thread=$(echo $line| cut -d" " -f 2)
	n_req=$(echo $line| cut -d" " -f 1) 
	echo "Il thread $thread ha servito $n_req richieste"
done
exec 3<&-  

#seleziona righe che contengono open_connection/close_connection
#dal file passato come argomento e il campo "operazione"
#da questi estrapola il numero massimo di connessioni contemporanee
cat $1 | grep "open_connection\|close_connection" | cut -d" " -f 1 > $tmpfile
sum=0
cnt=0
cnt_max=0
exec 3<$tmpfile     
while IFS=" " read -u 3 line; do 
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


