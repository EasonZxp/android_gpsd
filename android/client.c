#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#if 0
#define UDP
#else
#define TCP
#endif

int sockfd;
char *IP = "127.0.0.1";
#ifdef UDP
short PORT = 1025;
#endif
#ifdef TCP
short PORT = 7838;
#endif
typedef struct sockaddr SA;

void init(){
#ifdef UDP
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
#endif

#ifdef TCP
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = inet_addr(IP);
    if(connect(sockfd, (SA*)&addr, sizeof(addr)) == -1){
        printf("connect failed!\n");
        exit(-1);
    }
#endif
}

int main (int argc, char *argv[]) {
    //char buf[2048];
    char buf[512];
    int index, i = 0;

    if (argc == 2) {
        index = atoi(argv[1]);
    } else {
        printf("Usage: ./client <times>\n");
        return -1;
    }

    printf("start init ....\n");
    init();
    printf("connected...\n");
    memset(buf, 'H', sizeof(buf));
#ifdef UDP
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = inet_addr(IP);
#endif

    while (1) {
        //printf("please input something:\n");
        //scanf("%s", buf);
        //puts(buf);
        if (i == index)
            break;
#ifdef TCP
        send(sockfd, "hello", 5, 0);
        printf("send 2048! index:%d\n", i);
#endif
#ifdef UDP
        printf("sendto 2048! index:%d\n", i);
        if (sendto(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&addr, sizeof(struct sockaddr)) == -1){
            printf("sendto error!\n");
            return -1;
        }
#endif
        //sleep(1);
        i++;
    }

    close(sockfd);

    return 0;
}
