#ifndef LIST_H
#define LIST_H

/** Nodo della lista di open_owners.
 *
 */
typedef struct open_node{  
    int fd;
    struct open_node *next;
} open_node;

/** Inserisce un nodo in testa alla lista se non duplicato.
 *   \param puntatore alla lista su cui inserire il nodo 
 *   \param valore del nodo da inserire
 *  
 *   \retval 0 se successo
 */
int insert_head(open_node **list, int info);

/** Rimuove un nodo dalla lista.
 *   \param puntatore alla lista da cui rimuovere il nodo
 *   \param valore del nodo da rimuovere
 *
 *   \retval 0 se successo
 */
int remove_elm(open_node **list, int info);

/** Cerca un nodo nella lista.
 *   \param puntatore alla lista da cui cercare il nodo
 *   \param valore del nodo da cercare
 *
 *  \retval 0 se successo
 *  \retval -1 se errore
 */
int find_elm(open_node *list, int elm);

/** Stampa il contenuto della lista 
 *   \param puntatore alla lista da stampare
 *
 */
void print_list(open_node *list);

#endif /* LIST_H */