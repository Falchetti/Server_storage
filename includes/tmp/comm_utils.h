#if !defined(_COMM_UTILS_H)
#define _COMM_UTILS_H

static inline int readn(long fd, void *buf, size_t size) {
static inline int writen(long fd, void *buf, size_t size) {


int msg_sender(char *msg, char *cmd, const char *path, int size, char *cnt);
int file_receiver(const char *dirname, int fd);
int comunic_cs(char *cmd, const char *pathname, int *err);
int error_handler(char *msg, int sz);

int file_sender(file_repl *list, int fd);
int reply_sender(char *msg, int fd);