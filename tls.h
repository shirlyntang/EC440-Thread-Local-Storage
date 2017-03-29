#ifndef TLS
#define TLS

int tls_create(unsigned int size);

int tls_write(unsigned int offset, unsigned int length, char *buffer);

int tls_read(unsigned int offset, unsigned int length, char *buffer);

int tls_destroy();

int tls_clone(pthread_t tid);

int get_address();

void print_tls();

#endif