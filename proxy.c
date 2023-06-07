/*
 * A Web Proxy
 *
 * This program implements a multithreaded HTTP proxy.
 */

#include <assert.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

#include "csapp.h"

// #define MAXLINE 10
#define NITEMS 20
#define NTHREADS 10

/*
 * The struct information about a connected client.
 */
typedef struct
{
	struct sockaddr_in addr; // Socket address
	socklen_t addrlen;	 // address length
	int connfd;		 // Client connection file descriptor
	FILE *logger;		 // File logger
	char *port;		 // Port number
} client_info;

client_info *buffer[NITEMS];
int shared_cnt;
pthread_mutex_t mutex;
pthread_cond_t cond_empty;
pthread_cond_t cond_full;
unsigned int prod_index = 0;
unsigned int cons_index = 0;

static void client_error(int fd, const char *cause, int err_num,
    const char *short_msg, const char *long_msg);
static char *create_log_entry(const struct sockaddr_in *sockaddr,
    const char *uri, int size);
static int parse_uri(const char *uri, char **hostnamep, char **portp,
    char **pathnamep);
static int rio_readlineb_wrapper(rio_t *rp, char **bufp, int maxsize);
void proc_request(int connfd, struct sockaddr_in *clientaddr, FILE *logger);
void *consumer(void *arg);

/*
 * Requires:
 *	 A port number is given by the user to start listening.
 *
 * Effects:
 *   The program serves as a proxy for specifically the GET request.
 */
int main(int argc, char **argv)
{
	/* Check the arguments. */
	if (argc != 2)
	{
		fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
		exit(0);
	}

	/*
	 *Get rid of the broken pipe signals.
	 */

	Signal(SIGPIPE, SIG_IGN);

	int i;
	pthread_t cons_tids[NTHREADS];

	int listenfd = open_listenfd(argv[1]);
	if (listenfd < 0)
	{
		unix_error("open_listen error");
		return (-1);
	}

	/* Initialize pthread variables. */
	Pthread_mutex_init(&mutex, NULL);

	/* Initialize the condition variables. */
	pthread_cond_init(&cond_empty, NULL);
	pthread_cond_init(&cond_full, NULL);

	/* Start the producer and consumer threads. */
	for (i = 0; i < NTHREADS; ++i)
	{
		Pthread_create(&cons_tids[i], NULL, consumer, NULL);
	}

    /* Open the logger file.*/
	FILE *logger = fopen("proxy.log", "a+");

	while (true)
	{
		struct sockaddr_in clientaddr;
		socklen_t clientlen = sizeof(clientaddr);

		int connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
		if (connfd == -1)
		{
			fprintf(stderr, "Accept() error\n");
			continue;
		}

		/* Malloc struct to pass into thread. */
		client_info *info = malloc(sizeof(client_info));
		if (info == NULL)
		{
			fprintf(stderr, "Malloc failed!\n");
			Free(info);
			exit(1);
		}
		info->addr = clientaddr;
		info->addrlen = clientlen;
		info->connfd = connfd;
		info->port = argv[1];
		info->logger = logger;

		/* Push into the buffer */

		// Start the mutex lock.
		Pthread_mutex_lock(&mutex);
		while (shared_cnt == NITEMS)
		{
			/* Wait for signals to come.*/
			Pthread_cond_wait(&cond_empty, &mutex);
		}

		buffer[prod_index] = info;
		shared_cnt++;
		/* Send the buffer full signal. */
		Pthread_cond_signal(&cond_full);

		/* Update the producer index. */
		if (prod_index == NITEMS - 1)
			prod_index = 0;
		else
			prod_index++;

		// Release mutex lock.
		Pthread_mutex_unlock(&mutex);
	}

	/* Wait for all threads to finish. */
	for (i = 0; i < NTHREADS; ++i)
	{
		Pthread_join(cons_tids[i], NULL);
	}

	/* Clean up. */
	Pthread_mutex_destroy(&mutex);

	/* destroy the condition variables. */
	Pthread_cond_destroy(&cond_empty);
	Pthread_cond_destroy(&cond_full);

	/* Close the logger file. */
	Fclose(logger);

	/* Return success. */
	return (0);
}

/*
 * Requires:
 *    None.
 * Effects:
 *     Read and process a client from the buffer.
 */
void *consumer(void *arg)
{
	Pthread_detach(Pthread_self());
	client_info *info;
	//int id = (int)Pthread_self();
	//printf("Started consumer %d\n", id);

	while (true)
	{
		/* Start the mutex lock. */
		Pthread_mutex_lock(&mutex);

		while (shared_cnt == 0)
		{
			Pthread_cond_wait(&cond_full, &mutex);
		}
		/* Read from the buffer. */
		info = buffer[cons_index];
		shared_cnt--;
		if (shared_cnt == 0)
			Pthread_cond_signal(&cond_empty);

		/* Update the consumer index. */
		if (cons_index == NITEMS - 1)
			cons_index = 0;
		else
			cons_index++;

		/* Release mutex lock. */
		Pthread_mutex_unlock(&mutex);
		fprintf(stdout, "Received request.\n");

		/* Process the client request */
		proc_request(info->connfd, &info->addr, info->logger);
		/* Close the connection. */
		Close(info->connfd);
	}

	Free(arg);
	Free(info);
	return NULL;
}

/* Requires:
 *     A valid connfd, client address, and a logger.
 *
 * Effects:
 *     Read and process the request from the client, forward the request to the server
 *     and send response back to the client.
 */
void proc_request(int connfd, struct sockaddr_in *clientaddr, FILE *logger)
{
	rio_t rio;
	rio_t rio_sv;
	char *hostname;
	char *port;
	char *pathname;
	long n2;
	int n1;
	int nbytes;
	int serverfd;

	size_t requestlen = 0;

	/* Allocate buf to read line, and request to store the full request. */
	char *buf = malloc(MAXLINE * sizeof(char));
	if (buf == NULL)
	{
		Sio_puts("Out of memory. \n");
		Free(buf);
		_exit(1);
	}
	memset(buf, '\0', MAXLINE * sizeof(char));

	char *request = malloc(MAXLINE * sizeof(char));
	if (request == NULL)
	{
		Sio_puts("Out of memory. \n");
		Free(buf);
		Free(request);
		_exit(1);
	}
	memset(request, '\0', MAXLINE * sizeof(char));

	rio_readinitb(&rio, connfd);

	/* Read the request line after line. */
	if ((n1 = rio_readlineb_wrapper(&rio, &buf, MAXLINE)) > 0)
	{

		int buffer_size = (int)strlen(buf);
		char method[buffer_size];
		char uri[buffer_size];
		char version[buffer_size];
		char getline[buffer_size];

        /* Categorize the request message into three main parts. */
		sscanf(buf, "%s %s %s ", method, uri, version);

		/* Check the request method. */
		if (strcasecmp(method, "GET"))
		{
			client_error(connfd, method, 501, "Not Implemented",
			    "Proxy does not implement this method");
			Free(buf);
			Free(request);
			return;
		}

        /* Check the version of the request. */
		if (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))
		{
			client_error(connfd, version, 505, 
			    "HTTP Version Not Supported",
			    "Client requested an unsupported protocol version");
			Free(buf);
			Free(request);
			return;
		}

		/* Parse the uri. */
		if (parse_uri(uri, &hostname, &port, &pathname) == -1)
		{
			client_error(connfd, uri, 400, "Bad Request",
			    "Client requested a malformed URI");
			Free(buf);
			Free(request);
			return;
		}

		printf("Hostname: %s \n", hostname);
		// printf("Port: %s \n", port);

		snprintf(getline, buffer_size, "%s %s %s\r\n", method, 
		    pathname, version);

		requestlen += strlen(getline);
		if (requestlen > strlen(request))
			request = realloc(request, requestlen + 1);
		strcat(request, getline);

		printf("URI line is : %s \n", buf);
		printf("Start to parse headers \n");

		/* Call the wrapper function below to read lines. */
		while ((nbytes = rio_readlineb_wrapper(&rio, &buf, MAXLINE)) > 
		    0)
		{
			if (strcmp(buf, "\r\n") == 0)
			{
				printf("End of header section.\n");
				break;
			}
			else
			{
				/* Skip these headers to prevent failure. */
				if ((strncmp(buf, "Connection", 10) == 0) |
				    (strncmp(buf, "Keep-Alive", 10) == 0) |
				    (strncmp(buf, "Proxy-Connection", 16) == 0))
				{
					continue;
				}
				else
				{
					requestlen += strlen(buf);
					if (requestlen > strlen(request))
					{
						request = realloc(request, 
						    requestlen + 1);
					}
					strcat(request, buf);
				}
			}
		}

		if (strncmp(version, "HTTP/1.1", 8) == 0)
		{
			requestlen += strlen("Connection: close\r\n");
			if (requestlen > strlen(request))
				request = realloc(request, requestlen + 1);
			strcat(request, "Connection: close\r\n");
		}

		requestlen += strlen("\r\n");
		if (requestlen > strlen(request))
			request = realloc(request, requestlen + 1);
		strcat(request, "\r\n");

		// printf("Request is : %s \n", request);

		/* Connect to the server. */
		if ((serverfd = open_clientfd(hostname, port)) < 0)
		{
			client_error(connfd, hostname, 504, "Gateway Timeout",
			    "Unrecognized host name or port");
			fprintf(stderr, "Error connecting to the server\n");
			Free(buf);
			Free(request);
			Free(hostname);
			Free(port);
			Free(pathname);
			return;
		}
		fprintf(stdout, "Successfully connect to the server.\n");

		/* Forward the request to the server. */
		rio_readinitb(&rio_sv, serverfd);
		if ((rio_writen(serverfd, request, strlen(request))) < 0)
		{
			client_error(connfd, hostname, 504, "Gateway Timeout",
			    "Error sending headers to");
			fprintf(stderr, "Error in writing headers to server!\n"
			    );
			fprintf(stderr, "Error is in closing line. \n");
			Free(buf);
			Free(request);
			Free(hostname);
			Free(port);
			Free(pathname);
			return;
		}

		Free(buf);
		Free(request);

		long size = 0;
		char *server_buf = malloc((MAXLINE + 64) * sizeof(char));
		if (server_buf == NULL)
		{
			Sio_puts("Out of memory");

			Free(server_buf);
			Free(hostname);
			Free(port);
			Free(pathname);
			exit(1);
		}

		/* Read from the server and write back to the client. */
		while ((n2 = rio_readnb(&rio_sv, server_buf, MAXLINE)) > 0)
		{
			size += n2;
			if (rio_writen(connfd, server_buf, n2) < 0)
			{
				client_error(connfd, "client", 504,
				    "Gateway Timeout", 
				    "Error sending response to");
				fprintf(stderr, 
				    "Error sending request to client\n");

				Free(server_buf);
				Free(hostname);
				Free(port);
				Free(pathname);
				return;
			}
		}

		Free(server_buf);

		rio_writen(connfd, "\r\n", 2);

		if ((n2 < 0))
		{
			fprintf(stderr, 
			    "Error reading response from end server\n");
			client_error(connfd, hostname, 504, "Gateway Timeout",
			    "Error reading from");
			Free(hostname);
			Free(port);
			Free(pathname);
			return;
		}
		else if (size == 0)
		{
			Free(hostname);
			Free(port);
			Free(pathname);
			return;
		}

		/* Generate the log message and write to the log file. */
		char *log_msg = create_log_entry(clientaddr, uri, size);
		printf("%s\n", log_msg);
		fprintf(logger, "%s\n", log_msg);
		fflush(logger);

		Free(hostname);
		Free(port);
		Free(pathname);
		Free(log_msg);

		Close(serverfd);
	}

	else
	{
		client_error(connfd, "client", 504, "Gateway Timeout",
		    "Error reading request from");
		Free(buf);
		Free(request);
		return;
	}
}

/*
 * Requires:
 * 	 bufp points to a valid char pointer.
 *
 * Effects:
 *   Read a line from rp that may exceed the maxsize length. 
 *   Return the number of bytes read if success or return -1 if fails.
 */
static int
rio_readlineb_wrapper(rio_t *rp, char **bufp, int maxsize)
{
	int n_count = 0;
	int n_read;
	char *linebrk;
	char buf_new[maxsize];

	memset(*bufp, '\0', strlen(*bufp));
	memset(buf_new, '\0', sizeof(buf_new));

	/* Use buf_new as a new buffer to store the temp read. */
	if ((n_read = rio_readlineb(rp, buf_new, maxsize)) < 0)
	{
		fprintf(stderr, "Error reading request client\n");
		return (-1);
	}
	else
	{
		n_count += n_read;
		strcat(*bufp, buf_new);

		while ((linebrk = strstr(*bufp, "\r\n")) == NULL)
		{
			
			if ((n_read = rio_readlineb(rp, buf_new, maxsize)) < 0)
			{
				fprintf(stderr, 
				    "Error reading request client\n");
				return (-1);
			}

			/* Add to the count and reallocate if necessary. */
			n_count += n_read;
			if ((size_t)n_count > strlen(*bufp))
			{
				*bufp = (char *)realloc((*bufp), n_count + 1);
			}
			strcat(*bufp, buf_new);

		}
	}

	/* Return the number of bytes read. */
	return (n_count);
}

/*
 * Requires:
 *   The parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Given a URI from an HTTP proxy GET request (i.e., a URL), extract the
 *   host name, port, and path name.  Create strings containing the host name,
 *   port, and path name, and return them through the parameters "hostnamep",
 *   "portp", "pathnamep", respectively.  (The caller must free the memory
 *   storing these strings.)  Return -1 if there are any problems and 0
 *   otherwise.
 */
static int
parse_uri(const char *uri, char **hostnamep, char **portp, char **pathnamep)
{
	const char *pathname_begin, *port_begin, *port_end;

	if (strncasecmp(uri, "http://", 7) != 0)
		return (-1);

	/* Extract the host name. */
	const char *host_begin = uri + 7;
	const char *host_end = strpbrk(host_begin, ":/ \r\n");
	if (host_end == NULL)
		host_end = host_begin + strlen(host_begin);
	int len = host_end - host_begin;
	char *hostname = Malloc(len + 1);
	strncpy(hostname, host_begin, len);
	hostname[len] = '\0';
	*hostnamep = hostname;

	/* Look for a port number.  If none is found, use port 80. */
	if (*host_end == ':')
	{
		port_begin = host_end + 1;
		port_end = strpbrk(port_begin, "/ \r\n");
		if (port_end == NULL)
			port_end = port_begin + strlen(port_begin);
		len = port_end - port_begin;
	}
	else
	{
		port_begin = "80";
		port_end = host_end;
		len = 2;
	}
	char *port = Malloc(len + 1);
	strncpy(port, port_begin, len);
	port[len] = '\0';
	*portp = port;

	/* Extract the path. */
	if (*port_end == '/')
	{
		pathname_begin = port_end;
		const char *pathname_end = strpbrk(pathname_begin, " \r\n");
		if (pathname_end == NULL)
			pathname_end = pathname_begin + strlen(pathname_begin);
		len = pathname_end - pathname_begin;
	}
	else
	{
		pathname_begin = "/";
		len = 1;
	}
	char *pathname = Malloc(len + 1);
	strncpy(pathname, pathname_begin, len);
	pathname[len] = '\0';
	*pathnamep = pathname;

	return (0);
}

/*
 * Requires:
 *   The parameter "sockaddr" must point to a valid sockaddr_in structure.  The
 *   parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Returns a string containing a properly formatted log entry.  This log
 *   entry is based upon the socket address of the requesting client
 *   ("sockaddr"), the URI from the request ("uri"), and the size in bytes of
 *   the response from the server ("size").
 */
static char *
create_log_entry(const struct sockaddr_in *sockaddr, const char *uri, int size)
{
	struct tm result;

	/*
	 * Create a large enough array of characters to store a log entry.
	 * Although the length of the URI can exceed MAXLINE, the combined
	 * lengths of the other fields and separators cannot.
	 */
	const size_t log_maxlen = MAXLINE + strlen(uri);
	char *const log_str = Malloc(log_maxlen + 1);

	/* Get a formatted time string. */
	time_t now = time(NULL);
	int log_strlen = strftime(log_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z: ",
	    localtime_r(&now, &result));

	/*
	 * Convert the IP address in network byte order to dotted decimal
	 * form.
	 */
	Inet_ntop(AF_INET, &sockaddr->sin_addr, &log_str[log_strlen],
	    INET_ADDRSTRLEN);
	log_strlen += strlen(&log_str[log_strlen]);

	/*
	 * Assert that the time and IP address fields occupy less than half of
	 * the space that is reserved for the non-URI fields.
	 */
	assert(log_strlen < MAXLINE / 2);

	/*
	 * Add the URI and response size onto the end of the log entry.
	 */
	snprintf(&log_str[log_strlen], log_maxlen - log_strlen, " %s %d", uri,
	    size);

	return (log_str);
}

/*
 * Requires:
 *   The parameter "fd" must be an open socket that is connected to the client.
 *   The parameters "cause", "short_msg", and "long_msg" must point to properly
 *   NUL-terminated strings that describe the reason why the HTTP transaction
 *   failed.  The string "short_msg" may not exceed 32 characters in length,
 *   and the string "long_msg" may not exceed 80 characters in length.
 *
 * Effects:
 *   Constructs an HTML page describing the reason why the HTTP transaction
 *   failed, and writes an HTTP/1.0 response containing that page as the
 *   content.  The cause appearing in the HTML page is truncated if the
 *   string "cause" exceeds 2048 characters in length.
 */
static void
client_error(int fd, const char *cause, int err_num, const char *short_msg,
			 const char *long_msg)
{
	char body[MAXBUF], headers[MAXBUF], truncated_cause[2049];

	assert(strlen(short_msg) <= 32);
	assert(strlen(long_msg) <= 80);
	/* Ensure that "body" is much larger than "truncated_cause". */
	assert(sizeof(truncated_cause) < MAXBUF / 2);

	/*
	 * Create a truncated "cause" string so that the response body will not
	 * exceed MAXBUF.
	 */
	strncpy(truncated_cause, cause, sizeof(truncated_cause) - 1);
	truncated_cause[sizeof(truncated_cause) - 1] = '\0';

	/* Build the HTTP response body. */
	snprintf(body, MAXBUF,
			 "<html><title>Proxy Error</title><body bgcolor="
			 "ffffff"
			 ">\r\n"
			 "%d: %s\r\n"
			 "<p>%s: %s\r\n"
			 "<hr><em>The COMP 321 Web proxy</em>\r\n",
			 err_num, short_msg, long_msg, truncated_cause);

	/* Build the HTTP response headers. */
	snprintf(headers, MAXBUF,
			 "HTTP/1.0 %d %s\r\n"
			 "Content-type: text/html\r\n"
			 "Content-length: %d\r\n"
			 "\r\n",
			 err_num, short_msg, (int)strlen(body));

	/* Write the HTTP response. */
	if (rio_writen(fd, headers, strlen(headers)) != -1)
		rio_writen(fd, body, strlen(body));
}
