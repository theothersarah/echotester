// Argument handling
#include <argp.h>

// Internet shit
#include <arpa/inet.h>
#include <netinet/in.h>

// errno
#include <errno.h>

// poll API
#include <poll.h>

// sscanf, printf, fprintf
#include <stdio.h>

// malloc, calloc, free, atexit, exit
#include <stdlib.h>

// strerror, memset, memcmp
#include <string.h>

// mmap, munmap
#include <sys/mman.h>

// socket, connect
#include <sys/socket.h>

// timerfd API
#include <sys/timerfd.h>

// wait
#include <sys/wait.h>

// fork, read, write, close
#include <unistd.h>

// *********************************************************************
// argp stuff for option parsing
// *********************************************************************

// argp globals (these must have these names)
const char* argp_program_version = "echotester 0.1";
//const char* argp_program_bug_address = "";

// argp documentation string
static char argp_doc[] = "Benchmark tool for TCP/IP echo servers";

// Constants for arguments
enum arg_keys
{
	KEY_ADDRESS = 'a',
	KEY_DURATION = 'd',
	KEY_LENGTH = 'l',
	KEY_PORT = 'p',
	KEY_WORKERS = 'w'
};

// argp options vector
static struct argp_option argp_options[] =
{
	{"address",		KEY_ADDRESS,	"STRING",		0,			"Address of echo server (default 127.0.0.1)"},
	{"duration",	KEY_DURATION,	"NUMBER",		0,			"Duration of test in seconds (default 60)"},
	{"length",		KEY_LENGTH,		"NUMBER",		0,			"Length of message in bytes (default 32)"},
	{"port",		KEY_PORT,		"NUMBER",		0,			"Network port to use (default 8080)"},
	{"workers",		KEY_WORKERS,	"NUMBER",		0,			"Number of worker processes (default 1)"},
	{0}
};

// Program arguments
struct arguments
{
	char* address;
	int duration;
	int length;
	unsigned short port;
	int workers;
};

// Argp option parser
static error_t argp_parse_options(int key, char* arg, struct argp_state* state)
{
	struct arguments* args = state->input;

	switch (key)
	{
	case KEY_ADDRESS:
		args->address = arg;
		break;
	case KEY_DURATION:
		sscanf(arg, "%i", &args->duration);
		break;
	case KEY_LENGTH:
		sscanf(arg, "%i", &args->length);
		break;
	case KEY_PORT:
		sscanf(arg, "%hu", &args->port);
		break;
	case KEY_WORKERS:
		sscanf(arg, "%i", &args->workers);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	
	return 0;
}

// *********************************************************************
// Shared memory allocation functions
//
// smalloc asks for a little extra memory so it can prepend a size,
// which is used by sfree when later being freed
// *********************************************************************
void* smalloc(size_t size)
{
	if (size == 0)
	{
		errno = EINVAL;
		return NULL;
	}
	
	size_t length = sizeof(size_t) + size;

	size_t* ptr = (size_t*)mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	if (ptr == MAP_FAILED)
	{
		return NULL;
	}

	*ptr = length;
	ptr++;

	return (void*)ptr;
}

void* scalloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
	{
		errno = EINVAL;
		return NULL;
	}
	
	size_t total = nmemb * size;
	
	if (total / nmemb != size)
	{
		errno = EOVERFLOW;
		return NULL;
	}
	
	return smalloc(total);
}

void sfree(void* ptr)
{
	if (ptr == NULL)
	{
		return;
	}

	size_t* base = (size_t*)ptr;
	base--;

	munmap((void*)base, *base);
}

// *********************************************************************
// These things are dynamically allocated and are freed at exit
// *********************************************************************

struct process_entry
{
	pid_t pid;
	int status;
};

struct process_entry* processTable = 0;
long* shared = 0;

void cleanup()
{
	if (processTable)
	{
		free(processTable);
	}
	
	if (shared)
	{
		sfree(shared);
	}
}

// *********************************************************************
// Task for worker processes
// *********************************************************************
void process(int id, struct arguments* args)
{
	int retval;
	
	// Initialize success counter in shared memory
	shared[id] = 0;
	
	// Allocate buffers
	size_t txBufferSize = args->length;
	char* txBuffer = malloc(txBufferSize);
	
	if (txBuffer == NULL)
	{
		fprintf(stderr, "Error: Cannot allocate transmit buffer for worker #%i: %s\n", id, strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	memset(txBuffer, 0x55, txBufferSize);
	
	// Receive buffer is twice as big just in case the host incorrectly sends a larger reply
	size_t rxBufferSize = args->length * 2;
	char* rxBuffer = malloc(rxBufferSize);
	
	if (rxBuffer == NULL)
	{
		fprintf(stderr, "Error: Cannot allocate receive buffer for worker #%i: %s\n", id, strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	// Set up socket address
	struct sockaddr_in addr = 
	{
		.sin_family = AF_INET,
		.sin_port = htons(args->port),
		.sin_addr =
		{
			.s_addr = htonl(INADDR_ANY)
		}
	};
	
	inet_pton(AF_INET, args->address, &addr.sin_addr.s_addr);
	
	// Set up timerfd
	struct itimerspec timer =
	{
		.it_interval =
		{
			.tv_sec = args->duration
		},
		
		.it_value =
		{
			.tv_sec = args->duration
		}
	};
	
	int timerfd = timerfd_create(CLOCK_REALTIME, 0);
	
	if (timerfd < 0)
	{
		fprintf(stderr, "Error: Cannot create timerfd for worker #%i: %s\n", id, strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	retval = timerfd_settime(timerfd, 0, &timer, NULL);
	
	if (retval < 0)
	{
		fprintf(stderr, "Error: Cannot set timerfd time for worker #%i: %s\n", id, strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	// Initialize poll list
	struct pollfd poll_list[2];
	
	poll_list[0].fd = timerfd;
	poll_list[0].events = POLLIN;
	
	while (1)
	{
		// Create socket
		int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
		
		if (sockfd < 0)
		{
			fprintf(stderr, "Error: Cannot create socket for worker #%i: %s\n", id, strerror(errno));
			exit(EXIT_FAILURE);
		}
		
		// Connect to the host
		retval = connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));
		
		if (retval < 0)
		{
			if (errno != EINPROGRESS)
			{
				fprintf(stderr, "Error: Worker #%i cannot connect to server: %s\n", id, strerror(errno));
				exit(EXIT_FAILURE);
			}
		}
		
		// Add socket to poll list
		poll_list[1].fd = sockfd;
		poll_list[1].events = POLLOUT;
		
		while (1)
		{
			retval = poll(poll_list, 2, -1);
			
			if (retval < 0)
			{
				fprintf(stderr, "Error: Worker #%i has problem polling: %s\n", id, strerror(errno));
				exit(EXIT_FAILURE);
			}
			
			// If timerfd ticks, we're done
			if (poll_list[0].revents & POLLIN)
			{
				close(sockfd);
				
				free(txBuffer);
				free(rxBuffer);
				
				return;
			}
			
			// Check socket state
			if (poll_list[1].revents & POLLOUT) // Ready to send message
			{
				ssize_t n = write(sockfd, txBuffer, txBufferSize);
				
				if (n < 0)
				{
					fprintf(stderr, "Error: Worker #%i has problem sending: %s\n", id, strerror(errno));
					exit(EXIT_FAILURE);
				}
				else if (n != txBufferSize)
				{
					fprintf(stderr, "Error: Worker #%i unable to send full string\n", id);
					exit(EXIT_FAILURE);
				}
				
				poll_list[1].events = POLLIN;
			}
			else if (poll_list[1].revents & POLLIN) // Reply waiting
			{
				ssize_t n = read(sockfd, rxBuffer, rxBufferSize);
				
				// Compare message size
				if (n < 0)
				{
					fprintf(stderr, "Error: Worker #%i has problem receiving: %s\n", id, strerror(errno));
					exit(EXIT_FAILURE);
				}
				else if (n != txBufferSize)
				{
					fprintf(stderr, "Error: Worker #%i has receive length mismatch\n", id);
					exit(EXIT_FAILURE);
				}
				
				// Compare message contents
				retval = memcmp(rxBuffer, txBuffer, txBufferSize);
				
				if (retval != 0)
				{
					fprintf(stderr, "Error: Worker #%i has receive content mismatch\n", id);
					exit(EXIT_FAILURE);
				}
				
				close(sockfd);
				
				// Record a successful result
				shared[id]++;
				
				break;
			}
		}
	}
}

// *********************************************************************
// Main function
// *********************************************************************
int main(int argc, char* argv[])
{
	// Set up cleanup function
	atexit(cleanup);
	
	// argp parser options
	struct argp argp_parser = {argp_options, argp_parse_options, 0, argp_doc};
	
	// Default argument values
	struct arguments args;
	args.address = "127.0.0.1";
	args.duration = 60;
	args.length = 32;
	args.port = 8080;
	args.workers = 1;

	// Parse arguments
	argp_parse(&argp_parser, argc, argv, 0, 0, &args);

	// Report argument values
	printf("Address: %s\n", args.address);
	printf("Port: %hu\n", args.port);
	printf("Duration: %i seconds\n", args.duration);
	printf("Length: %i bytes\n", args.length);
	printf("Workers: %i\n", args.workers);
	
	// Allocate a table to store process information in
	processTable = (struct process_entry*)calloc(args.workers, sizeof(struct process_entry));
	
	if (processTable == NULL)
	{
		fprintf(stderr, "Error: Cannot allocate memory for process table: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	// Allocate shared memory
	shared = (long*)scalloc(args.workers, sizeof(long));
	
	if (shared == NULL)
	{
		fprintf(stderr, "Error: Cannot allocate shared memory for results: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	// Process stuff
	int id;
	pid_t pid;

	// Spawn worker processes. For workers, the loop will break with id being the worker number.
	for (id = 0; id < args.workers; id++)
	{
		pid = fork();
	
		if (pid == (pid_t)0) // Worker
		{
			break;
		}
		else if (pid < (pid_t)0) // Error
		{
			fprintf(stderr, "Error: Cannot create worker process #%i: %s\n", id, strerror(errno));
			exit(EXIT_FAILURE);
		}
		else // Parent
		{
			// Store mapping between our internal id and the process id
			processTable[id].pid = pid;
			processTable[id].status = -1;
		}
	}

	// Tasks for each process
	if (pid == (pid_t)0) // Worker task
	{
		process(id, &args);
	}
	else // Parent task
	{
		printf("All worker processes dispatched, waiting for results\n");
		
		// Wait for workers to exit
		int status;
		
		while (pid = wait(&status), pid > 0)
		{
			// Search process table to find out which worker had just exited
			for (id = 0; id < args.workers; id++)
			{
				if (processTable[id].pid == pid)
				{
					if (status != 0)
					{
						fprintf(stderr, "Warning: Worker process #%i (PID %i) exited with status %i\n", id, pid, status);
					}
					
					processTable[id].status = status;

					break;
				}
			}
		}
		
		// Sum the results from workers that successfully exited
		long result = 0;
		int count = 0;
		
		for (id = 0; id < args.workers; id++)
		{
			if (processTable[id].status == 0)
			{
				result += shared[id];

				count++;
			}
		}

		// Report count of successes
		printf("%i process(es) exited successfully\n", count);
		
		if (count == 0)
		{
			fprintf(stderr, "Because no processes exited successfully, a result cannot be calculated\n");
			exit(EXIT_FAILURE);
		}

		// Do the final calculation and report the final result
		printf("Number of successful echoes: %li\n", result);
		printf("Rate of successful echoes: %li per second\n", result / args.duration);
	}
	
	exit(EXIT_SUCCESS);
}
