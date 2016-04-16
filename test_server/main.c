//
//  server_lab2.c
//  lab2
//
//  Created by allen woo on 4/13/16.
//
//
/*
 * FILE: file_browser.c
 *
 * Description: A simple, iterative HTTP/1.0 Web server that uses the
 * GET method to serve static and dynamic content.
 *
 * Date: April 4, 2016
 */

#include <arpa/inet.h>          // inet_ntoa
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#define LISTENQ  1024  // second argument to listen()
#define MAXLINE 1024   // max length of a line
#define RIO_BUFSIZE 1024

typedef struct {
    int rio_fd;                 // descriptor for this buf
    int rio_cnt;                // unread byte in this buf
    char *rio_bufptr;           // next unread byte in this buf
    char rio_buf[RIO_BUFSIZE];  // internal buffer
} rio_t;

// simplifies calls to bind(), connect(), and accept()
typedef struct sockaddr SA;

typedef struct {
    char filename[512];
    int browser_index;    //  1: Chrome  2: Safari   3: Firefox   4 MSIE
    off_t offset;              // for support Range
    size_t end;
} http_request;

typedef struct {
    const char *extension;
    const char *mime_type;
} mime_map;

char* browser_map[] = {"Chrome","Safari", "Firefox", "MSIE" , "Unknown"};

mime_map meme_types [] = {
    {".css", "text/css"},
    {".gif", "image/gif"},
    {".htm", "text/html"},
    {".html", "text/html"},
    {".jpeg", "image/jpeg"},
    {".jpg", "image/jpeg"},
    {".ico", "image/x-icon"},
    {".js", "application/javascript"},
    {".pdf", "application/pdf"},
    {".mp4", "video/mp4"},
    {".png", "image/png"},
    {".svg", "image/svg+xml"},
    {".xml", "text/xml"},
    {NULL, NULL},
};

char *default_mime_type = "text/plain";

// set up an empty read buffer and associates an open file descriptor with that buffer
void rio_readinitb(rio_t *rp, int fd){
    rp->rio_fd = fd;
    rp->rio_cnt = 0;
    rp->rio_bufptr = rp->rio_buf;
}

// utility function for writing user buffer into a file descriptor
ssize_t written(int fd, void *usrbuf, size_t n){
    size_t nleft = n;
    ssize_t nwritten;
    char *bufp = usrbuf;
    
    while (nleft > 0){
        if ((nwritten = write(fd, bufp, nleft)) <= 0){
            if (errno == EINTR)  // interrupted by sig handler return
                nwritten = 0;    // and call write() again
            else
                return -1;       // errorno set by write()
        }
        nleft -= nwritten;
        bufp += nwritten;
    }
    return n;
}


/*
 *    This is a wrapper for the Unix read() function that
 *    transfers min(n, rio_cnt) bytes from an internal buffer to a user
 *    buffer, where n is the number of bytes requested by the user and
 *    rio_cnt is the number of unread bytes in the internal buffer. On
 *    entry, rio_read() refills the internal buffer via a call to
 *    read() if the internal buffer is empty.
 */
static ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n){
    int cnt;
    while (rp->rio_cnt <= 0){  // refill if buf is empty
        
        rp->rio_cnt = read(rp->rio_fd, rp->rio_buf,
                           sizeof(rp->rio_buf));
        if (rp->rio_cnt < 0){
            if (errno != EINTR) // interrupted by sig handler return
                return -1;
        }
        else if (rp->rio_cnt == 0)  // EOF
            return 0;
        else
            rp->rio_bufptr = rp->rio_buf; // reset buffer ptr
    }
    
    // copy min(n, rp->rio_cnt) bytes from internal buf to user buf
    cnt = n;
    if (rp->rio_cnt < n)
        cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

// robustly read a text line (buffered)
ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen){
    int n, rc;
    char c, *bufp = usrbuf;
    
    for (n = 1; n < maxlen; n++){
        if ((rc = rio_read(rp, &c, 1)) == 1){
            *bufp++ = c;
            if (c == '\n')
                break;
        } else if (rc == 0){
            if (n == 1)
                return 0; // EOF, no data read
            else
                break;    // EOF, some data was read
        } else
            return -1;    // error
    }
    *bufp = 0;
    return n;
}

// utility function to get the format size
void format_size(char* buf, struct stat *stat){
    if(S_ISDIR(stat->st_mode)){
        sprintf(buf, "%s", "[DIR]");
    } else {
        off_t size = stat->st_size;
        if(size < 1024){
            sprintf(buf, "%lu", size);
        } else if (size < 1024 * 1024){
            sprintf(buf, "%.1fK", (double)size / 1024);
        } else if (size < 1024 * 1024 * 1024){
            sprintf(buf, "%.1fM", (double)size / 1024 / 1024);
        } else {
            sprintf(buf, "%.1fG", (double)size / 1024 / 1024 / 1024);
        }
    }
}

// pre-process files in the "home" directory and send the list to the client
void handle_directory_request(int out_fd, int dir_fd, char *filename){
    // send response headers to client e.g., "HTTP/1.1 200 OK\r\n"
    char head[MAXLINE], curtime[MAXLINE], sz[MAXLINE];
    struct stat statbuf;
    sprintf(head, "HTTP/1.1 200 OK\r\n%s%s%s%s%s", "Content-Type: text/html\r\n\r\n", "<html><head><style>", "body{font-family: monospace; font-size: 13px;}","td {padding: 1.5px 6px;}","</style></head><body><table>\n");
    // get file directory
    DIR *d;
    struct dirent *entry;
    d = opendir(filename);          /*Use url to open dir*/
    int ffd;
    if (d != NULL) {
        while ((entry = readdir(d)) != NULL) {         /*read a directory*/
            if (strcmp(entry -> d_name, ".") == 0 || strcmp(entry -> d_name, "..") ==0 || entry ->d_name[0] == '.'){
                continue;
            }
            ffd = openat(dir_fd, entry ->d_name, O_RDONLY);
            fstat(ffd, &statbuf);
            strftime(curtime, sizeof(curtime), "%Y-%m-%d %H:%M", localtime(&statbuf.st_mtime));
            format_size(sz, &statbuf);      /*display size*/
            //            sprintf(buf, "<tr><td><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>\n",
            //            entry->d_name, d, entry->d_name, d, curtime, sz);
            //            written(out_fd, head, strlen(head));
            sprintf(head, "%s<tr><td><a href=\"%s\">%s</a></td><td>%s</td><td>%s</td></tr>", head, entry->d_name, entry->d_name,curtime, sz);
            //            sprintf(head, "%s<tr><td><a href=\"%s\">%s</td>", head, entry->d_name, entry->d_name);
            //            sprintf(head, "%s<td>%s</td><td>%s</td>", head, curtime, sz);
            //            sprintf(head, "%s</tr>", head);
        }
        closedir(d);
    }
    else {
        perror ("Open directory failed");
    }

    sprintf(head, "%s</table>", head);
    written(out_fd, head, strlen(head));
}

// utility function to get the MIME (Multipurpose Internet Mail Extensions) type
static const char* get_mime_type(char *filename){
    char *dot = strrchr(filename, '.');
    if(dot){ // strrchar Locate last occurrence of character in string
        mime_map *map = meme_types;
        while(map->extension){
            if(strcmp(map->extension, dot) == 0){
                return map->mime_type;
            }
            map++;
        }
    }
    return default_mime_type;
}

// open a listening socket descriptor using the specified port number.
int open_listenfd(int port){
    int fd;
    struct sockaddr_in server;
    if ((fd = socket(AF_INET, SOCK_STREAM,0)) < 0) {
        perror("Error: Could not create the server socket\n");
    }
    int temp = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &temp, sizeof(temp)) < 0) {   /* eliminate "Address already in use" error from bind */
        perror("Fail of setsocket");
        exit(EXIT_FAILURE);
    }
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(port);                          /**/
    
    // 6 is TCP's protocol number
    // enable this, much faster : 4000 req/s -> 17000 req/s
    // if (setsockopt(fd, 6, TCP_CORK, (const void*)&temp, sizeof(temp)) < 0) {
    //        perror("Fail of setsocket");
    //     }
    
    // Listenfd will be an endpoint for all requests to port on any IP address for this host
    if ( bind(fd, &server, sizeof(server)) < 0){
        perror("Error on binding");
    }
    
    listen(fd, LISTENQ);
//    listen(fd,port);
    return fd;
}

// decode url
void url_decode(char* src, char* dest, int max) {
    
}

// parse request to get url
//void parse_request(int fd, http_request *req, struct sockaddr_in *clientaddr){
//    rio_t rio;
//    FILE*fp, *ipfp;
//    char usrbuf[MAXLINE], method[MAXLINE], uri[MAXLINE];
//    char index[4*MAXLINE];
//    char temp[1024];
//    char indextemp[MAXLINE];
//    memset(indextemp, 0, strlen(indextemp));
//    memset(index, 0, strlen(index));
//    memset(temp, 0, strlen(temp));
//    
//    // Rio (Robust I/O) Buffered Input Functions
//    rio_readinitb(&rio, fd);
//    rio_readlineb(&rio, usrbuf, MAXLINE);
//    printf("%s",usrbuf);
//    sscanf(usrbuf, "%s %s",method,uri);
//    // read all
//    while(usrbuf[0] != '\n' && usrbuf[1] != '\n') {
//        rio_readlineb(&rio, usrbuf, MAXLINE);
//        
//        if(usrbuf[0]=='U'&&usrbuf[1]=='s'&&usrbuf[2]=='e'&&usrbuf[3]=='r'){
//            if(strstr(usrbuf,"Chrome")){
//                req->browser_index=1;
//            }else if(strstr(usrbuf,"Firefox")){
//                req->browser_index=2;
//            }else if(strstr(usrbuf, "Version")){
//                req->browser_index=3;}
//            else{req->browser_index=4;}
//        }
//    }
//    // update recent browser data
//    int count =0;
//    printf("before write browser index.\n");
//    
//    if(strcmp("GET ", method)){
//        printf("Enter filter!\n");
//        if((fp=fopen("recent_browser.txt","r"))!=NULL){
//            printf("counting browser index.\n");
//            
//            while(fgets(index,4*MAXLINE, fp)!=NULL&&count<9){
//                printf("count: %d\n",count);
//                count++;
//                printf("%s\n",index);
//                //fgets(index,MAXLINE, fp);
//                sprintf(indextemp+strlen(indextemp), "%s",index);
//            }
//            fclose(fp);
//        }
//        fp = fopen("recent_browser.txt","w");
//        sprintf(temp, "%d\n",req->browser_index);
//        fputs(temp,fp);
//        fputs(indextemp,fp);
//        fclose(fp);
//        
//        memset(indextemp, 0, strlen(indextemp));
//        memset(index, 0, strlen(index));
//        memset(temp, 0, strlen(temp));
//        
//        ////////////
//        int count2=0;
//        if((ipfp=fopen("ip_address.txt","r"))!=NULL){
//            printf("counting IP\n");
//            
//            while(fgets(index,4*MAXLINE, fp)!=NULL&&count2<9){
//                printf("count: %d\n",count);
//                count2++;
//                printf("%s\n",index);
//                //fgets(index,MAXLINE, fp);
//                sprintf(indextemp+strlen(indextemp), "%s",index);
//            }
//            fclose(ipfp);
//        }
//        ipfp = fopen("ip_address.txt","w");
//        sprintf(temp, "%s\n",inet_ntoa(clientaddr->sin_addr));
//        fputs(temp,ipfp);
//        fputs(indextemp,ipfp);
//        fclose(ipfp);
//        
//        memset(indextemp, 0, strlen(indextemp));
//        memset(index, 0, strlen(index));
//        memset(temp, 0, strlen(temp));
//        
//        ////////////
//        
//        printf("Browser is %d\n",req->browser_index);
//        printf("IP is %s\n", inet_ntoa(clientaddr->sin_addr));
//        
//        sprintf(req->filename,".%s",uri);
//    }
//}

// parse request to get url
void parse_request(int fd, http_request *req){
    // Get the range start and end; Get the url;
    // Rio (Robust I/O) Buffered Input Functions
    rio_t rd;
    ssize_t n;
    char buffer[MAXLINE],
    method[MAXLINE] ,
    url[MAXLINE];
    char  request_head[50];
    char  browser[20];
    rio_readinitb(&rd,fd);
    if (rio_readlineb(&rd, buffer, MAXLINE) < 0) {   /*read buffer for the first line*/
        perror("Error reading buffer");
    }

    sscanf(buffer, "%s %s", method,url);    /*store method and url into two array*/
    if (strcmp(method, "GET") != 0) {         /*Only allow GET method*/
        printf("Requested method is not GET, is %s",method);
    }

//     read all
    int index = 0;
    while ((buffer[0] != '\n' || buffer[1]  != '\n' )&& index < 8)    {        /* iterate the buffer under two  cases: \n or \r\n, find the range*/
        memset(request_head, 0, sizeof(request_head));
        memset(browser, 0, sizeof(browser));
        memset(buffer, 0, sizeof(buffer));
        if ((n = rio_readlineb(&rd, buffer, MAXLINE))  < 0)  {       /*buffer length*/
            printf("Reading Buffer error");
            break;
        }
        int k = 0;
        while (k < n && buffer[k] != ' '){
            request_head[k] = buffer[k];
            k++;
        }
        printf("request head = %s fd = %d index  = %d\n ",request_head,fd,index);
        request_head[k] = '\0';
        if (strcmp(request_head,"User-Agent:") == 0) {   /*Current line includes browser info*/
            if (strstr(buffer, "Chrome") != NULL) {
                req->browser_index = 1;
            }
            else if(strstr(buffer, "Safari") != NULL) {
                req->browser_index = 2;
            }
            else if (strstr(buffer, "Firefox") != NULL) {
                req->browser_index = 3;
            }
            else if (strstr(buffer, "MSIE") != NULL) {
                req->browser_index  = 4;
            }
            else
                req->browser_index = 5;
        }
//        else if (strcmp(request_head,"Range:") == 0) {   /*Current line includes range*/
//            sscanf(buffer, "Range: bytes=%llu-%lu", &req->offset, &req->end);
//            printf("Range = %lu-%lu", req->offset, req->end);
//        }
//        if (req->end > 0) {
//            req->end ++;                                    /*Get request end*/
//        }
        
        index ++;
    }
    
//    rio_readlineb(&rio, buff, MAXLINE);
//    sscanf(buff, "%s %s %s", method, uri, version);
//    
//    rio_read(&rio, content, MAXLINE);
//    char *substring = "User-Agent:";
//    char * res = strstr(content, substring);
//    char browser[20];
//    for (int i = 0; i < 6; i++) {
//        char * b_type = strstr(res, browsers[i]);
//        if (b_type != NULL) {
//            memcpy(browser, browsers[i], strlen(browsers[i]));
//            break;
//        }
//    }
//    // update recent browser data
//    insertdeleteLine("home/recent_browser.txt", browser);
//    insertdeleteLine("home/ip_address.txt", inet_ntoa(clientaddr->sin_addr));

    // update recent browser data
    // decode url
    printf("url =%s\n",url);
    sprintf(req->filename,".%s",url);
    printf("I am in parse request after , fd = %d file name = %s\n",fd,req->filename);
    
}

// log files
void log_access(int status, struct sockaddr_in *c_addr, http_request *req){
    return;
}

// echo client error e.g. 404
void client_error(int fd, int status, char *msg, char *longmsg){   /*Write error message back*/
    char temp[MAXLINE];
    sprintf(temp, "HTTP/1.1 %d %s\r\n", status, msg);
    sprintf(temp + strlen(temp), "Content-length: %lu\r\n\r\n", strlen(longmsg)) ;   /*strlen won't calculate '\0'   must have \r\n\r\n at the end*/
    sprintf(temp + strlen(temp), "%s", longmsg);                /*Add long msg to buffer*/
    write(fd, temp, strlen(temp));
}

// serve static content
void serve_static(int out_fd, int in_fd, http_request *req,
                  size_t total_size){
    // send response headers to client e.g., "HTTP/1.1 200 OK\r\n"
    char temp[MAXLINE];
    char file[9999];
    char* type;
    type = get_mime_type(req -> filename);
//    sprintf(temp, "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n");
//    sprintf(temp + strlen(temp), "Cache-Control: no-cache\r\n");
//    sprintf(temp + strlen(temp), "Content-length: %u\r\n", req->end - req->offset);
//    sprintf(temp + strlen(temp), "Content-type: %s\r\n\r\n", type);

    sprintf(temp, "HTTP/1.1 200 OK\r\n");
    sprintf(temp, "%sContent-length: %lu\r\n", temp, total_size);
    sprintf(temp, "%sContent-type: %s\r\n\r\n", temp, type);

    memset(temp, 0, sizeof(temp));   /*important*/
    printf("I am in serve_static");
    read(in_fd, file, total_size);
    strcpy(temp, file);
    write(out_fd, temp, sizeof(temp));     /*send response body to client*/
    
//    char *filetype;
//    char buff[MAXLINE];
//    // send response headers to client e.g., "HTTP/1.1 200 OK\r\n"
//    filetype = get_mime_type(req->filename);
//    sprintf(buff, "HTTP/1.1 200 OK\r\n");
//    sprintf(buff, "%sContent-length: %lu\r\n", buff, total_size);
//    sprintf(buff, "%sContent-type: %s\r\n\r\n", buff, filetype);
//    written(out_fd, buff, strlen(buff));
//    
//    // send response body to client
//    memset(buff, 0, sizeof(buff));
//    while (read(in_fd, buff, MAXLINE) != 0) {
//        written(out_fd, buff, strlen(buff));
//        memset(buff, 0, strlen(buff));
//    }

}

//insert a line in the front
void insertdeleteLine(char *filename, char *data) {
    FILE *fin;
    char buff[MAXLINE];
    int num = 0;
    char tmp[20][MAXLINE];
    fin = fopen(filename, "r+");
    if (fin == NULL) {
        perror("Cannot open the file");
        exit(EXIT_FAILURE);
    }
    
    while (!feof(fin)) {
        fgets(buff, MAXLINE, fin);
        if (num == 0) {
            strcpy(tmp[num], data);
            tmp[num][strlen(data)] = '\n';
            tmp[num][strlen(data)+1] = '\0';
        }
        num++;
        strcpy(tmp[num], buff);
        memset(buff, 0, strlen(buff));
    }
    
    rewind(fin);
    for (int i = 0; i <= num; i++) {
        fputs(tmp[i], fin);
    }
    fclose(fin);
    if (num == 10) {
        int num1 = 0;
        memset(buff, 0, strlen(buff));
        char tmp2[20][MAXLINE];
        fin = fopen(filename, "r+");
        if (fin == NULL) {
            perror("Cannot open the file");
            exit(EXIT_FAILURE);
        }
        
        while (!feof(fin)) {
            fgets(buff, MAXLINE, fin);
            if (num1 != num) {
                strcpy(tmp2[num1], buff);
            }
            num1++;
        }
        fclose(fin);
        fin = fopen(filename, "w+");
        if (fin == NULL) {
            perror("Cannot open the file");
            exit(EXIT_FAILURE);
        }
        for (int i = 0; i < 9; i++) {
            fputs(tmp2[i], fin);
        }
        uint len = strlen(tmp2[9]);
        tmp2[9][len-1] = '\0';
        fputs(tmp2[9], fin);
        fclose(fin);
    }
}

// handle one HTTP request/response transaction
void process(int fd, struct sockaddr_in *clientaddr){

    printf("accept request, fd is %d, pid is %d\n", fd, getpid());
    http_request req;
    parse_request(fd, &req );

//    parse_request(fd, &req ,clientaddr);

    printf("I got in process, my fd is %d\n",fd);
    
    //

    // get logs out
    
//    char temp_1[100];
//    char temp_11[100];
//    char line_1[100];
//    memset(temp_1, 0, sizeof(temp_1));
//    memset(temp_11, 0, sizeof(temp_11));
//    memset(line_1,0,sizeof(line_1));
//    FILE* f1 = fopen("recent_browser.txt", "rt+");
//    int t1 = 0;
//    while (fgets(temp_1,100,f1) != NULL && t1 <9) {
//        strcat(temp_11,temp_1);
//        t1++;
//    }
//    fclose(f1);
//    FILE* f11 = fopen("recent_browser.txt", "w+	");
//    sprintf(line_1,"%d",req.browser_index);   /*Add the recent browser info*/
//    strcat(line_1,"\n");
//    strcat(line_1, temp_11);
//    fputs(line_1, f11);
//    fclose(f11);
//    printf ("The last 10 visited browsers: %s", line_1);
//
//    //  Get IP address in client addr and store it in  ip_address.txt
//    char temp_2[100];
//    char temp_22[100];
//    char line_2[100];
//    memset(temp_2,0, sizeof(temp_2));
//    memset(temp_22,0, sizeof(temp_22));
//    memset(line_2,0,sizeof(line_2));
//    FILE* f2 = fopen("ip_address.txt", "rt+	");
//    int t2 = 0;
//    while (fgets(temp_2,100,f2) != NULL && t2 <9) {
//        strcat(temp_22,temp_2);
//        t2++;
//    }
//    fclose(f2);
//    FILE* f22 = fopen("ip_address.txt", "w+	");
//    char* ip_addr = inet_ntoa(clientaddr->sin_addr);
//    strcpy(line_2, ip_addr);
//    strcat(line_2, "\n");
//    strcat(line_2, temp_22);
//    fputs(line_2,f22);
//    fclose(f22);
//    printf("The correspoinding IP address: %s", line_2);
//    
    struct stat sbuf;
    char * msg1 = "We haven't found what you requested.";
    char *msg2 = "Unknown Error occured.";
    int status = 200; //server status init as 200
    int ffd = open(req.filename, O_RDONLY, 0);
    printf("I am ready to get directory and static contents filename = %s \n",req.filename);
    
    if(ffd <= 0){
        // detect 404 error and print error log
        status = 404;
        client_error(fd, status, "Not found", msg1);        /*Return format:  HTTP 1.1 404 Not found \n Content-length: %u \r\n\r\n;*/
    } else {
        // get descriptor status
        fstat(ffd, &sbuf);
        if(S_ISREG(sbuf.st_mode)){
            // server serves static content
            printf("I am fetching static");
            serve_static(fd, ffd, &req, sbuf.st_size);
        } else if(S_ISDIR(sbuf.st_mode)){
            // server handle directory request
            status = 200;
            printf("I am fetching directory\n");
            handle_directory_request(fd, ffd, req.filename);
        } else {
            // detect 400 error and print error log
            status = 400;
            client_error(fd, status, "Error", msg2);
        }
        close(ffd);
    }
    // print log/status on the terminal
    log_access(status, clientaddr, &req);
}

// main function:
// get the user input for the file directory and port number
int main(int argc, char** argv){
    struct sockaddr_in clientaddr;
    int default_port = 9999,
    listenfd,
    connfd,
    pid;
    char buf[256];
    socklen_t clilent_size;
    int status;
    pid_t w;

//    struct sigaction sa;
//    sa.sa_handler = &handle_sigchld;
//    sigemptyset(&sa.sa_mask);
//    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
//    if (sigaction(SIGCHLD, &sa, 0) == -1) {
//        perror(0);
//        exit(1);
//    }

    listenfd = open_listenfd(default_port);
    printf("Run main");
    // get the name of the current working directory
    // user input checking
    // ignore SIGPIPE signal, so if browser cancels the request, it
    // won't kill the whole process.
    
    signal(SIGPIPE, SIG_IGN);
    while(1){
        // permit an incoming connection attempt on a socket.
        clilent_size = sizeof(struct sockaddr_in);
        connfd = accept(listenfd, (struct sockaddr_in*)&clientaddr, &clilent_size);
        printf(" connfd = %d", connfd);
        if (connfd < 0) {
            perror("Error on accepting here\n");
            exit(EXIT_FAILURE);
        }
        
        // fork children to handle parallel clients
        pid = fork();
        printf("run after fork pid = %d\n", pid);
        if (pid < 0){
            perror("Error on fork");
        }
        else if (pid == 0) {
            printf("Listen id = %d\n", listenfd);
            close(listenfd);
            process(connfd, &clientaddr);
            printf("connfd = %d is ready to exit",connfd);
            exit(0);
        }
        else {
            close(connfd);
        }
        // handle one HTTP request/response transaction
    }
    close(listenfd);          /*Close listening socket*/
    return 0;
}