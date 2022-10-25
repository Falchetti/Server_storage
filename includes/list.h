#ifndef LIST_H
#define LIST_H

typedef struct open_node{  //struttura dei nodi nella lista open_owners di un file
    int fd;
    struct open_node *next;
} open_node;

int insert_head(open_node **list, int info);
int remove_elm(open_node **list, int info);
int find_elm(open_node *list, int elm);
void print_list(open_node *list);

#endif /* LIST_H */