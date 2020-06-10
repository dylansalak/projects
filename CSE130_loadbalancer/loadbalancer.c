//loadbalancer.c
//Gail Dylan Salak
//1583241
//CSE 130
//Program will act as an intermediary load balancer between a client socket and multiple servers.

#include<err.h>
#include<arpa/inet.h>
#include<netdb.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<sys/time.h>
#include<unistd.h>
#include<stdbool.h>     //true, false
#include <getopt.h>     
#include <pthread.h>    //threads
#include <limits.h>     //INT_MAX stuff
#include <ctype.h>      //C standard lib for characters


#define X 2 //max time b/w healthchecks
#define INTERNAL "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n"

//colors for print statement testing
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KMAG  "\x1B[35m"
#define KCYN  "\x1B[36m"
#define RESET "\033[0m"

//GLOBAL VARIABLES:
int requestCount = 0;


struct server {

    size_t port;              //ID of server 
    int connfd;             //socket descriptor of server port
    size_t totalRequests;   //total requests sent to that server
    size_t totalErrors;
    bool alive;             //boolean to flag if a server is up or down

};

struct worker {

    ssize_t ID;
    int acceptfd;
    size_t optServer;
    pthread_t workerID;
    pthread_cond_t condition;
    pthread_mutex_t* lock;
    pthread_cond_t* dispatchCond;
    pthread_mutex_t* requestLock;
    pthread_cond_t* hcCond;
    size_t portCounter;
    struct server* servers[50];

};

struct healthchecker {

    pthread_t hcID;
    pthread_mutex_t* hcLock;
    pthread_cond_t* hcCond;
    pthread_mutex_t* serverLock;
    pthread_cond_t* optCond;
    size_t portCounter;
    struct server* servers[50];

};

/*
 * client_connect takes a port number and establishes a connection as a client.
 * connectport: port number of server to connect to
 * returns: valid socket if successful, -1 otherwise
 */
int client_connect(uint16_t connectport) {
    int connfd;
    struct sockaddr_in servaddr;

    connfd=socket(AF_INET,SOCK_STREAM,0);
    if (connfd < 0)
        return -1;
    memset(&servaddr, 0, sizeof servaddr);

    servaddr.sin_family=AF_INET;
    servaddr.sin_port=htons(connectport);

    /* For this assignment the IP address can be fixed */
    inet_pton(AF_INET,"127.0.0.1",&(servaddr.sin_addr));

    if(connect(connfd,(struct sockaddr *)&servaddr,sizeof(servaddr)) < 0)
        return -1;
    return connfd;
}

/*
 * server_listen takes a port number and creates a socket to listen on 
 * that port.
 * port: the port number to receive connections
 * returns: valid socket if successful, -1 otherwise
 */
int server_listen(int port) {
    int listenfd;
    int enable = 1;
    struct sockaddr_in servaddr;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0)
        return -1;
    memset(&servaddr, 0, sizeof servaddr);
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
        return -1;
    if (bind(listenfd, (struct sockaddr*) &servaddr, sizeof servaddr) < 0)
        return -1;
    if (listen(listenfd, 500) < 0)
        return -1;
    return listenfd;
}

/*
 * bridge_connections send up to 100 bytes from fromfd to tofd
 * fromfd, tofd: valid sockets
 * returns: number of bytes sent, 0 if connection closed, -1 on error
 */
int bridge_connections(int fromfd, int tofd) {
    char recvline[4096];

    int n = recv(fromfd, recvline, 4096, 0);
    if (n < 0) {
        printf("connection error receiving\n");
        return -1;
    } else if (n == 0) {
        printf("receiving connection ended\n");
        return 0;
    }
    recvline[n] = '\0'; //end message w newline char
        // printf("%s", recvline);
        // sleep(1);
    n = send(tofd, recvline, n, 0);
    if (n < 0) {
        printf("connection error sending\n");
        return -1;
    } else if (n == 0) {
        printf("sending connection ended\n");
        return 0;
    }

    return n;
}

/*
 * bridge_loop forwards all messages between both sockets until the connection
 * is interrupted. It also prints a message if both channels are idle.
 * sockfd1, sockfd2: valid sockets
 */
void bridge_loop(int sockfd1, int sockfd2) {
    fd_set set;
    struct timeval timeout;

    int fromfd, tofd;
    while(1) {
        // set for select usage must be initialized before each select call
        // set manages which file descriptors are being watched
        FD_ZERO (&set);
        FD_SET (sockfd1, &set);
        FD_SET (sockfd2, &set);

        // same for timeout
        // max time waiting, X seconds, 0 microseconds
        timeout.tv_sec = X;
        timeout.tv_usec = 0;

        // select return the number of file descriptors ready for reading in set
        switch (select(FD_SETSIZE, &set, NULL, NULL, &timeout)) {
            case -1:
                printf("error during select, exiting\n");
                return;
            case 0:
                //select timed out
                //send a 500
                dprintf(sockfd1, INTERNAL);
                return;
            default:
                if (FD_ISSET(sockfd1, &set)) {
                    fromfd = sockfd1;
                    tofd = sockfd2;
                } else if (FD_ISSET(sockfd2, &set)) {
                    fromfd = sockfd2;
                    tofd = sockfd1;
                } else {
                    printf("this should be unreachable\n");
                    return;
                }
        }
        if (bridge_connections(fromfd, tofd) <= 0)
            return;
    }
}

/*
    \Function dedicated to healthchecking
    \param *thread - pointer to thread which is executing this function
*/
void* hcProbe(void* thread){
    struct healthchecker* hc = (struct healthchecker*)thread;
    struct timespec timeout;
    struct timeval now;

    while (true){
        printf(KYEL "Healthchecker is ready for a task\n" RESET);

        // for (size_t i = 0; i < hc->portCounter; i++){
        //     printf(KYEL "Port ID %zu\n" RESET, hc->servers[i]->port);
        //     printf(KYEL "Port fd %d\n" RESET, hc->servers[i]->connfd);
        //     printf(KYEL "Port totalRequests %ld\n" RESET, hc->servers[i]->totalRequests);
        // }

        //---CRITICAL SECTION---

        pthread_mutex_lock(hc->hcLock);

        memset(&timeout, 0, sizeof(timeout));

        gettimeofday(&now, NULL);
        timeout.tv_sec = now.tv_sec + X;    

        //healthcheck is either signaled or every X seconds
        pthread_cond_timedwait(hc->hcCond, hc->hcLock, &timeout);
        
        pthread_mutex_unlock(hc->hcLock);

        //---END OF CRITICAL SECTION---

        for (size_t i = 0; i < hc->portCounter; i++){
            int connfd = client_connect(hc->servers[i]->port);

            if(connfd < 0){
                printf(KRED "SERVER NOT CONNECTING\n" RESET);
                hc->servers[i]->alive = false;   //set the server to dead
                continue;        
            } 
            // else {
            //     printf(KGRN "Established connection on port: %zu\n" RESET, hc->servers[i]->port); 
            // }

            uint8_t buffer[4096];
            int ret = dprintf(connfd, "GET /healthcheck HTTP/1.1\r\n\r\n");

            if (ret < 0){
                return 0;
            }

            // sleep(1);

            while (true){
                //DO I NEED TO LOOP THIS READ TO MAYBE GET ALL THE BYTES
                int rBytes = recv(connfd, buffer, sizeof(buffer), 0);
                if (rBytes < 0){
                    hc->servers[i]->alive = false;   //set the server to dead
                    break;
                }
                if (rBytes == 0){
                    //printf("I AM BREAKING\n");
                    break;
                }
                buffer[rBytes] = '\0'; //end message w newline char
                int status_code;
                int length;
                int errors = 0;
                int entries = 0;

                int nscan = sscanf((char*)buffer, "HTTP/1.1 %d", &status_code);
                if (nscan < 1){
                    printf("sscanf didn't work\n");
                }

                if (status_code == 500 || status_code == 404){
                    hc->servers[i]->alive = false;   //set the server to dead
                    continue;
                }

                char cont[] = "Content-Length:";
                char* content = strstr((char*)buffer, cont);
                if (content == NULL){
                    printf("shouldn't get here\n");
                }else{
                    int ret1 = sscanf(content, "Content-Length: %d\r\n%d\n%d", &length, &errors, &entries);

                    //check for error in this substring
                    if (ret1 < 1){
                        printf("sscanf didn't work\n");
                    }
                }

                // printf("Status: %d \nErrors:%d\nEntries: %d\n", status_code, errors, entries);

                //CRITCAL SECTION for server requests
                pthread_mutex_lock(hc->serverLock);
                hc->servers[i]->totalRequests = entries;
                hc->servers[i]->totalErrors = errors;
                hc->servers[i]->alive = true;
                pthread_mutex_unlock(hc->serverLock);
                //END OF CRITICAL SECTION

                memset(&buffer, 0, sizeof(buffer));
            }   //end of while loop for recv

            close(connfd);

        }   //end of for loop through servers

        pthread_cond_signal(hc->optCond);  //signal dispatch thread that hc is done and we can find optServer

    }   //end of while loop
}

/*
    \brief 4. Function called by all worker threads
    \param *thread - pointer to thread which is executing this function
*/
void* handleRequest(void* thread){
    struct worker* wThread = (struct worker*)thread;

    while (true){
        printf(KRED "Thread [%zu] is ready for a task\n" RESET, wThread->ID);

        //---CRITICAL SECTION---

        pthread_mutex_lock(wThread->lock);
        //we must wait for a valid client_sockd
        while(wThread->acceptfd < 0){
            //wait until the thread is signaled by the dispatcher
            pthread_cond_wait(&wThread->condition, wThread->lock);
        }
        pthread_mutex_unlock(wThread->lock);

        //---END OF CRITICAL SECTION---

        printf(KGRN "Worker\t[%zu]\t Handling socket: %u\n" RESET, wThread->ID, wThread->acceptfd);

        size_t optServer = wThread->optServer;

        // printf(KMAG "optServer = %ld\n" RESET, optServer);

        // for (size_t i = 0; i < wThread->portCounter; i++){
        //     printf(KBLU "Port ID %zu\n" RESET, wThread->servers[i]->port);
        //     printf(KBLU "Port fd %d\n" RESET, wThread->servers[i]->connfd);
        //     printf(KBLU "Port totalRequests %ld\n" RESET, wThread->servers[i]->totalRequests);
        // }

        int connfd = client_connect(wThread->servers[optServer]->port);
        if (connfd < 0){
            //mark the server as down and send 500 error
            wThread->servers[optServer]->alive = false;
            dprintf(wThread->acceptfd, INTERNAL);
        } else {
            bridge_loop(wThread->acceptfd, connfd);
            close(connfd);
        }

        // printf("optServer fd = %d\n",  connfd;

        printf(KRED "Thread [%zu] DONE handling socket [%u]\n" RESET,  wThread->ID, wThread->acceptfd);
        close(wThread->acceptfd);
        // CLOSE CONNFD
        wThread->acceptfd = INT_MIN;

        //---CRITICAL SECTION---

        pthread_mutex_lock(wThread->requestLock);
        //we increment the requestCount when a request has finished 
        requestCount++; 
        // if (requestCount == R_value){
        //     pthread_cond_signal(wThread->hcCond);
        // }
        pthread_mutex_unlock(wThread->requestLock);

        //---END OF CRITICAL SECTION---


        pthread_cond_signal(wThread->dispatchCond);
    }
}


int main(int argc,char **argv) {

    char* numThreads = NULL;
    char* numRequests = NULL;
    bool existingPort = false;
    int clientPort = 0;
    int threadCount = 4;    //default amount of threads
    int R_value = 5;   //default amount of requests between healthchecks
    int c;
    size_t* portArray = (size_t*)malloc(argc*sizeof(size_t));
    size_t portCounter = 0;

    opterr = 0;

    while (optind < argc) {
        if ((c = getopt(argc, argv, "-:N:R:")) != -1){
            switch (c) {
                case 'N':
                    numThreads = optarg;
                    break;
                case 'R':
                    numRequests = optarg;
                    break;
                case '\1':
                    if (existingPort){  //server ports -> place into port array
                        portArray[portCounter] = atoi(optarg);  
                        if (portArray[portCounter] < 1024){ //the arg or port is invalid
                            fprintf(stderr, "Invalid port argument: %s.\n", argv[optind-1]);
                            return EXIT_FAILURE;
                        }
                        portCounter++;
                    } else {    //it's the first port arg, so it's the client side
                        clientPort = atoi(optarg);
                        if (clientPort < 1024){ //the arg or port is invalid
                            fprintf(stderr, "Invalid port argument: %s.\n", argv[optind-1]);
                            return EXIT_FAILURE;
                        }
                        existingPort = true;
                    }
                    break;
                case '?':
                    if (isprint (optopt)) {
                        fprintf(stderr, "Unknown option -%c.\n", optopt);
                    } else {
                        fprintf(stderr, "Unknown option character \\x%x'.\n", optopt);
                    }
                    return EXIT_FAILURE;
                case ':':
                    if (optopt == 'R' || optopt == 'N') {
                        fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                    }
                
                    return EXIT_FAILURE;
                default:
                    abort();
            }
        }
    }

    if (!existingPort){
        fprintf(stderr, "Missing client port number\n");
        return EXIT_FAILURE;
    }

    if (portCounter < 1){
        fprintf(stderr, "Program needs at least one server port number\n");
        return EXIT_FAILURE;
    }

    size_t serverPorts[portCounter];    //actual array of size portCounter for the server ports
    for (int i = 0; i < argc; i++){
        if (portArray[i] != NULL){
            serverPorts[i] = portArray[i];
        } 
    }

    free(portArray);    //we no longer need portArray


    if (numThreads != NULL){
        threadCount = atoi(numThreads);
        if (threadCount <= 0){
            fprintf(stderr, "Invalid argument for thread count\n");
            return EXIT_FAILURE;
        }
    }

    if (numRequests != NULL){
        R_value = atoi(numRequests);
        if (R_value <= 0){
            fprintf(stderr, "Invalid argument for request count\n");
            return EXIT_FAILURE;
        }
    }

    //variables needed for ports
    int connfd, listenfd, acceptfd;
    uint16_t connectport, listenport;

    //declaring array of servers-----------------------
    struct server servers[portCounter];

    //initializing values for servers in array
    for (size_t i = 0; i < portCounter; i++){
        servers[i].totalRequests = 0;
        servers[i].totalErrors = 0;
        servers[i].alive = true;

        //setting socket descriptors for server ports
        connectport = serverPorts[i];
        servers[i].port = connectport;
        if ((connfd = client_connect(connectport)) < 0){
            //couldn't connect so the server is dead
            servers[i].alive = false;
        } 
        servers[i].connfd = connfd;
        printf("Server %d has connected to fd %d\n", connectport, servers[i].connfd);
        if (connfd >= 0){
            close(connfd);
        }
    }

    //create client-side port to listen to client as a server
    listenport = clientPort;
    if ((listenfd = server_listen(listenport)) < 0)
        err(1, "failed listening");

    //declare array of WORKERS -------------------------
    struct worker taskForce[threadCount];
    int error = 0;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t dispatchLock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t dispatchCond = PTHREAD_COND_INITIALIZER;
    pthread_mutex_t requestLock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t hcLock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t hcCond = PTHREAD_COND_INITIALIZER;
    pthread_mutex_t serverLock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t optLock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t optCond = PTHREAD_COND_INITIALIZER;

    //initialize values for the workers in array
    for (int i = 0; i < threadCount; i++){
        taskForce[i].acceptfd = INT_MIN;    //initialize client_sockd to very negative number
        taskForce[i].optServer = INT_MIN;      //SHOULD THIS BE AN ARRAY OF SERVER PORTS
        taskForce[i].ID = i;
        taskForce[i].condition = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
        taskForce[i].lock = &lock;
        // taskForce[i].dispatchLock = &dispatchLock;
        taskForce[i].dispatchCond = &dispatchCond;
        taskForce[i].requestLock = &requestLock;
        taskForce[i].hcCond = &hcCond;
        taskForce[i].portCounter = portCounter;

        //for loop for servers array in worker thread
        for (size_t j = 0; j < portCounter; j++){
            taskForce[i].servers[j] = &servers[j];
        }

        error = pthread_create(&taskForce[i].workerID, NULL, handleRequest, (void *)&taskForce[i]);

        if (error){
            fprintf(stderr, "Error creating thread[%d]", i);
            return EXIT_FAILURE;
        }
    }

    //declare HEALTHCHECKER struct var -------------------------
    struct healthchecker hcThread;  
    hcThread.hcLock = &hcLock;
    hcThread.hcCond = &hcCond;
    hcThread.serverLock = &serverLock;
    hcThread.optCond = &optCond;
    hcThread.portCounter = portCounter;
    //for loop for servers array in healthchecker thread
    for (size_t j = 0; j < portCounter; j++){
        hcThread.servers[j] = &servers[j];
    }
    error = pthread_create(&hcThread.hcID, NULL, hcProbe, (void *)&hcThread);

    if (error){
        fprintf(stderr, "Error creating healthchecker thread");
        return EXIT_FAILURE;
    }

    ssize_t count = 0;
    ssize_t target = 0;
    ssize_t busy = 0;
    while (true) {
        //printf("[+] server is waiting...\n");

        /*
         * 1. Accept Connection
         */
        acceptfd = accept(listenfd, NULL, NULL);

        if (acceptfd < 0){
            fprintf(stderr, "Error accepting connection\n");
            return EXIT_FAILURE;
        }
        // Remember errors happen      

        target = count % threadCount;

        //DETERMINE HERE THREAD AVAILABILITY 
        //If no threads available, dispatcher must sleep
        //wait for worker thread to wake up 
        pthread_mutex_lock(&dispatchLock);
        while (taskForce[target].acceptfd >= 0){
            busy++;
            printf(KYEL "[%zu] busy thread(s)\n" RESET, busy);
            if (busy == threadCount){
                printf(KYEL "ALL THREADS BUSY\n" RESET);
                pthread_cond_wait(&dispatchCond, &dispatchLock);
                printf(KMAG "WAKE UP\n" RESET);

                //inner while loop is to find the thread which woke up dispatcher
                //without incrementing busy
                while (taskForce[target].acceptfd >= 0){
                    count++;
                    target = count % threadCount;
                }
                busy--;
                break;
            }
            count++;
            target = count % threadCount;
        }
        pthread_mutex_unlock(&dispatchLock);


        //MAYBE SOMETHING ABOUT HEALTHCHECKS HERE (bcuz requestCount matters before a thread is signaled)
        pthread_mutex_lock(&optLock);
        if ((requestCount % R_value) == 0){
            pthread_cond_signal(&hcCond);   //signal the healthchecker
            pthread_cond_wait(&optCond, &optLock);
            printf(KMAG "Healthcheck wakes up dispatcher\n"RESET);
        }
        pthread_mutex_unlock(&optLock);

        //for loop to compare server request amounts and find the optimal server
        size_t minRequests = INT_MAX;
        int optServer = -1;
        for (size_t i = 0; i < portCounter; i++){
            if (minRequests == servers[i].totalRequests && servers[i].alive){   //entries are equal
                if (servers[optServer].totalErrors > servers[i].totalErrors){   //tiebreaker through error count
                    optServer = i;
                }
            }
            else if (minRequests > servers[i].totalRequests && servers[i].alive){
                minRequests = servers[i].totalRequests;
                optServer = i;
            }
        }

        //if this minRequests doesn't change, then every server is down
        if (minRequests == INT_MAX){
            dprintf(acceptfd, INTERNAL);
            printf(KRED "ALL SERVERS DOWN\n" RESET);
            count++;
            continue;
        }
        
        //since optServer index is chosen, it is sent a request so it must be incremented
        //CRITCAL SECTION for server requests --------
        pthread_mutex_lock(&serverLock);
            servers[optServer].totalRequests++;
        pthread_mutex_unlock(&serverLock);
        //END OF CRITICAL SECTION --------------------

        if (optServer < 0){
            fprintf(stderr, "This statement should be unreachable\n");
            return EXIT_FAILURE;
        }
        printf(KYEL "optServer = %d with %zu requests\n" RESET, optServer, servers[optServer].totalRequests);

        taskForce[target].acceptfd = acceptfd;
        taskForce[target].optServer = optServer;    //send the optimal server to the thread
        pthread_cond_signal(&taskForce[target].condition);
        printf(KGRN "[%zu] thread is free and signaled\n" RESET, target);
        busy = 0;

        count++;
    }

    return EXIT_SUCCESS;
}
