//Gail Dylan Salak
//1583241
//CSE 130
//Assignment 2: HTTP Server w/ Multithreading and Logging

#include <sys/socket.h>
#include <sys/stat.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <fcntl.h>
#include <unistd.h> // write
#include <string.h> // memset
#include <stdlib.h> // atoi
#include <stdbool.h> // true, false
#include <ctype.h> //character checking for filename
#include <errno.h> //errors for status codes
#include <pthread.h>
#include <limits.h>
#include <getopt.h>
#include <math.h>

#define BUFFER_SIZE 4096
#define NICE 69
#define OK "200 OK\r\n"
#define CREATED "201 Created\r\n"
#define BAD_REQUEST "400 Bad Request\r\n"
#define FORBIDDEN "403 Forbidden\r\n"
#define NOT_FOUND "404 Not Found\r\n"
#define INTERNAL "500 Internal Server Error\r\n"

#define ENTRY_END "========\n"

#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KMAG  "\x1B[35m"
#define KCYN  "\x1B[36m"
#define RESET "\033[0m"

//GLOBAL VARIABLES FOR LOG FILE
size_t LOG_SIZE = 0;
size_t ENTRY_COUNT = 0;
size_t ERROR_COUNT = 0;

//3) Implement fstat()? for GET and HEAD

//FUCKY CASE: 1 THREAD w/ MULTIPLE PUTS

struct httpObject {
    /*
        Create some object 'struct' to keep track of all
        the components related to a HTTP message
    */
    char method[BUFFER_SIZE];       // PUT, HEAD, GET
    char filename[BUFFER_SIZE];     // what is the file we are worried about
    char httpversion[BUFFER_SIZE];  // HTTP/1.1
    ssize_t content_length;         // example: 13
    int status_code;
    bool validity;                  //true until proved false
    char buffer[BUFFER_SIZE];    //BUFFER_SIZE because it's max length of request
};

struct worker {

    struct httpObject message;
    ssize_t ID;
    ssize_t client_sockd;
    pthread_t workerID;
    pthread_cond_t condition;
    pthread_mutex_t* lock;
    pthread_cond_t* dispatchCond;
    pthread_mutex_t* dispatchLock;
    int logFD;
    char logBuffer[16384];            //16 KiB size buffer accoring to spec
    pthread_mutex_t* logLock;
};

/*
    \brief 1. Want to read in the HTTP message/ data coming in from socket
    \param client_sockd - socket file descriptor
    \param message - object we want to 'fill in' as we read in the HTTP message
*/
void readResponse(ssize_t client_sockd, struct httpObject* message) {
    //printf("----------READING HTTP RESPONSE----------\n");

    ssize_t recvBytes = recv(client_sockd, message->buffer, BUFFER_SIZE, 0);

    //checking for error in recv()
    if (recvBytes < 0){
        message->status_code = 500; //Internal Server Error
        message->content_length = 0;
        return;
    }

    int ret = sscanf((char*)&message->buffer, "%s %s %s", message->method, message->filename, message->httpversion);

    if (ret < 1){
        message->validity = false;
        printf("Invalid format (not 3 strings to begin)");
        return;
    }

    // printf("Method: %s\n", message->method);
    // printf("Filename: %s\n", message->filename);
    // printf("httpversion: %s\n", message->httpversion);

    char cont[] = "Content-Length:";
    char* content = strstr((char*)&message->buffer, cont);
    if (content == NULL){
        //printf("No Content-Length, not a PUT request\n");
        message->content_length = 0;   //initialize content length to 0
    }else{
        ssize_t length;
        int ret1 = sscanf(content, "Content-Length: %zd", &length);

        //check for error in this substring
        if (ret1 < 1){
            message->validity = false;
            printf("Invalid content length format\n");
            return;
        }

        if (length < 0){
            message->validity = false;
            message->content_length = 0;
            return;
        }else{
            message->content_length = length;
        }
        // printf("Content = %s\n", content);
        // printf("Content-Length: %zd\n", length);
    }

    //checking validity of method
    if (strcmp(message->method, "HEAD") != 0 && 
        strcmp(message->method, "PUT") != 0 && 
        strcmp(message->method, "GET") != 0){   

        message->validity = false;
        printf("Invalid method\n");
        return;
    } //end of method checking

    //checking validity of filename
    if (strlen(message->filename) > 28){    //filename is too long
        message->validity = false;
        printf("Invalid filename: %s (too long)\n", message->filename);
        return;
    } else {
        if ('/' != message->filename[0]){ //checking if first character is slash
            message->validity = false;
            printf("Invalid filename (no slash)\n");
            return;
        }

        // printf("filename = %s\n", &message->filename[1]);
        //loop to check for valid characters
        for (size_t i = 1; i <= 28; i++){
            //FOR SOME REASON: this if statement should be the address, 
            //but if I change it, it causes errors
            if (message->filename[i] == NULL){
                if (i == 1){    //filename is just a slash so it's invalid
                    message->validity = false;
                    printf("Invalid filename (just a slash)\n");
                    return;
                } else {    //done parsing through each character
                    break;
                }
            }
            if (!isalnum(message->filename[i]) && 
                '-' != message->filename[i] && 
                '_' != message->filename[i]){

                message->validity = false;
                printf("inval char[%zu] = %c\n", i, message->filename[i]);
                printf("Invalid filename (Invalid character)\n");
                return;
            }
        }
    }   //end of filename checking

    //checking validity of httpversion
    if (strcmp(message->httpversion, "HTTP/1.1") != 0){   
        message->validity = false;
        printf("Invalid httpversion\n");
        return;
    } //end of httpversion checking

    char CRLF[] = "\r\n\r\n";
    char* headerEnd = strstr((char*)&message->buffer, CRLF);
    if (headerEnd == NULL){
        message->validity = false;
        printf("Invalid request (too long)\n");
        return;
    }

    message->validity = true;
    return;
}

/*
    \brief 2. Want to process the message we just recieved
    \param client_sockd - socket file descriptor
    \param message - object we will process contents from and write to
    \return fd - constructing response will need the file descriptor from this process
*/
ssize_t processRequest(ssize_t client_sockd, struct httpObject* message) {
    //printf("----------PROCESSING REQUEST----------\n");

    // check validity as a result of reading
    if (!message->validity){
        message->status_code = 400; //BAD REQUEST
        printf("%s", BAD_REQUEST);
        return -1;
    }

    if (message->status_code == 500){   //there was a problem with receive 
        printf("%s", INTERNAL);
        return -1;
    }

    //Deal with PUT first since GET and HEAD are the same
    if (strcmp(message->method, "PUT") == 0){
        //open file for reading
        int fd = open(&message->filename[1], O_RDWR | O_TRUNC, 0664); //filename string without '/'
        message->status_code = 200; //assuming open worked, set 200 -> will change if file is created

        if (fd < 0){    //check reason for error
            if (errno == EACCES || errno == EISDIR){   //Permission denied
                message->status_code = 403; //Forbidden
                message->content_length = 0;

                //test if file has no read permission
                fd = open(&message->filename[1], O_RDONLY | O_TRUNC, 0664);

                //if no read permission, create new file with read permission
                if (fd < 0){
                    fd = open(&message->filename[1], O_WRONLY | O_CREAT | O_TRUNC, 0664); //create new file

                    //check if something went wrong in creating a file
                    if (fd < 0){
                        message->status_code = 403; //the file has NO permissions
                        message->content_length = 0;
                        printf("%s", FORBIDDEN);
                        return -1;
                    }

                    message->status_code = 201; //Created
                    printf("%s", CREATED);
                }

                printf("%s\n", FORBIDDEN);
                // return -1;
            } else if (errno == ENOENT){    //No such file or directory
                fd = open(&message->filename[1], O_WRONLY | O_CREAT | O_TRUNC, 0664); //create new file

                //check if something went wrong in creating a file
                if (fd < 0){
                    message->status_code = 500; //INTERNAL SERVER ERROR
                    message->content_length = 0;
                    printf("%s", INTERNAL);
                    return -1;
                }

                message->status_code = 201; //Created
                printf("%s", CREATED);
            }
        } 

        if (message->status_code == 403){
            return -1;
        }


        //at this point fd should point to an open file

        if (message->content_length != 0){
            ssize_t tempLength = message->content_length;   //to preserve the original content length
            while (true){
                // printf("PUT loop enter\n");
                //receives BUFFER_SIZE bytes from client_sockd into buffer
                ssize_t rBytes = recv(client_sockd, message->buffer, BUFFER_SIZE, 0);
                // printf("recv WORKED\n");
                //check for error 
                if (rBytes < 0){
                    message->status_code = 500; //Internal Server Error
                    message->content_length = 0;
                    printf("%s", INTERNAL);
                    return -1;
                }

                //if read reaches EOF
                else if(rBytes == 0){
                    break;
                }
                
                //write rBytes from buffer to fd
                ssize_t wBytes = write(fd, message->buffer, rBytes);

                //check for error 
                if (wBytes < 0 || rBytes != wBytes){
                    message->status_code = 500; //Internal Server Error
                    message->content_length = 0;
                    printf("%s", INTERNAL);
                    return -1;
                }

                //with each loop decrement till we reach 0 -> the whole content length
                tempLength -= wBytes;  
                if (tempLength == 0){
                    break;
                }
            }
        }

        //at this point, body data has been written into fd
        printf("PUT loop finished\n");
        return fd;            
    } 

    //***** IMPLEMENT FSTAT??? *****//
    //this conditional deals with both GET and HEAD
    else {    
        //open file for reading
        int fd = open(&message->filename[1], O_RDONLY); //filename string without '/'

        if (fd < 0){    //check reason for error
            if (errno == EACCES || errno == EISDIR){   //Permission denied
                message->status_code = 403; //Forbidden
                message->content_length = 0;\
                printf("%s", FORBIDDEN);
                return -1;
            } else if (errno == ENOENT){    //No such file or directory
                message->status_code = 404; //Not Found
                message->content_length = 0;
                printf("%s", NOT_FOUND);
                return -1;
            }
        } else {    //file has been opened for reading    

            while (true){

                //reads BUFFER_SIZE bytes from fd into buffer
                ssize_t rBytes = read(fd, message->buffer, BUFFER_SIZE);

                //check for error (read returns -1 for error)
                if (rBytes < 0){
                    message->status_code = 500; //Internal Server Error
                    message->content_length = 0;
                    printf("%s", INTERNAL);
                    return -1;
                }
                //if read reaches EOF
                else if(rBytes == 0){
                    break;
                }
                message->content_length += rBytes;  //add # of bytes read to content-length with every loop
            }
            message->status_code = 200; //OK
            return fd;
        }

    }

    return -1;  //fd doesn't matter
}

/*
    \brief 3. Construct some response based on the HTTP request you recieved
    \param client_sockd - socket file descriptor
    \param message - object we will process contents from and write to
    \param fd - file descriptor or -1 returns from process
*/
void constructResponse(ssize_t client_sockd, struct httpObject* message, int fd) {
    //printf("----------CONSTRUCTING RESPONSE----------\n");

    //Write Header message

    //1. First HTTP version
    dprintf(client_sockd, "HTTP/1.1 ");

    //2. Then status code
    if (message->status_code == 200){ dprintf(client_sockd, "%s", OK);
    }else if (message->status_code == 201){ dprintf(client_sockd, "%s", CREATED);
    }else if (message->status_code == 400){ dprintf(client_sockd, "%s", BAD_REQUEST);
    }else if (message->status_code == 403){ dprintf(client_sockd, "%s", FORBIDDEN);
    }else if (message->status_code == 404){ dprintf(client_sockd, "%s", NOT_FOUND);
    }else if (message->status_code == 500){ dprintf(client_sockd, "%s", INTERNAL);}

    //3. Then Content-Length header
    dprintf(client_sockd, "Content-Length: %zu\r\n\r\n", message->content_length);

    //if request is a GET and the fd is valid, we need to print the file contents
    if(strcmp(message->method, "GET") == 0 && message->status_code == 200){

        close(fd);
        //open file for reading
        fd = open(&message->filename[1], O_RDONLY); //filename string without '/' 

        ssize_t totalBytes = 0; //counter for total bytes written
        while (true){

            //receives BUFFER_SIZE bytes from fd into buffer
            ssize_t rBytes = read(fd, message->buffer, BUFFER_SIZE);

            //check for error 
            if (rBytes < 0){
                printf("GET contents error\n");
                break;
            }
            //if read reaches EOF
            else if(rBytes == 0){
                break;
            }
            
            
            //write rBytes from buffer to fd
            ssize_t wBytes = write(client_sockd, message->buffer, rBytes);
            //check for error 
            if (wBytes < 0 || rBytes != wBytes){
                printf("GET contents error\n");
                break;
            }

            totalBytes += wBytes;
            if (message->content_length == totalBytes){
                break;
            }
        }
    }

    //check if fd is open 
    if (fd >= 0){
        close(fd);
    }

    close(client_sockd);
    return;
}

void* logging(void* thread, ssize_t reservedSpace, ssize_t start_write, bool isHealthcheck){
    
    struct worker* wThread = (struct worker*)thread;
    // printf("BEGINNING OF LOGGING: %s", wThread->logBuffer);
    ssize_t tempReserved = reservedSpace;

    // printf(KBLU "tempReserved = %zu\n" RESET, tempReserved);

    ssize_t wBytes = pwrite(wThread->logFD, wThread->logBuffer, strlen(wThread->logBuffer), start_write);
    tempReserved -= wBytes;
    start_write += wBytes;

    //if it's a valid request AND it's either GET or PUT, log the data
    //AND there is content to be logged
    if ((wThread->message.status_code == 200 || wThread->message.status_code == 201) && 
        (strcmp(wThread->message.method, "GET") == 0 || strcmp(wThread->message.method, "PUT") == 0) &&
        wThread->message.content_length != 0){
        // printf(KBLU "WOULD BE LOGGING DATA LINES HERE\n" RESET);

        //conditional log for valid GET healthcheck
        if (isHealthcheck){
            sprintf(wThread->logBuffer, "%08d", 0);

            wBytes = pwrite(wThread->logFD, wThread->logBuffer, strlen(wThread->logBuffer), start_write);
            tempReserved -= wBytes;
            start_write += wBytes;

            char vibeCheck[100];
            size_t TEMP_ENTRIES = ENTRY_COUNT - 1;  //this log entry shouldn't contain itself in the count
            sprintf(vibeCheck, "%zu\n%zu",  ERROR_COUNT, TEMP_ENTRIES);

            //adapted from Clark's code on Piazza
            for (size_t idx = 0; idx < strlen(vibeCheck) / sizeof(char); ++idx) {
                snprintf(wThread->logBuffer, 16384, " %02x", vibeCheck[idx]);
                // printf("%s", wThread->logBuffer);
                wBytes = pwrite(wThread->logFD, wThread->logBuffer, strlen(wThread->logBuffer), start_write);
                tempReserved -= wBytes;
                start_write += wBytes;
            }

            snprintf(wThread->logBuffer, 16384, "\n");
            wBytes = pwrite(wThread->logFD, wThread->logBuffer, strlen(wThread->logBuffer), start_write);
            tempReserved -= wBytes;
            start_write += wBytes;

        } else {   //else log normally

            int fd2Log = open(wThread->message.filename + 1, O_RDONLY);
            if (fd2Log < 0){
                printf("Something went wrong opening file to read for log\n");
            }

            size_t charCount = 0;   //will keep count for every line logged
            ssize_t dataBytes = 0;   //track all of logged bytes and compare to content length

            while (true){
                ssize_t rBytes = read(fd2Log, wThread->message.buffer, 20);

                //check for error 
                if (rBytes < 0){
                    printf("LOG reading contents error\n");
                    break;
                }
                       
                sprintf(wThread->logBuffer, "%08zu", charCount);

                wBytes = pwrite(wThread->logFD, wThread->logBuffer, strlen(wThread->logBuffer), start_write);
                tempReserved -= wBytes;
                start_write += wBytes;

                //adapted from Clark's code on Piazza
                for (size_t idx = 0; idx < rBytes / sizeof(char); ++idx) {
                    snprintf(wThread->logBuffer, 16384, " %02x", wThread->message.buffer[idx]);
                    // printf("%s", wThread->logBuffer);
                    wBytes = pwrite(wThread->logFD, wThread->logBuffer, strlen(wThread->logBuffer), start_write);
                    tempReserved -= wBytes;
                    start_write += wBytes;
                }

                snprintf(wThread->logBuffer, 16384, "\n");
                wBytes = pwrite(wThread->logFD, wThread->logBuffer, strlen(wThread->logBuffer), start_write);
                tempReserved -= wBytes;
                start_write += wBytes;

                dataBytes += rBytes;
                if (dataBytes == wThread->message.content_length){
                    // printf(KBLU "Logged ALL of the data\n");
                    break;
                }

                charCount += 20;    //increment charCount by 20 for the next line
            }
               
        close(fd2Log);
        }
    } //end of logging data

    sprintf(wThread->logBuffer, ENTRY_END);
    wBytes = pwrite(wThread->logFD, wThread->logBuffer, strlen(wThread->logBuffer), start_write);
    tempReserved -= wBytes;
    if (tempReserved == 0){
        printf(KMAG "YOU SHOULD SEE THIS MESSAGE IF LOGGED CORRECTLY\n" RESET);
    }
    return wThread;
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
        while(wThread->client_sockd < 0){
            //wait until the thread is signaled by the dispatcher
            pthread_cond_wait(&wThread->condition, wThread->lock);
        }
        pthread_mutex_unlock(wThread->lock);

        //---END OF CRITICAL SECTION---

        printf(KGRN "Worker\t[%zu]\t Handling socket: %zu\n" RESET, wThread->ID, wThread->client_sockd);

        // Read HTTP Message
        readResponse(wThread->client_sockd, &wThread->message);

        //Check here for healthcheck

        bool isHealthcheck = false; //only will be made true for valid GET healthcheck
        if (wThread->message.validity && strcmp(wThread->message.filename, "/healthcheck") == 0){

            if (strcmp(wThread->message.method, "GET") != 0){    //HEAD or PUT returns 403

                //1. First HTTP version
                dprintf(wThread->client_sockd, "%s ", wThread->message.httpversion);

                //2. Then status code of 403
                dprintf(wThread->client_sockd, "%s", FORBIDDEN);

                //3. Then Content-Length header of 0
                dprintf(wThread->client_sockd, "Content-Length: 0\r\n\r\n");

                wThread->message.status_code = 403; //FORBIDDEN status code in case there is log file

            } else if (wThread->logFD < 0 ){ //if there's no log file, send 404 response

                //1. First HTTP version
                dprintf(wThread->client_sockd, "%s ", wThread->message.httpversion);

                //2. Then status code of 404
                dprintf(wThread->client_sockd, "%s", NOT_FOUND);

                //3. Then Content-Length header of 0
                dprintf(wThread->client_sockd, "Content-Length: 0\r\n\r\n");

            } else {    //valid healthcheck

                char vibeCheck[100];
                sprintf(vibeCheck, "%zu\n%zu",  ERROR_COUNT, ENTRY_COUNT);
                // char vibeCheck[strlen(tempCheck)];
                // sprintf(vibeCheck, "%s", tempCheck);

                // printf(KCYN "vibeCheck:\n%s\nhas %lu bytes\n" RESET, vibeCheck, strlen(vibeCheck));

                //1. First HTTP version
                dprintf(wThread->client_sockd, "%s ", wThread->message.httpversion);

                //2. Then status code of 200
                dprintf(wThread->client_sockd, "%s", OK);

                wThread->message.content_length = strlen(vibeCheck);
                //3. Then Content-Length header 
                dprintf(wThread->client_sockd, "Content-Length: %zu\r\n\r\n%s", wThread->message.content_length, vibeCheck);

                wThread->message.status_code = 200; //set status_code for logging
                isHealthcheck = true;   //boolean to pass to logging
            }


        } else {   //no health check so process regularly
            // Process Request
            ssize_t fd = processRequest(wThread->client_sockd, &wThread->message);

            // Construct Response
            constructResponse(wThread->client_sockd, &wThread->message, fd);
        }

        // pthread_mutex_unlock(wThread->lock);

        //LOGGING: 
        if (wThread->logFD >= 0){

            //---CRITICAL SECTION---

            pthread_mutex_lock(wThread->logLock);

            //the variable to reserve space in the log file
            //must be thread-safe as all threads will be changing this
            ssize_t reservedSpace = 0;

            //if the request was successfull
            if (wThread->message.status_code == 200 || wThread->message.status_code == 201){
                //method to find length of an integer from stack overflow:
                //https://stackoverflow.com/questions/3068397/finding-the-length-of-an-integer-in-c
                ssize_t lengthContent = 1;  //for the case of 0
                if (wThread->message.content_length !=0){
                    lengthContent = floor(log10(wThread->message.content_length)) + 1;
                }

                ssize_t defaultLines = 22 + strlen(wThread->message.filename) + lengthContent;
                // printf(KBLU "defaultLines = %zu\n" RESET, defaultLines);

                if (strcmp(wThread->message.method, "HEAD") == 0){   
                    // printf(KBLU "Logging a HEAD\n" RESET);
                    reservedSpace = defaultLines + 1;   //add 1 bcuz defaultLines is calculated for GET/PUT
                    // printf(KBLU "%s" RESET, entry);
                } else {
                    // printf(KBLU "Logging a GET/PUT\n" RESET);
                    //calculating the characters for all the full lines
                    ssize_t dataLines = (floor(wThread->message.content_length/20) * NICE);
                    //accounting for extra line that isn't full
                    if ((wThread->message.content_length % 20) != 0){   
                         dataLines = dataLines + (9 + (3 * (wThread->message.content_length % 20)));
                    }
                    reservedSpace = defaultLines + dataLines;
                }

                sprintf(wThread->logBuffer, "%s %s length %zu\n", wThread->message.method, wThread->message.filename, wThread->message.content_length);

            } else {    //error status_codes go here
                // printf(KBLU "Logging a FAILURE\n" RESET);
                // printf(KBLU "method = %lu, filename = %lu, version = %lu\n" RESET, strlen(wThread->message.method), strlen(wThread->message.filename), strlen(wThread->message.httpversion));
                reservedSpace = 35 + strlen(wThread->message.method) + strlen(wThread->message.filename) + strlen(wThread->message.httpversion);
                ERROR_COUNT++;  //increment global failure count
                sprintf(wThread->logBuffer, "FAIL: %s %s %s --- response %d\n", wThread->message.method, wThread->message.filename, wThread->message.httpversion, wThread->message.status_code);
            }

            ENTRY_COUNT++;  //increment global entry count
            // printf(KBLU "reservedSpace = %zu\n" RESET, reservedSpace);

            ssize_t start_write = LOG_SIZE;
            LOG_SIZE += reservedSpace;

            pthread_mutex_unlock(wThread->logLock);

            //---END OF CRITICAL SECTION---

            //function that handles all of the logging
            logging(wThread, reservedSpace, start_write, isHealthcheck);
            printf(KBLU "Thread [%zu] is done logging (%s)\n", wThread->ID, wThread->message.method);
        }


        printf(KRED "Thread [%zu] DONE handling socket [%zu]\n" RESET,  wThread->ID, wThread->client_sockd);
        // close(wThread->client_sockd);
        wThread->client_sockd = INT_MIN;
        pthread_cond_signal(wThread->dispatchCond);
    }
}




int main(int argc, char** argv) {

    //INSERT ARGUMENT CHECKERS AND GETOPT HERE
    char* numThreads = NULL;
    char* logFile = NULL;
    bool existingPort = false;
    int portArg;
    int threadCount = 4;
    int c;

    opterr = 0;

    while (optind < argc) {
        if ((c = getopt(argc, argv, "-:N:l:")) != -1){
            switch (c) {
                case 'N':
                    numThreads = optarg;
                    break;
                case 'l':
                    logFile = optarg;
                    break;
                // case '\1':
                //     portArg = atoi(optarg);
                //     if (portArg < 1024 || existingPort){ //the arg or port is invalid or there was already an inputted port
                //         fprintf(stderr, "Invalid argument: %s.\n", argv[optind]);
                //         return EXIT_FAILURE;
                //     }
                //     existingPort = true;
                //     break;
                case '?':
                    if (optopt == 'l' || optopt == 'N') {
                        fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                    } else if (isprint (optopt)) {
                        fprintf(stderr, "Unknown option -%c.\n", optopt);
                    } else {
                        fprintf(stderr, "Unknown option character \\x%x'.\n", optopt);
                    }
                
                    return EXIT_FAILURE;
                default:
                    abort();
            }
        }
        else{
            printf("Non flag value = %s\n", argv[optind]);
            portArg = atoi(argv[optind]);
            if (portArg == 0 || portArg < 1024 || existingPort){ //the arg or port is invalid or there was already an inputted port
                fprintf(stderr, "Invalid argument: %s.\n", argv[optind]);
                return EXIT_FAILURE;
            }
            existingPort = true;
            optind++;
        } 
    }

    // printf("N: %s\nlogFile: %s\nportArg: %d\noptind: %d\n", numThreads, logFile, portArg, optind);

    if (!existingPort){
        fprintf(stderr, "Missing port number\n");
        return EXIT_FAILURE;
    }

    if (numThreads != NULL){
        threadCount = atoi(numThreads);
        if (threadCount == 0){
            fprintf(stderr, "Invalid argument for thread count\n");
            return EXIT_FAILURE;
        }
    }

    int logFD = -1; //will be changed to file descriptor if logFile is given 
    if (logFile != NULL){
        logFD = open(logFile, O_RDWR | O_CREAT | O_TRUNC, 0664);
        if (logFD < 0){
            fprintf(stderr, "Error in creating log file\n");
            return EXIT_FAILURE;        
        }
        printf("Opened log file: %s\n", logFile);
    }

    /*
        Create sockaddr_in with server information
    */

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(portArg);  //portArg given at command line as argument
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    socklen_t addrlen = sizeof(server_addr);

    /*
        Create server socket
    */
    int server_sockd = socket(AF_INET, SOCK_STREAM, 0);

    // Need to check if server_sockd < 0, meaning an error
    if (server_sockd < 0) {
        perror("socket");
    }

    /*
        Configure server socket
    */
    int enable = 1;

    /*
        This allows you to avoid: 'Bind: Address Already in Use' error
    */
    int ret = setsockopt(server_sockd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    /*
        Bind server address to socket that is open
    */
    ret = bind(server_sockd, (struct sockaddr *) &server_addr, addrlen);

    /*
        Listen for incoming connections
    */
    ret = listen(server_sockd, 5); // 5 should be enough, if not use SOMAXCONN

    if (ret < 0) {
        return EXIT_FAILURE;
    }

    /*
        Connecting with a client
    */
    struct sockaddr client_addr;
    socklen_t client_addrlen;

    //declare httObject variable
    // struct httpObject message;

    //declare array of workers 
    struct worker taskForce[threadCount];
    int error = 0;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t logLock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t dispatchLock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t dispatchCond = PTHREAD_COND_INITIALIZER;


    //initialize values for the workers in array
    for (int i = 0; i < threadCount; i++){
        taskForce[i].client_sockd = INT_MIN;    //initialize client_sockd to very negative number
        taskForce[i].ID = i;
        taskForce[i].condition = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
        taskForce[i].lock = &lock;
        taskForce[i].logLock = &logLock;
        taskForce[i].logFD = logFD;
        taskForce[i].dispatchLock = &dispatchLock;
        taskForce[i].dispatchCond = &dispatchCond;

        error = pthread_create(&taskForce[i].workerID, NULL, handleRequest, (void *)&taskForce[i]);

        if (error){
            fprintf(stderr, "Error creating thread[%d]", i);
            return EXIT_FAILURE;
        }
    }

    ssize_t count = 0;
    ssize_t target = 0;
    ssize_t busy = 0;
    while (true) {
        //printf("[+] server is waiting...\n");

        /*
         * 1. Accept Connection
         */
        int client_sockd = accept(server_sockd, &client_addr, &client_addrlen);

        if (client_sockd < 0){
            fprintf(stderr, "Error accepting connection\n");
            return EXIT_FAILURE;
        }
        // Remember errors happen

        target = count % threadCount;

        //DETERMINE HERE THREAD AVAILABILITY 
        //If no threads available, dispatcher must sleep
        //wait for worker thread to wake up 
        pthread_mutex_lock(&dispatchLock);
        while (taskForce[target].client_sockd >= 0){
            busy++;
            printf(KYEL "[%zu] busy thread(s)\n" RESET, busy);
            if (busy == threadCount){
                printf(KYEL "ALL THREADS BUSY\n" RESET);
                pthread_cond_wait(&dispatchCond, &dispatchLock);
                printf(KMAG "WAKE UP\n" RESET);

                //inner while loop is to find the thread which woke up dispatcher
                //without incrementing busy
                while (taskForce[target].client_sockd >= 0){
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

 
        taskForce[target].client_sockd = client_sockd;
        pthread_cond_signal(&taskForce[target].condition);
        printf(KGRN "[%zu] thread is free and signaled\n" RESET, target);
        busy = 0;

        count++;
    }

    return EXIT_SUCCESS;
}
