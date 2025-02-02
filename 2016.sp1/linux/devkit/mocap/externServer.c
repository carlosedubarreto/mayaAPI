/*
//-
// ==========================================================================
// Copyright 1995,2006,2008 Autodesk, Inc. All rights reserved.
//
// Use of this software is subject to the terms of the Autodesk
// license agreement provided at the time of installation or download,
// or which otherwise accompanies this software in either electronic
// or hard copy form.
// ==========================================================================
//+
*/

#include <stdio.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <sys/types.h>
#ifndef _WIN32
#include <sys/time.h>
#include <sys/socket.h>
#else
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#endif
#include <sys/stat.h>
#ifndef _WIN32
#include <getopt.h>
#endif
#ifdef IRIX
#include <bstring.h>
#endif
#include <fcntl.h>
#include "channelParse.h"

#ifndef FALSE
#	define FALSE 0
#endif
#ifndef TRUE
#	define TRUE 1
#endif

/*
 * Missing prototypes
 */

#ifndef _WIN32
#include <maya/mocapserver.h>
#include <maya/mocapserial.h>
#else
#include <maya/mocapserver.h>
#include <maya/mocapserial.h>
#endif

#ifdef  LINUX
#define GETOPTHUH '?'
#define FNONBLK O_NONBLOCK
#endif  /* LINUX */

#ifdef _WIN32

#define PATH_MAX _MAX_PATH

char *   optarg = NULL;
int	    optind = 1;
int	    opterr = 0;
int     optlast = 0;
#define GETOPTHUH 0

int getopt(int argc, char **argv, char *pargs)
{
	if (optind >= argc) return EOF;

	if (optarg==NULL || optlast==':')
	{
		optarg = argv[optind];
		if (*optarg!='-' && *optarg!='/')
			return EOF;
	}

	if (*optarg=='-' || *optarg=='/') optarg++;
	pargs = strchr(pargs, *optarg);
	if (*optarg) optarg++;
	if (*optarg=='\0')
	{
		optind++;
		optarg = NULL;
	}
	if (pargs == NULL) return 0;  //error
	if (*(pargs+1)==':')
	{
		if (optarg==NULL)
		{
			if (optind >= argc) return EOF;
			// we want a second paramter
			optarg = argv[optind];
		}
		optind++;
	}
	optlast = *(pargs+1);

	return *pargs;
}

int gettimeofday(struct timeval* tp)
{
	unsigned int dw = timeGetTime();
	tp->tv_sec = dw/1000;
	tp->tv_usec = dw%1000;

	return 0;
}

#else
#define closesocket close
#endif


#ifndef lengthof
#define lengthof(array)	(sizeof(array) / sizeof(array[0]))
#endif /* lengthof */

static int gDaemonMode = 0;
static int gInetdMode  = 0;
static int gShowUsage  = 0;
static int gVerbose    = 0;
static int gDebugMode  = 0;
static int gReadReopen = FALSE;
static int gReadRewind = FALSE;
static int gReadNext   = FALSE;
static int gBufferSize = 4096;
static float	gMinRecordRate = 1.;
static float	gMaxRecordRate = 300.;
static float	gDefRecordRate = 60.;

static channelInfo *gChannels = NULL;
static char gProgramName[PATH_MAX] = { 0 };
static char gConfigFile[PATH_MAX]  = { 0 };
static char gServerName[PATH_MAX]  = { 0 };
static char gDataPath[PATH_MAX]    = { 0 };
static FILE *gDataFile = NULL;
static char *gBuffers[2] = { NULL, NULL };
static int  gReadBuffer	 = 0;
static int  gBufferOffset= 0;
static void parseArgs(int argc, char **argv);
static void printUsage();
static void nonBlockFifo(int client_fd);
static int handle_client(int client_fd);
static void get_data(int client_fd);
static channelInfo * create_channels(char *config, int client_fd);


double toDouble(const struct timeval t) {
	return (double)(t.tv_sec) + (double)(t.tv_usec) / 1000000.0;
}

struct timeval	toTimeval(double sec)
{
	struct timeval tim;

	tim.tv_sec  = sec;
	tim.tv_usec = (sec - tim.tv_sec) * 1000000;

	return tim;
}


int main(int argc, char **argv)
{

	int status;
	int client_fd = -1;

	/*
	 * on err, this exits...
	 */
	parseArgs( argc, argv);


	/*
	 * Initialization...
	 */
	gBuffers[0] = malloc(gBufferSize);
	gBuffers[1] = malloc(gBufferSize);
	if ( ( NULL == gBuffers[0] ) || ( NULL == gBuffers[1] ) ) {
		CapError(-1, CAP_SEV_FATAL, gProgramName, 
				 "out of memory, -b %d failed",gBufferSize);
		exit(1);
	}
	*(gBuffers[0]) = '\0';
	*(gBuffers[1]) = '\0';


	if (gInetdMode) {
		/*
		 * The socket to the client is open on file descriptor 0.
		 * Handle one client and then exit.
		 */
		client_fd = 0;
	}

	if (gDaemonMode) {
		/*
		 * Convert this process into a standard unix type daemon process.
		 * It will be running in the background with no controlling terminal
		 * and using syslog to report error messages.
		 */
		status = CapDaemonize();
		if (status < 0)	{
			CapError(-1, CAP_SEV_FATAL, gProgramName, NULL);
			exit(1);
		}
	}

	/*
	 * open the configuration and data files
	 */
    gChannels = create_channels(gConfigFile, client_fd);
	if ( NULL == gChannels ) {
		CapError(-1, CAP_SEV_FATAL, gProgramName, NULL);
		exit(1);
	}

	if ( 0 == strcmp(gDataPath,"-") ) {
		gDataFile = stdin;
	} else {
		/*
		 * RFE: delay opening until first read
		 */
		gDataFile = fopen(gDataPath, "r");
		if ( !gReadReopen && (NULL == gDataFile) ) {
			CapError(-1, CAP_SEV_FATAL, gProgramName, NULL);
			exit(1);
		}
	}

	if ( NULL != gDataFile ) 
		nonBlockFifo(-1);
			
	if (gInetdMode)
	{
		/*
		 * The socket to the client is already open on file descriptor 0.
		 * Handle one client and then exit.
		 */
		status = handle_client(client_fd);
		exit(status);
	}
	else
	{
		while (1)
		{
			/*
			 * Set up the server socket and wait for a connection.
			 */
			client_fd = CapServe(gServerName);
			if (client_fd < 0)
			{
				CapError(-1, CAP_SEV_FATAL, gProgramName, NULL);
				exit(1);
			}
      
			/* Handle client requests */
			status = handle_client(client_fd);

			if (status < 0)
			{
				CapError(-1, CAP_SEV_FATAL, gProgramName, NULL);
			}

			/* Shutdown the client */
			closesocket(client_fd);
			client_fd = -1;

			/* Go back and wait for another connection request */
			continue;
		}
	}
}

static int handle_client(int client_fd)
{
	int status;
	size_t size, nitems;
	FILE *serial_file;
	char buf[1024];	/* buffer for serial IO */
	CapCommand cmd;
	char ruser[64], rhost[64], realhost[64];
	double rate, time;
	fd_set rd_fds;
	struct timeval timeout, now, *timeoutPtr;
	int    recording = 0;
	double recordNow    = 0.;
	double recordNext   = 0.;
	double recordPeriod = 0.;
	double recordDelta  = 0.;
#ifdef _WIN32
	int ms;
#endif

	while (1)
	{
		FD_ZERO(&rd_fds);
		FD_SET(client_fd, &rd_fds);

		if ( !recording ) {
			timeoutPtr = NULL;
		} else {
			/* record and schedule the next recording */
#ifdef LINUX
			gettimeofday(&now, NULL);
#else
			gettimeofday(&now);
#endif
			recordNow   = toDouble(now);
			if ( recordNext == 0.0 ) /* The first record */
				recordNext = recordNow;

			while ( recordNext <= recordNow ) {
				get_data(client_fd);
				recordNext = recordNext + recordPeriod;
			}
			
			timeout = toTimeval( recordNext - recordNow );
			timeoutPtr = &timeout;
		}

		/*
		 * wait for commands or a timeout
		 */
#ifndef _WIN32
		status = select(FD_SETSIZE, &rd_fds, NULL, NULL, timeoutPtr);
#else
		/* Should really use CapWaitTimeout when possible */
		ms = -1;
		if (timeoutPtr)
			ms = timeoutPtr->tv_sec*1000 + timeoutPtr->tv_usec/1000;
		status = CapWaitTimeout(client_fd, ms);
#endif


		if (status < 0 ) {
			if (errno == EINTR) {
				/* Ignore signals and try again */
				continue;
			}

			/* Otherwise, give a fatal error message */
			CapError(client_fd, CAP_SEV_FATAL, gProgramName, "select failed");
			CapError(client_fd, CAP_SEV_FATAL, "select", NULL);
			exit(1);
		}
		else if (status == 0) {
			/* Try again */
			continue;
		}

		/* There is data on the client file descriptor */
		cmd = CapGetCommand(client_fd);
		switch (cmd) {
		case CAP_CMD_QUIT:
			return 0;

		case CAP_CMD_AUTHORIZE:
			status = CapGetAuthInfo(client_fd, ruser, rhost, realhost);
			if (status < 0)
			{
				return -1;
			}

			/*
			 * If user@host is not authorized to use this server then:
			 *
			 * status = CapAuthorize(client_fd, 0);
			 */
			status = CapAuthorize(client_fd, 1);
			break;

		case CAP_CMD_INIT:	/* Initial client/server handshake */
			status = CapInitialize(client_fd, gProgramName);
			break;

		case CAP_CMD_VERSION:	/* Send version information */
			status = CapVersion(client_fd, gProgramName, "1.0",
								"Extern server (example) - v1.0");
			break;

		case CAP_CMD_INFO:
			if (NULL == gChannels)
			{
				/* Only create the channel data once */
				gChannels = create_channels(gConfigFile, client_fd);
				if ( NULL == gChannels)
				{
					status = CapError(client_fd, CAP_SEV_ERROR, 
									  gProgramName,
									  "Missing or empty config file");  
				}
			}
			/* Return the recording information. gMaxRecordRate <= 0.
			 * says recording is not supported
			 */
			{
				size_t buf_size = 0;
				if ( 0. < gMaxRecordRate) 
					buf_size = INT_MAX;
				status = CapInfo(client_fd, gMinRecordRate, 
								 gMaxRecordRate, gDefRecordRate, 
								 buf_size, 1);
			}
			break;
	  
		case CAP_CMD_DATA:	/* Send frame data */
			if (!recording )
				get_data(client_fd);
			status = CapData(client_fd);
			break;


		case CAP_CMD_START_RECORD:	/* Start recording */
			rate = CapGetRequestedRecordRate(client_fd);
			size = CapGetRequestedRecordSize(client_fd);

			/* 
			 * Set up for recording operations
			 */
			if (rate < gMinRecordRate ) 
				rate = gMinRecordRate;
			else if (rate > gMaxRecordRate ) 
				rate = gMaxRecordRate;

			status = CapStartRecord(client_fd, rate, size);
			if (status != -1)
			{
				recordPeriod = 1.0 / rate;
				recordNext   = 0.;
				recording = 1;
			}
			break;

		case CAP_CMD_STOP_RECORD:		/* Stop recording */
			status = CapStopRecord(client_fd);
			recording = 0;
			break;

		default:			/* Ignore unknown commands */
			status = CapError(client_fd, CAP_SEV_ERROR, gProgramName,
							  "Unknown server command.");
			break;
		}

		if (status < 0)
		{
			return -1;
		}
		
	}

	/* return 0; */
}

static void get_data(int client_fd)
{

	channelInfo *chan = gChannels;
	char *tokBuffer;
	char *lp;
	int i;
	char *errOverFlow =
		"Input buffer overflow line length greater than -b %d";

	if ( gReadReopen ) {
		if ( NULL != gDataFile )
			fclose(gDataFile);

		gDataFile = fopen(gDataPath, "r");

		if (NULL == gDataFile) {
			CapError(client_fd, CAP_SEV_FATAL, gProgramName, NULL);
			exit(1);
		}

		nonBlockFifo(client_fd);
	} else if ( gReadRewind )
		rewind(gDataFile);


	/*
	 * find the data record
	 */
	while ( lp = fgets(gBuffers[gReadBuffer] + gBufferOffset,
					   gBufferSize - gBufferOffset, gDataFile) )
	{
		gBufferOffset = strlen(lp);
				
		if ( '\n' == lp[gBufferOffset-1] ) {
			/* This completes a full record */
			gReadBuffer   = 1 - gReadBuffer;
			gBufferOffset = 0;
			/*
			 * RFE: add support for leading character "comments"
			 */
			if (gReadNext)
				break;
		} else if ( gBufferSize <= gBufferOffset ) {
			CapError(client_fd, CAP_SEV_FATAL, gProgramName, 
					 errOverFlow, gBufferSize );
			exit(1);
		}
	}

	/*
	 * gReadBuffer is the buffer to read or in process 1-grb is complete!
	 */
	tokBuffer = gBuffers[1 - gReadBuffer];
	while (chan != NULL) {
		float val;
		channelInfo *next = chan->next;
		for ( i = 0; i< chan->info->p1.dim; i++) {
			char *tok = strtok(tokBuffer," \n\t");
			tokBuffer = NULL; /* how strtok works ;-o */
			if ( NULL == tok || ( 1 != sscanf(tok,"%g", &val) )) {
				next = NULL;
				break;
			}
			chan->data[i] = val;
		}
		chan = next;
	}


	channelInfoSetData(gChannels, 1, NULL);

	return;
}


static channelInfo *create_channels(char *config, int client_fd)
{
	channelInfo *channels = NULL;
	FILE	*channel_file;
	char  *channel_file_name = getenv("MAYA_EXTERN_SERVER_CFG");

	/* figure out a filename to use */
	if ( (NULL == config)  || (strlen(config) < 1) ) {
		if ( NULL == channel_file_name )
			channel_file_name = "externServer.cfg";
	} else 
		channel_file_name = config;

	channel_file = fopen(channel_file_name, "r");
  

	channels = channelInfoCreate(channel_file, 1, NULL);					   
	
	if ( NULL != channel_file )
		fclose(channel_file);
	return channels;
}

void printUsage()
{
    fprintf(stderr, "Usage:\n");
#ifdef DEVEL
    fprintf(stderr, 
			"    %s [-hdNRrvDi] [-b size] [-c config] [-n name] [-f file]\n",
			gProgramName);
#else /* DEVEL */
    fprintf(stderr, 
			"    %s [-hdNRr] [-b size] [-c config] [-n name] [-f file]\n", 
			gProgramName);
#endif /* DEVEL */
    fprintf(stderr, "\n");
    fprintf(stderr, "        -h        Print this help message\n");
    fprintf(stderr, "        -d        Run as a daemon in the background\n");
	fprintf(stderr, "        -N        read next record, instead of last\n");
	fprintf(stderr, "        -r        rewind file on reread\n");
	fprintf(stderr, "        -R        reopen file on reread\n");
	fprintf(stderr, "        -t        minimum record frequency in Hz\n");
	fprintf(stderr, "        -T        maximum record frequency in Hz\n");
	fprintf(stderr, "        -H        default record frequency in Hz\n");

	
#ifdef DEVEL
    fprintf(stderr, "        -i        Run from inetd\n");
    fprintf(stderr, "        -D        Set the debug flag\n");
    fprintf(stderr, "        -v        Set the vebose flag\n");
#endif /* DEVEL */
	fprintf(stderr, "        -b	size   max line length for input file\n");
 	fprintf(stderr, "        -c config Use <config> as the config file\n");
	fprintf(stderr, "        -f file   file to read (default is stdin)\n");
	fprintf(stderr, "        -n name   Set the UNIX socket name to name\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Defaults:\n");
    fprintf(stderr, "    %s -b %d\n", gProgramName, 4096);
    fprintf(stderr, "    %s -c %s.cfg\n", gProgramName, gProgramName);
    fprintf(stderr, "    %s -n %s\n", gProgramName, gProgramName);
    fprintf(stderr, "\nDefault input file is stdin\n");
    fprintf(stderr, "\n");

    exit(1);
}

void parseArgs(int argc, char **argv)
{
	int opt;
	char *cptr;
	float optFloat;
	/*
	 * Grab a copy of the program name
	 */
#ifdef _WIN32
		_splitpath (argv[0], NULL, NULL, gProgramName, NULL);
#else
	cptr = strrchr(argv[0], '/');
	if (cptr)
	{
		strcpy(gProgramName, (cptr + 1));
	}
	else
	{
		strcpy(gProgramName, argv[0]);
	}
#endif

	/*
	 * Set default values
	 */
	gReadReopen = FALSE;
	gReadRewind = FALSE;
	gReadNext   = FALSE;
	gBufferSize = 4096;
	gDaemonMode = 0;
	gShowUsage  = 0;
	strcpy(gConfigFile, gProgramName);
	strcat(gConfigFile,".cfg");
	strcpy(gDataPath,"-");
	strcpy(gServerName, gProgramName);

	/*
	 * Parse the options
	 */
#ifdef DEVEL
	while ((opt = getopt(argc, argv, "hdNRrb:c:f:n:t:T:H:Div")) != -1)
#else /* DEVEL */
		while ((opt = getopt(argc, argv, "hdNRrb:c:f:n:t:T:H:")) != -1)
#endif /* DEVEL */
		{
			switch (opt)
			{
			case 'h':
				gShowUsage++;
				break;

			case 'd':
				gDaemonMode++;
				break;

			case 'N' :
				gReadNext  = TRUE;
				break;

			case 'R' :
				gReadReopen = TRUE;
				break;

			case 'r' :
				gReadRewind = TRUE;
				break;

			case 'b' :
				gBufferSize = atoi(optarg);
				break;

			case 'c' :
				strcpy(gConfigFile, optarg);
				break;

			case 'f' :
				strcpy(gDataPath, optarg);
				break;

			case 'n':
				strcpy(gServerName, optarg);
				break;

			case 't':
				if ( 1 == sscanf( optarg ,"%f", &optFloat) ) {
					gMinRecordRate = optFloat;
				} else {
					gShowUsage++;
				}
				break;

			case 'T':
				if ( 1 == sscanf( optarg ,"%f", &optFloat) ) {
					gMaxRecordRate = optFloat;
				} else {
					gShowUsage++;
				}
				break;

			case 'H':
				if ( 1 == sscanf( optarg ,"%f", &optFloat) ) {
					gDefRecordRate = optFloat;
				} else {
					gShowUsage++;
				}
				break;

				/* DEVEL FLAGS */

			case 'D':
				gDebugMode++;
				break;

			case 'i':
				gInetdMode++;
				break;

			case 'v':
				gVerbose++;
				break;

			case GETOPTHUH:
				gShowUsage++;
			}
		}


	/*
	 * Check for errors
     *
	 * KL: RFE: Should stat file type for "IFREG" and make sure that
	 *      gReadReopen and gReadRewind make sense here.
	 */  
	
	if ( ( 0 == strcmp(gDataPath,"-") ) &&
		 ( gReadReopen || gReadRewind || gDaemonMode) ) {
		fprintf(stderr, "\nInvalid options for read from standard input.\n");
		gShowUsage++;	
	}

	if (gDaemonMode && gInetdMode) 
		gShowUsage++;

	if (optind < argc) 
		gShowUsage++;

	if ( gBufferSize < 1 )
		gShowUsage++;

	if (gShowUsage) 
		printUsage();

}

#ifdef _WIN32
#define S_IFIFO _S_IFIFO
#endif

void nonBlockFifo(int clientFd) 
/*
 * See if the input is a fifo, and setup the fcntl correctly
 */
{
	struct stat info;
	int flags;
	int dataFd = fileno(gDataFile);
	fstat(dataFd, &info);
	if ( info.st_mode & S_IFIFO ) {
		if ( gReadRewind ) {
			CapError(clientFd, CAP_SEV_FATAL, gProgramName, 
					 "Cannot -r (rewind) FIFO");
			exit(1);
		}
#ifndef _WIN32
		flags = fcntl(dataFd,F_GETFL);
		fcntl(dataFd,F_SETFL,flags|FNONBLK);
#endif
	}
}
