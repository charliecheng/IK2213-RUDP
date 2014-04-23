#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include "event.h"
#include "rudp.h"
#include "rudp_api.h"
#define SENT 1
#define NOTSENT 0
#define ACKED 2
#define udp_lost 0
//structs declaration

struct rudppacket {
    struct rudp_hdr header;
    char data[RUDP_MAXPKTSIZE];
} __attribute__((packed));
typedef struct rudppacket rudp_packet;

struct packet_node {
    struct sockaddr_in to;
    int is_FIN_ACK;
    int state;
    int retries;
    int data_len;
    int TimeoutDel;
    rudp_packet packet;
    struct packet_node *next;
};
typedef struct packet_node packet_queue_node;

struct sendernode {
    u_int32_t last_seq;
    u_int32_t FIN_seq;
    struct sockaddr_in to;
    int SYN_ACK; //1 stands for ack of syn received
    struct sendernode *next;
};
typedef struct sendernode sender_list_node;

struct receivernode {
    u_int32_t last_seq;
    u_int32_t SYN_seq;
    u_int32_t FIN_seq;
    struct sockaddr_in to;
    int data_seq;
    int SYN_ACK;
    int FIN_ACK;
    packet_queue_node *bufferd_packet;
    packet_queue_node *SYN_packet;
    packet_queue_node *last_sent_packet;
    struct receivernode *next;
};
typedef struct receivernode receiver_list_node;

struct rudp_socket_node {
    int sockfd;
    int port;
    int (*socket_recvfrom_handler)(rudp_socket_t, struct sockaddr_in *, char *, int);
    int (*socket_event_handler)(rudp_socket_t, rudp_event_t, struct sockaddr_in *);
    struct sockaddr_in socket_addr;
    sender_list_node *senders;
    receiver_list_node *receivers;
    struct rudp_socket_node *next;
};
typedef struct rudp_socket_node socket_list_node;

//linked list of rudp sockets
socket_list_node *sock_list = NULL;

//Functions Declaration
socket_list_node * add_to_socket_list(socket_list_node *node);
socket_list_node *search_socket(socket_list_node *r_socket);
sender_list_node *add_sender(socket_list_node *r_socket, struct sockaddr_in addr);
sender_list_node *search_sender(socket_list_node *r_socket, struct sockaddr_in addr);
receiver_list_node *add_receiver(socket_list_node *r_socket, struct sockaddr_in addr);
receiver_list_node *search_receiver(socket_list_node *r_socket, struct sockaddr_in addr);
packet_queue_node *add_packet_to_queue(receiver_list_node * receiver, int data_len, rudp_packet rudppacket, struct sockaddr_in to);
packet_queue_node *search_packet(receiver_list_node * receiver, u_int32_t seq);

int send_packet(socket_list_node * r_socket, packet_queue_node *pk, struct sockaddr_in to);
int retransmit_packet(int fd, void *arg);
int rudp_receive_packet(int fd, void *arg);

/* 
 * rudp_socket: Create a RUDP socket. 
 * May use a random port by setting port to zero. 
 */

rudp_socket_t rudp_socket(int port) {
    struct rudp_socket_node *rudp_socket = malloc(sizeof (struct rudp_socket_node));
    int socket_fd;
    struct sockaddr_in addr;
    socklen_t sin_len;
    int err = 0;
    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        fprintf(stderr, "Failed to new an UDP socket in rudp_socket\n");
        return NULL;
    }
    bzero(&addr, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    sin_len = sizeof (struct sockaddr_in);
    if (bind(socket_fd, (struct sockaddr *) &addr, sizeof (addr)) == -1) {
        fprintf(stderr, "Failed to bind address in rudp_socket\n");
        return NULL;
    }
    err = getsockname(socket_fd, (struct sockaddr *) &addr, &sin_len);
    if (err == -1) {
        fprintf(stderr, "getsockname() failed");
        return NULL;
    }
    rudp_socket->port = ntohs(addr.sin_port);
    rudp_socket->sockfd = socket_fd;
    rudp_socket->socket_addr = addr;
    rudp_socket = add_to_socket_list(rudp_socket); ////////////////////////
    printf("Create socket: sockfd: %d, port: %d\n", rudp_socket->sockfd, rudp_socket->port);
    if (event_fd((int) socket_fd, &rudp_receive_packet, (void*) rudp_socket, "rudp_receive_packet") < 0) {
        printf("failed to register rudp_receive_data()!\n");
        return NULL;
    }
    return rudp_socket;
}

/* 
 *rudp_close: Close socket 
 */

int rudp_close(rudp_socket_t rsocket) {

    socket_list_node *socket = search_socket(rsocket);
    rudp_packet pk;
    memset(&pk, 0x0, sizeof (rudp_packet));
    if (socket == NULL) {
        return -1;
    }
    pk.header.version = RUDP_VERSION;
    pk.header.type = RUDP_FIN;
    pk.header.seqno = 0;


    receiver_list_node *receiver = socket->receivers;
    while (receiver != NULL) {
        //Add FIN packet to PacketQueue
        pk.header.seqno = receiver->data_seq + 1;
        receiver->FIN_seq = pk.header.seqno;
        add_packet_to_queue(receiver, 0, pk, receiver->to);
        receiver = receiver->next;
    }

    return 0;
}

/* 
 *rudp_recvfrom_handler: Register receive callback function 
 */

int rudp_recvfrom_handler(rudp_socket_t rsocket, int (*handler)(rudp_socket_t, struct sockaddr_in *, char *, int)) {
    socket_list_node *socket;
    socket = (socket_list_node *) rsocket;
    socket->socket_recvfrom_handler = handler;
    return 0;
}

/* 
 *rudp_event_handler: Register event handler callback function 
 */
int rudp_event_handler(rudp_socket_t rsocket, int (*handler)(rudp_socket_t, rudp_event_t, struct sockaddr_in *)) {
    socket_list_node *socket;
    socket = (socket_list_node *) rsocket;
    socket->socket_event_handler = handler;
    return 0;
}

/* 
 * rudp_sendto: Send a block of data to the receiver. 
 */

int rudp_sendto(rudp_socket_t rsocket, void* data, int len, struct sockaddr_in* to) {
    if (len > RUDP_MAXPKTSIZE) {
        printf("Data length is more than RUDP_MAXPKTSIZE!\n");
        return -1;
    }
    struct sockaddr_in addr = *to;
    socket_list_node *socket = search_socket(rsocket);
    receiver_list_node *receiver = search_receiver(socket, addr);
    if (receiver == NULL) {
        receiver = add_receiver(socket, addr);
    }
    rudp_packet packet;
    memset(&packet, 0x0, sizeof (rudp_packet));
    if (receiver->SYN_seq == 0) {
        packet.header.type = RUDP_SYN;
        packet.header.version = RUDP_VERSION;
        packet.header.seqno = rand() % 0xFFFFFFFF + 1;
        if (packet.header.seqno == 0) {
            packet.header.seqno++;
        }
        memcpy(&(receiver->to), &addr, sizeof (struct sockaddr_in));

        //update SYN_seqno 
        receiver->SYN_seq = packet.header.seqno;
        receiver->data_seq = packet.header.seqno;
        packet_queue_node *synpacket = add_packet_to_queue(receiver, 0, packet, addr);
        receiver->last_sent_packet = receiver->bufferd_packet;
        if (send_packet(socket, synpacket, addr) < 0) {
            fprintf(stderr, "Failed to send SYN packet!\n");
            return -1;
        }
    }
    packet.header.type = RUDP_DATA;
    packet.header.version = RUDP_VERSION;
    packet.header.seqno = receiver->data_seq + 1;
    receiver->data_seq++;
    memcpy(&(packet.data), data, len);
    add_packet_to_queue(receiver, len, packet, addr);






    return 0;
}

socket_list_node * add_to_socket_list(socket_list_node *node) {
    socket_list_node *temp = NULL;
    if (sock_list == NULL) {
        sock_list = malloc(sizeof (socket_list_node));
        temp = sock_list;
    } else {
        temp = sock_list;
        while (temp->next != NULL) {
            temp = temp->next;
        }
        temp->next = malloc(sizeof (socket_list_node));
        temp = temp->next;
    }
    temp->sockfd = node->sockfd;
    temp->port = node->port;
    temp->socket_addr = node->socket_addr;
    temp->receivers = NULL;
    temp->senders = NULL;
    temp->socket_recvfrom_handler = node->socket_recvfrom_handler;
    temp->socket_event_handler = node->socket_event_handler;
    temp->next = NULL;
    return temp;
}

socket_list_node *search_socket(socket_list_node *r_socket) {
    socket_list_node *temp = sock_list;
    while (temp != NULL) {
        if (temp->sockfd == r_socket->sockfd && temp->port == r_socket->port) {
            return temp;
        }
        temp = temp->next;
    }
    return NULL;
}

sender_list_node *add_sender(socket_list_node *r_socket, struct sockaddr_in addr) {
    socket_list_node *temp_socket_list = search_socket(r_socket);
    if (temp_socket_list == NULL) {
        return NULL;
    }
    sender_list_node *temp_sender;
    if (temp_socket_list->senders == NULL) {
        temp_socket_list->senders = malloc(sizeof (sender_list_node));
        temp_socket_list->senders->next = NULL;
        temp_sender = temp_socket_list->senders;
    } else {
        temp_sender = temp_socket_list->senders;
        while (temp_sender != NULL) {
            if (temp_sender->next == NULL) {
                break;
            }
            temp_sender = temp_sender->next;
        }
        temp_sender->next = malloc(sizeof (sender_list_node));
        temp_sender = temp_sender->next;
    }
    temp_sender->to = addr;
    temp_sender->next = NULL;
    temp_sender->SYN_ACK = 0;
    temp_sender->FIN_seq = 0;
    temp_sender->last_seq = 0;
    return temp_sender;
}

sender_list_node *search_sender(socket_list_node *r_socket, struct sockaddr_in addr) {
    sender_list_node *temp_sender = r_socket->senders;
    while (temp_sender != NULL) {
        if (ntohs(temp_sender->to.sin_port) == ntohs(addr.sin_port) && !strcmp(inet_ntoa(temp_sender->to.sin_addr), inet_ntoa(addr.sin_addr))) {
            return temp_sender;
        }
        temp_sender = temp_sender->next;
    }
    return NULL;
}

receiver_list_node *add_receiver(socket_list_node *r_socket, struct sockaddr_in addr) {
    socket_list_node *temp_socket_list = search_socket(r_socket);
    if (temp_socket_list == NULL) {
        return NULL;
    }
    receiver_list_node *temp_receiver;
    if (temp_socket_list->receivers == NULL) {
        temp_socket_list->receivers = malloc(sizeof (receiver_list_node));
        temp_socket_list->receivers->next = NULL;
        temp_receiver = temp_socket_list->receivers;
    } else {
        temp_receiver = temp_socket_list->receivers;
        while (temp_receiver != NULL) {
            if (temp_receiver->next == NULL) {
                break;
            }
            temp_receiver = temp_receiver->next;
        }
        temp_receiver->next = malloc(sizeof (receiver_list_node));
        temp_receiver = temp_receiver->next;
    }
    temp_receiver->to = addr;
    temp_receiver->next = NULL;
    temp_receiver->SYN_seq = 0;
    temp_receiver->SYN_ACK = 0;
    temp_receiver->data_seq = 0;
    temp_receiver->FIN_seq = 0;
    temp_receiver->FIN_ACK = 0;
    temp_receiver->last_seq = 0;
    temp_receiver->SYN_packet = NULL;
    temp_receiver->last_sent_packet = NULL;
    temp_receiver->bufferd_packet = NULL;
    return temp_receiver;
}

receiver_list_node *search_receiver(socket_list_node *r_socket, struct sockaddr_in addr) {
    receiver_list_node *temp_receiver = r_socket->receivers;
    while (temp_receiver != NULL) {
        if (ntohs(temp_receiver->to.sin_port) == ntohs(addr.sin_port) && !strcmp(inet_ntoa(temp_receiver->to.sin_addr), inet_ntoa(addr.sin_addr))) {
            return temp_receiver;
        }
        temp_receiver = temp_receiver->next;
    }
    return NULL;
}
//static int ii=0;

packet_queue_node *add_packet_to_queue(receiver_list_node * receiver, int data_len, rudp_packet rudppacket, struct sockaddr_in to) {
    receiver_list_node *temp_receiver = receiver;
    if (temp_receiver == NULL) {
        return NULL;
    }
    packet_queue_node *temp_packet_node;
    if (temp_receiver->bufferd_packet == NULL) {
        temp_receiver->bufferd_packet = malloc(sizeof (packet_queue_node));
        temp_receiver->bufferd_packet->next = NULL;
        temp_receiver->last_sent_packet = temp_receiver->bufferd_packet;
        temp_packet_node = temp_receiver->bufferd_packet;
    } else {
        temp_packet_node = temp_receiver->bufferd_packet;
        while (temp_packet_node != NULL) {
            if (temp_packet_node->next == NULL) {
                break;
            }
            temp_packet_node = temp_packet_node->next;
        }
        temp_packet_node->next = malloc(sizeof (packet_queue_node));
        temp_packet_node = temp_packet_node->next;
    }
    temp_packet_node->to = to;
    temp_packet_node->retries = 0;
    temp_packet_node->state = 0;
    temp_packet_node->data_len = data_len;
    temp_packet_node->packet = rudppacket;
    temp_packet_node->TimeoutDel = 0;
    temp_packet_node->next = NULL;
    temp_packet_node->is_FIN_ACK = 0;
    //printf("packet type: %d    packet seq %d\n",temp_packet_node->packet.header.type,temp_packet_node->packet.header.seqno);
    // for ( ii=0;ii<temp_packet_node->data_len+1;ii++){
    // printf("%d",temp_packet_node->packet.data[ii]);}
    //  printf("\n");
    return temp_packet_node;
}

packet_queue_node *search_packet(receiver_list_node * receiver, u_int32_t seq) {
    packet_queue_node *temp_packet = receiver->bufferd_packet;
    while (temp_packet != NULL) {
        if (receiver->SYN_seq == seq) {
            return temp_packet;
        }
        if (temp_packet->packet.header.seqno == seq) {
            return temp_packet;
        }
        if (temp_packet->next == NULL) {
            return NULL;
        }
        temp_packet = temp_packet->next;
    }
    return NULL;
}

int send_packet(socket_list_node * r_socket, packet_queue_node *pk, struct sockaddr_in to) {
    struct timeval timer, t0, t1; // timer variables
    timer.tv_sec = 0;
    timer.tv_usec = 0;
    t0.tv_sec = 0;
    t0.tv_usec = 0;
    t1.tv_sec = 0;
    t1.tv_usec = 0;
    int len = pk->data_len;
    rudp_packet * packet = (rudp_packet *) malloc(sizeof (rudp_packet));
    packet->header = pk->packet.header;
    memcpy(packet->data, pk->packet.data, len);
    char * buffer = malloc(len * sizeof (char) + sizeof (struct rudp_hdr));
    memcpy(buffer, &packet->header, sizeof (struct rudp_hdr));
    memcpy(buffer + sizeof (struct rudp_hdr), pk->packet.data, len);


    // Start the timeout callback with event_timeout
    timer.tv_sec = RUDP_TIMEOUT / 1000; // convert to second
    timer.tv_usec = (RUDP_TIMEOUT % 1000) * 1000; // convert to micro
    gettimeofday(&t0, NULL); // current time of the day
    timeradd(&t0, &timer, &t1); //add the timeout time with the current time of the day

    // register timeout

    if (event_timeout(t1, &retransmit_packet, (void*) pk, "timer_callback") == -1) {
        fprintf(stderr, "Error registering event_timeout in send_packet function\n");
        return -1;
    }


    //printf("RUDP:Sending packet with seqno=%0x - ", packet->header.seqno);
    /*
    if (DROP != 0 && rand() % DROP == 1) {
        printf("packet dropped!\n");
        return 1;
    }*/
    printf("Sending packet! %0x\n", pk->packet.header.seqno);
    if (sendto((int) r_socket->sockfd, buffer, len + sizeof (struct rudp_hdr), 0, (struct sockaddr *) &to, sizeof (struct sockaddr)) <= 0) {
        fprintf(stderr, "Failed to send packet in send_packet function\n");
        return -1;
    }
    return 1;
}

int retransmit_packet(int fd, void *arg) {
    event_timeout_delete(retransmit_packet, arg);
    packet_queue_node *pk = (packet_queue_node*) arg;
    struct sockaddr_in to = pk->to;
    socket_list_node *temp_socket = sock_list;
    receiver_list_node *temp_receiver;
    packet_queue_node *temp_packet;
    int found = 0;
    while (temp_socket != NULL) {
        temp_receiver = temp_socket->receivers;
        while (temp_receiver != NULL) {
            temp_packet = temp_receiver->bufferd_packet;
            while (temp_packet != NULL) {
                if (temp_packet ->packet.header.seqno == pk->packet.header.seqno && ntohs(temp_packet ->to.sin_port) == ntohs(pk->to.sin_port) && !strcmp(inet_ntoa(temp_packet ->to.sin_addr), inet_ntoa(pk->to.sin_addr))) {
                    found = 1;
                    break;
                }
                temp_packet = temp_packet ->next;
            }
            if (found) break;
            temp_receiver = temp_receiver->next;
        }
        if (found) break;
        temp_socket = temp_socket->next;
    }
    if (temp_packet->retries < RUDP_MAXRETRANS) {
        printf("retransmission!\n");
        if (send_packet(temp_socket, temp_packet, temp_packet->to) < 0) {
            fprintf(stderr, "Failed to re send packet in retransmit_packet function\n");
            return -1;
        }
        temp_packet->retries++;
    } else {//check
        event_timeout_delete(retransmit_packet, arg);
        temp_receiver->FIN_ACK = 1; //fake solution
        receiver_list_node* treceiver = temp_socket->receivers;
        int allFIN = 1;
        while (treceiver != NULL) {
            if (treceiver->FIN_ACK == 0)
                allFIN = 0;
            treceiver = treceiver->next;
        }
        if (allFIN) {
            temp_socket->socket_event_handler((rudp_socket_t) temp_socket, RUDP_EVENT_CLOSED, NULL);
            event_fd_delete(rudp_receive_packet, (void *) temp_socket);
        }

        temp_socket->socket_event_handler((rudp_socket_t) temp_socket, RUDP_EVENT_TIMEOUT, &to);

    }
    return 1;
}

int rudp_receive_packet(int fd, void *arg) {

    socket_list_node *socket = search_socket((socket_list_node *) arg);
    if (socket == NULL) {
        return -1;
    }
    struct sockaddr_in addr; //for the receiver sider here it is the address of the sender
    int addr_size = sizeof (addr);
    int bytes = 0;
    rudp_packet packet;
    memset(&packet, 0x0, sizeof (rudp_packet));
    bytes = recvfrom((int) fd, (void*) &packet, sizeof (packet), 0, (struct sockaddr*) &addr, (socklen_t*) & addr_size);
    if (bytes <= 0) {
        fprintf(stderr, "recvfrom failed in rudp_receive_packet function\n");
        return -1;
    }
    int data_length = bytes - sizeof (struct rudp_hdr);
    if (packet.header.version != RUDP_VERSION) {
        printf("Invalid RUDP version of received packet in rudp_receive_packet\n");
        return -1;
    }
    sender_list_node *sender;
    receiver_list_node *receiver;
    packet_queue_node *temp_packet;
    receiver_list_node *temp_receiver;
    int number_of_unacked;
    double random_number;
    static double x = 0;
    switch (packet.header.type) {
            //When the receiver application socket receives an SYN:
        case RUDP_SYN:
            sender = search_sender(socket, addr);
            if (sender != NULL) {
                break;
            }
            sender = add_sender(socket, addr);
            if (sender == NULL) {
                return -1;
            }
            sender->to = addr;
            sender->last_seq = packet.header.seqno;
            packet.header.type = RUDP_ACK;
            packet.header.seqno = packet.header.seqno + 1;
            if (sendto((int) socket->sockfd, (void *) &packet.header, sizeof (struct rudp_hdr), 0, (struct sockaddr*) &addr, sizeof (struct sockaddr_in)) < 0) {
                fprintf(stderr, "Failed to send SYN ACK in rudp_send_packet function\n");
                return -1;
            }
            break;
            //When the receiver application socket receives an FIN:
        case RUDP_FIN:
            sender = search_sender(socket, addr);
            if (packet.header.seqno != sender->last_seq + 1) {
                break;
            }
            sender->last_seq = packet.header.seqno;
            packet.header.type = RUDP_ACK;
            packet.header.seqno = packet.header.seqno + 1;
            printf("Sending FIN ACK of seq %0x\n", packet.header.seqno);
            if (sendto((int) socket->sockfd, (void *) &packet.header, sizeof (struct rudp_hdr), 0, (struct sockaddr*) &addr, sizeof (struct sockaddr_in)) < 0) {
                fprintf(stderr, "Failed to send SYN ACK in rudp_send_packet function\n");
                return -1;
            }
            break;
            //When the receiver application socket receives a data packet:
        case RUDP_DATA:
            //srand(time(NULL));
            printf("The packet with seq of %0x\n", packet.header.seqno);
            if (udp_lost == 1) {
                srand(x);
                random_number = rand() / (double) RAND_MAX;
                x++;
                printf("p=%f\n", random_number);
                if (random_number < 0.2) {
                    printf("The ACK %0x is discard!\n", packet.header.seqno);
                    break;
                }
            }
            sender = search_sender(socket, addr);
            if (packet.header.seqno == (sender->last_seq + 1)) {
                // It is the expected data packet
                printf("sending datagram with seq of %0x\n", packet.header.seqno);
                socket->socket_recvfrom_handler(socket, &addr, packet.data, data_length);
                sender->last_seq = packet.header.seqno;
            }
            packet.header.type = RUDP_ACK;
            packet.header.seqno = sender->last_seq + 1; //update header
            if (sendto((int) socket->sockfd, (void *) &packet.header, sizeof (struct rudp_hdr), 0, (struct sockaddr*) &addr, sizeof (struct sockaddr_in)) < 0) {
                fprintf(stderr, "Failed to send DATA ACK in rudp_send_packet function\n");
                return -1;
            }

            break;
            //When the sending application socket receives an ACK:
        case RUDP_ACK:
            printf("receiving ack!\n");
            receiver = search_receiver(socket, addr);
            if (receiver == NULL) {
                return -1;
            }
            temp_packet = search_packet(receiver, packet.header.seqno - 1);
            if (temp_packet->state == ACKED) {
                //break;
                printf("old acks %0x\n", temp_packet->packet.header.seqno);
                break;
            }
            if (event_timeout_delete(retransmit_packet, temp_packet) == 0) {
                printf("going to delete retransmit for %0x\n", temp_packet->packet.header.seqno);
                //fprintf(stderr, "Failed to delete time out event In rudp_ack\n");
                //return -1;
            }
            temp_packet->state = ACKED;


            temp_packet = receiver->bufferd_packet;



            /*   while (temp_packet->packet.header.seqno != (packet.header.seqno - 1)) {
                   if (temp_packet->state != ACKED) {
                        printf("going to delete retransmit multi\n");
                       if (event_timeout_delete(retransmit_packet, temp_packet) == 0) {
                           printf("going to delete retransmit for %0x\n",temp_packet->packet.header.seqno);
                           //fprintf(stderr, "Error deleting SYNtimeout\n");
                           //return -1;
                       }

                       temp_packet->state = ACKED;
                   }
                   temp_packet = temp_packet->next;
               }*/
            if (packet.header.seqno == (receiver->FIN_seq + 1)) {
                printf("ACK for FIN received\n");
                receiver->FIN_ACK = 1;
                temp_receiver = socket->receivers;
                int allFIN = 1;
                while (temp_receiver != NULL) {
                    if (temp_receiver->FIN_ACK == 0) {
                        printf("==Not all Finished\n");
                        allFIN = 0;
                        break;
                    }
                    temp_receiver = temp_receiver->next;
                }
                if (allFIN == 1) {
                    printf("ALL FIN ACKs received\n");
                    socket->socket_event_handler((rudp_socket_t) socket, RUDP_EVENT_CLOSED, NULL);
                    if (event_fd_delete(rudp_receive_packet, (void *) socket) != 0) {
                        printf("Not Founde\n");
                    }
                }

            } else {
                temp_packet = search_packet(receiver, packet.header.seqno - 1);
                number_of_unacked = 0;
                while (temp_packet->packet.header.seqno != receiver->last_sent_packet->packet.header.seqno) {
                    number_of_unacked++;
                    temp_packet = temp_packet->next;
                }
                temp_packet = receiver->last_sent_packet->next;
                if (temp_packet == NULL) {
                    break;
                }
                while (number_of_unacked <= RUDP_WINDOW && temp_packet != NULL) {
                    send_packet(socket, temp_packet, addr);
                    receiver->last_sent_packet = temp_packet;
                    number_of_unacked++;
                    temp_packet = temp_packet->next;
                }
            }




            break;
        default:
            break;
    }
    return 0;
}