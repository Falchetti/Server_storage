#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include <list.h>

/**
 * @file list.c
 * @brief File di implementazione dell'interfaccia per la lista 
 */


int insert_head(open_node **list, int info){
	open_node *tmp, *new;
	open_node *aux = *list;
	if(*list == NULL){
		if((new = malloc(sizeof(open_node))) == NULL){
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		new->fd = info;	
		new->next = NULL;
		*list = new; 
	}
    else{
		while(aux != NULL){ //no duplicati
			if(aux->fd == info)
				return 0;
			else 
				aux = aux->next;
		}
		
		if((tmp = malloc(sizeof(open_node))) == NULL){ 
			perror("malloc");
			int errno_copy = errno;
			fprintf(stderr,"FATAL ERROR: malloc\n");
			exit(errno_copy);
		}
		tmp->fd = info;
		tmp->next = *list;
		*list = tmp;
	}
	
	return 0;
}

int remove_elm(open_node **list, int info){
	open_node *curr, *prec;
	
	curr = *list;

	if(curr != NULL){
		if(curr->fd == info){
			*list = (*list)->next;
			free(curr);
		}
		else{
			prec = curr;
			curr = curr->next;
			while(curr != NULL){
				if(curr->fd == info){
					prec->next = curr->next;
					free(curr);
				}
				else{
					prec = curr;
					curr = curr->next;
				}
			}
		}
	}
	
	return 0;
}

int find_elm(open_node *list, int elm){
	
	while(list != NULL){
		if(list->fd == elm)
			return 0;
		else
			list = list->next;
    }
	return -1;
}

void print_list(open_node *list){
	
	if (list == NULL)
		fprintf(stderr, "open_owners: NULL\n");
	else{
		fprintf(stderr, "open_owners: ");
		while(list != NULL){
			fprintf(stderr, "%d, ", list->fd);
			list = list->next;
		}
		fprintf(stderr, "\n");
	}
}
