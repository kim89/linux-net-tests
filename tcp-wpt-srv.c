
#include <ev.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <err.h>

#include <string.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>


#define     MAX_BUF_LEN     256


char* tmp_buf;
//int f_buf_ready;
int f_buf_received;
pthread_mutex_t mutex;

typedef struct __pkt_t_ {
    char *buffer;         
    volatile int rcv;     
    volatile int offset;  
    size_t buf_len;       
} tcp_buf;


int listen_on_socket(struct sockaddr_in *listen_addr, int mode) {

    int listen_fd;
    int on = 1;

    if (mode)
        listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    else
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (listen_fd < 0) return -1;

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(listen_fd, IPPROTO_TCP, TCP_QUICKACK, &on, sizeof(on));
    setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    if (bind(listen_fd, (const struct sockaddr *)listen_addr, sizeof(struct sockaddr_in)) < 0)
        return -1;

    if (listen(listen_fd, 32)) return -1;

    return listen_fd;
}


int read_data(int fd, tcp_buf *pkt) {
    int rc = -1;
    volatile int rcv;

    if (pkt == NULL || pkt->buffer == NULL || fd <= 0) return rc;

    while (pkt->offset < (int)pkt->buf_len) {
        rcv = recv(fd, pkt->buffer + pkt->offset, pkt->buf_len - pkt->offset, 0);

        if (rcv < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                rc = 0;
            else
                rc = -1;

            break;
        }

        if (rcv == 0) {
            rc = -1;
            break;
        }

        pkt->rcv += rcv;
        pkt->offset += rcv;
        rc = 1;
    }

    return rc;
}


int send_data(int fd, tcp_buf *pkt) {
    int rc = -1;
    volatile int snt = 0;

    if (pkt == NULL || pkt->buffer == NULL || fd <= 0) return rc;

    while (pkt->rcv) {
        snt = send(fd, pkt->buffer + (pkt->offset - pkt->rcv), pkt->rcv, 0);

        if (snt <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                rc = 0;
            else
                rc = -1;

            break;
        }

        pkt->rcv -= snt;
    }

    if (pkt->rcv == 0) {
        pkt->offset = 0;
        rc = 1;
    }

    return rc;
}


void on_read(struct ev_loop *loop, ev_io *w, int revents) {
    int rc, fd = w->fd;

    tcp_buf *buf = (tcp_buf *)w->data;

    if (revents & EV_ERROR) {
        fprintf(stderr, "[%d] Internal error.\n", fd);
        close_connection(fd, loop, w);
        return;
    }

    if (revents & EV_READ && buf->offset < (int)buf->buf_len) {
        rc = read_data(fd, buf);
        
        // wait handler in main thread
        while (f_buf_received == 1) continue;
        tmp_buf = buf->buffer;
        f_buf_received = 1;

        while((pthread_mutex_trylock(&mutex)) == EBUSY) continue;
        pthread_mutex_unlock(&mutex);

        if (rc >= 0) {
            fprintf(stderr, "[%d] RECEIVED: %.*s\n", fd, buf->rcv, buf->buffer);
            rc = send_data(fd, buf);

            memset(buf->buffer, '\0', buf->buf_len);           

            if (rc == 0) ev_io_set(w, fd, EV_WRITE | EV_READ);
        }

        if (rc < 0) {
            close_connection(fd, loop, w);
            return;
        }
    }

    if (revents & EV_WRITE && buf->rcv > 0) {
        rc = send_data(fd, buf);

        if (rc == 1) ev_io_set(w, fd, EV_READ);

        if (rc < 0) {
            close_connection(fd, loop, w);
            return;
        }
    }
}


void close_connection(int fd, struct ev_loop *loop, ev_io *io){
    
    tcp_buf *buf = (tcp_buf *)io->data;

    printf("\n ===> Connection closed!\n");
    ev_io_stop(loop, io);
    close(fd);
    free(io);
    free(buf->buffer);
    free(buf);

    pthread_mutex_destroy(&mutex);
    exit(0);
}


void accept_connection(struct ev_loop *loop, ev_io *w, __attribute__((unused)) int revents){

    ev_io *io = NULL;
    struct sockaddr_in sa;
    socklen_t sa_len = sizeof(sa);
    int fd;
    tcp_buf *buf = NULL;

    if ((io = calloc(1, sizeof(struct ev_io))) == NULL) err(EXIT_FAILURE, "calloc() ");

    if ((fd = accept(w->fd, (struct sockaddr *)&sa, &sa_len)) <= 0) return;

    fcntl(fd, F_SETFL, O_NONBLOCK);

    printf("\n =====> Accept new connection from %s:%d\n", inet_ntoa(sa.sin_addr),
           ntohs(sa.sin_port));

    if ((buf = calloc(1, sizeof(tcp_buf))) == NULL) err(EXIT_FAILURE, "calloc()");
    if ((buf->buffer = calloc(MAX_BUF_LEN, sizeof(char))) == NULL) err(EXIT_FAILURE, "calloc()");
    buf->buf_len = MAX_BUF_LEN;

    ev_io_init(io, on_read, fd, EV_READ);
    io->data = buf;
    ev_io_start(loop, io);

}



void *connection_handler(void *_arg){
    
    int port_number = (int) _arg;
    printf("\n ===> trying to create socket with port number: %d\n\n", port_number);

    if ((port_number < 49152) || (port_number > 65535)){
        printf("\n Please choose correct userspace portnumber in range: 49152 - 65535\n\n");
        exit(-1);
    }

    struct sockaddr_in sock = {0};
    struct ev_loop *loop = NULL;
    ev_io sock_watcher;
    int s_fd;
    uint16_t port;

    port = port_number;

    sock.sin_port = htons(port);
    sock.sin_addr.s_addr = htonl(INADDR_ANY);
    sock.sin_family = AF_INET;

    if ((s_fd = listen_on_socket(&sock, 1)) == -1){
        printf("\n !===> listen_on_socket failed!\n");
        exit(-1);
    }
    else
        printf("\n ===> server listen on port: %d\n", port);

    if ((loop = ev_loop_new(EVFLAG_NOENV | EVBACKEND_EPOLL)) == NULL) {
        close(s_fd);
        printf("\n !===> Failed with creating main event loop!\n");
        exit(-1);
    }

    ev_io_init(&sock_watcher, accept_connection, s_fd, EV_READ);
    ev_io_start(loop, &sock_watcher);

    ev_run(loop, 0);

}


int main(int argc, char* argv[]){

    if (argc != 2){
        printf("\n Usage: tcp-wpt-srv [port number]\n\n");
        exit(-1);
    }

    int port_number = atoi(argv[1]);
    f_buf_received = 0;

    pthread_mutex_init(&mutex, NULL);
    pthread_t thread_conn;
    if((pthread_create(&thread_conn, NULL, connection_handler, (void *)port_number)) != 0){
        printf(" !===> Failed to create conn handler thread!\n");
        pthread_mutex_destroy(&mutex);
        exit(0);
    }


    while(-1){
        if(f_buf_received == 0) continue;

        pthread_mutex_lock(&mutex);
        int tmp_buf_len = strlen(tmp_buf);
        for(int i = 0, j = tmp_buf_len - 1; i < j; ++i, --j){
            char tmp_char = tmp_buf[i];
            tmp_buf[i] = tmp_buf[j];
            tmp_buf[j] = tmp_char;
        }
        f_buf_received = 0;
        pthread_mutex_unlock(&mutex);

    }

    exit(0);
}
