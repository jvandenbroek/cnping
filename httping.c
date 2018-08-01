#include "httping.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#ifdef WIN32
	#include <winsock2.h>
	#ifndef MSG_NOSIGNAL
	#define MSG_NOSIGNAL 0
	#endif

#else
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <netdb.h>
#endif

#include "os_generic.h"
#include "error_handling.h"


#define HTTPTIMEOUT 3.0

//Callbacks (when started/received)
void HTTPingCallbackStart( int seqno );
void HTTPingCallbackGot( int seqno );

//Don't dynamically allocate resources here, since execution may be stopped arbitrarily.
void DoHTTPing( const char * addy, double minperiod, int * seqnoptr, volatile double * timeouttime, int * socketptr, volatile int * getting_host_by_name )
{
	struct sockaddr_in serveraddr;
	struct hostent *server;
	int httpsock;
	int addylen = strlen(addy);
	char hostname[addylen+1];
	memcpy( hostname, addy, addylen + 1 );
	char * eportmarker = strchr( hostname, ':' );
	char * eurl = strchr( hostname, '/' );

	int portno = 80;

	(*seqnoptr) ++;
	HTTPingCallbackStart( *seqnoptr );

	if( eportmarker )
	{
		portno = atoi( eportmarker+1 );
		*eportmarker = 0;
	}
	else
	{
		if( eurl )
			*eurl = 0;
	}

#ifdef WIN32
	*socketptr = httpsock = WSASocket(AF_INET, SOCK_STREAM, 0, 0, 0, WSA_FLAG_OVERLAPPED);
	{
		//Setup windows socket for nonblocking io.
		unsigned long iMode = 1;
		ioctlsocket(httpsock, FIONBIO, &iMode);
	}
/*	{
		int lttl = 0xff;
		if (setsockopt(httpsock, IPPROTO_IP, IP_TTL, (const char*)&lttl, sizeof(lttl)) == SOCKET_ERROR)
		{
			printf( "Warning: No IP_TTL.\n" );
		}
	}*/
#else
	*socketptr = httpsock = socket(AF_INET, SOCK_STREAM, 0);
#endif
	if (httpsock < 0)
	{
		ERRM( "Error opening socket\n" );
		exit (1);
		return;
	}
	/* gethostbyname: get the server's DNS entry */
	*getting_host_by_name = 1;
	server = gethostbyname(hostname);
	*getting_host_by_name = 0;
	if (server == NULL) {
		ERRM("ERROR, no such host as %s\n", hostname);
		return;
	}

	/* build the server's Internet address */
	bzero((char *) &serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	memcpy((char *)&serveraddr.sin_addr.s_addr, (char *)server->h_addr, server->h_length);
	serveraddr.sin_port = htons(portno);

	/* connect: create a connection with the server */
	if (connect(httpsock, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
	{
		ERRM( "%s: ERROR connecting\n", hostname );
		exit ( 1 );
		return;
	}


	while( 1 )
	{
		char buf[8192];

		int n = sprintf( buf, "HEAD %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\n\r\n", eurl?eurl:"/favicon.ico", hostname );
		send( httpsock, buf, n, MSG_NOSIGNAL );
		double starttime = *timeouttime = OGGetAbsoluteTime();

		int endstate = 0;
		int breakout = 0;
		while( !breakout )
		{
			n = read( httpsock, buf, sizeof(buf)-1 );
			if( n < 0 ) return;
			int i;
			for( i = 0; i < n; i++ )
			{
				char c = buf[i];
				switch( endstate )
				{
				case 0: if( c == '\r' ) endstate++; break;
				case 1: if( c == '\n' ) endstate++; else endstate = 0; break;
				case 2: if( c == '\r' ) endstate++; else endstate = 0; break;
				case 3: if( c == '\n' ) breakout = 1; else endstate = 0; break;
				}
			}
		}

		*timeouttime = OGGetAbsoluteTime();

		HTTPingCallbackGot( *seqnoptr );

		double delay_time = minperiod - (*timeouttime - starttime);
		if( delay_time > 0 )
			usleep( (int)(delay_time * 1000000) );
		(*seqnoptr) ++;
		HTTPingCallbackStart( *seqnoptr );


	}
}


struct HTTPPingLaunch
{
	const char * addy;
	double minperiod;

	volatile double timeout_time;
	volatile int failed;
	int seqno;
	int socket;
	volatile int getting_host_by_name;
};

static void * DeployPing( void * v )
{
	struct HTTPPingLaunch *hpl = (struct HTTPPingLaunch*)v;
	hpl->socket = 0;
	hpl->getting_host_by_name = 0;
	DoHTTPing( hpl->addy, hpl->minperiod, &hpl->seqno, &hpl->timeout_time, &hpl->socket, &hpl->getting_host_by_name );
	hpl->failed = 1;
	return 0;
}


static void * PingRunner( void * v )
{
	struct HTTPPingLaunch *hpl = (struct HTTPPingLaunch*)v;
	hpl->seqno = 0;
	while( 1 )
	{
		hpl->timeout_time = OGGetAbsoluteTime();
		og_thread_t thd = OGCreateThread( DeployPing, hpl );
		while( 1 )
		{
			double Now = OGGetAbsoluteTime();
			double usl = hpl->timeout_time - Now + HTTPTIMEOUT;
			if( usl > 0 ) usleep( (int)(usl*1000000 + 1000));
			else usleep( 10000 );

			if( hpl->timeout_time + HTTPTIMEOUT < Now && !hpl->getting_host_by_name ) //Can't terminate in the middle of a gethostbyname operation otherwise bad things can happen.
			{
				if( hpl->socket )
				{
#ifdef WIN32
					closesocket( hpl->socket );
#else
					close( hpl->socket );
#endif
				}

				OGCancelThread( thd );
				break;
			}
		}
	}
	return 0;
}

int StartHTTPing( const char * addy, double minperiod )
{

#ifdef WIN32
	WSADATA wsaData;
	int r =	WSAStartup(MAKEWORD(2,2), &wsaData);
	if( r )
	{
		ERRM( "Fault in WSAStartup\n" );
		exit( -2 );
	}
#endif
	struct HTTPPingLaunch *hpl = malloc( sizeof( struct HTTPPingLaunch ) );
	hpl->addy = addy;
	hpl->minperiod = minperiod;
	OGCreateThread( PingRunner, hpl );
	return 0;
}



