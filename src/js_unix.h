#ifndef JS_UNIX_H
#define JS_UNIX_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <math.h>
#include <time.h>


#include <openssl/ssl.h>
#include <openssl/err.h>
#include "list.h"
#include "cutils.h"
#include "quickjs.h"

#endif /* JS_UNIX_H */
