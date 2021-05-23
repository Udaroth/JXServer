#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>

#define SERVER_MSG ("Hello from server!\n")
#define MAX_FILENAME_LEN 1024

struct connection_data {

    int socketfd;
    char* msg;
    size_t msg_len;

};

void* connection_handler(void* arg){
    struct connection_data* data = (struct connection_data*)arg;
    char buffer[1024];
    memset(buffer, 0 ,1024);
    read(data->socketfd, buffer, 1024);
    write(data->socketfd, data->msg, data->msg_len);
    close(data->socketfd);
    free(data);
    return NULL;
}

int main_example(int argc, char** argv){
   

    // Declare sockets and initialise them to an error value
    int serversocket_fd = -1;
    int clientsocket_fd = -1;

    int port = 8000;
    struct sockaddr_in address;
    int option = 1;

    // char buffer[1024];
    // Initialise the socket 
    serversocket_fd = socket(AF_INET, SOCK_STREAM, 0);

    // Check initialisation
    if(serversocket_fd < 0){
        puts("Socket initialisatino failed");
        exit(1);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Sets the value of reuse address and reuse port options to 1
    setsockopt(serversocket_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &option, sizeof(int));

    if(bind(serversocket_fd, (struct sockaddr*)&address, sizeof(struct sockaddr_in))) {
        puts("Binding to address failed");
        exit(1);
    }

    // Have the socket begin listening on the serversocket, with up to 128 connections
    listen(serversocket_fd, 128);
    // Put into while loop so that the server continues to listen for requests
    while(1){
        // Set the length of the address
        uint32_t addrlen = sizeof(struct sockaddr_in);
        // Accept the connection
        clientsocket_fd = accept(serversocket_fd, (struct sockaddr*)&address, &addrlen);

        // Create the connection_data structre to store all the arguments required for threading the process
        struct connection_data* d = (struct connection_data*)malloc(sizeof(struct connection_data));
        // set the connection socket to the where the socket that we've accepted the connection
        d->socketfd = clientsocket_fd;
        d->msg = SERVER_MSG;
        d->msg_len = strlen(SERVER_MSG)+1;
        pthread_t thread;
        pthread_create(&thread, NULL, connection_handler, d);


    }


    close(serversocket_fd);
    return 0;
    



}