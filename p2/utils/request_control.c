#include "../utils/logging.h"
#include "../utils/fifo.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFFER_SIZE 1024

int send_request(int code, char *register_pipe_name, char *pipe_name, char *box_name)  {
    // build request
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE-1);
    buffer[0] =(char) code;
    memcpy(buffer+1, pipe_name, strlen(pipe_name));
    memcpy(buffer+1+256, box_name, strlen(box_name));

    // make pipe_name
    unlink(pipe_name);
    makefifo(pipe_name);

    // open pipe to send request to mbroker
    int fserv = open(register_pipe_name, O_WRONLY);
    if (fserv < 0) return -1;
    
    // send request to mbroker
    ssize_t ret = write(fserv, buffer, BUFFER_SIZE-1);
    if (ret < 0) return -1;

    return 0;
}