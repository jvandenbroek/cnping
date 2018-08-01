//Copyright 2017 <>< C. Lohr, under the MIT/x11 License
//Rewritten from Sean Walton and Macmillan Publishers.
//Most of it was rewritten but the header was never updated.
//Now I finished the job.

#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "ping.h"
#include "os_generic.h"


#include "error_handling.h"

#ifdef WIN32
	#include <winsock2.h>
	#define SOL_IP		0
	#define F_SETFL		4
	#define ICMP_ECHO	8
	#define IP_TTL		2
	#define O_NONBLOCK   04000
	#pragma comment(lib, "Ws2_32.lib")
	#include <windows.h>
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <stdint.h>
#else
	#include <unistd.h>
	#include <sys/socket.h>
	#include <resolv.h>
	#include <netdb.h>
	#ifdef __APPLE__
		#ifndef SOL_IP
			#define SOL_IP IPPROTO_IP
		#endif
	#endif
	#include <netinet/ip.h>
	#include <netinet/ip_icmp.h>
#endif
#if defined WIN32 || defined __APPLE__
struct icmphdr
{
	uint8_t		type;
	uint8_t		code;
	uint16_t	checksum;
	union
	{
		struct
		{
			uint16_t	id;
			uint16_t	sequence;
		} echo;
		uint32_t	gateway;
		struct
		{
			uint16_t	__unused;
			uint16_t	mtu;
		} frag;
	} un;
};
#endif

float pingperiodseconds;
int precise_ping;
uint8_t notify;
static struct hostent *hname = NULL;

#define PACKETSIZE	65536

struct packet
{
	struct icmphdr hdr;
	unsigned char msg[PACKETSIZE-sizeof(struct icmphdr)];
};

int sd;
int pid=-1;
int ping_failed_to_send;
struct sockaddr_in psaddr;

uint16_t checksum( const unsigned char * start, uint16_t len )
{
	uint16_t i;
	const uint16_t * wptr = (uint16_t*) start;
	uint32_t csum = 0;
	for (i=1;i<len;i+=2)
		csum += (uint32_t)(*(wptr++));
	if( len & 1 )  //See if there's an odd number of bytes?
		csum += *(uint8_t*)wptr;
	if (csum>>16)
		csum = (csum & 0xFFFF)+(csum >> 16);
	//csum = (csum>>8) | ((csum&0xff)<<8);
	return ~csum;
}

void listener()
{
#ifndef WIN32
	const int val=255;

	int sd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);

	if ( setsockopt(sd, SOL_IP, IP_TTL, &val, sizeof(val)) != 0)
	{
		ERRM("Error: could not set TTL option  - did you forget to run as root or sticky bit cnping?\n");
			exit( -1 );
	}
#endif

	struct sockaddr_in addr;
	unsigned char buf[66000];
#ifdef WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
	for (;;)
	{
		socklen_t addrlenval=sizeof(addr);
		int bytes;

#ifdef WIN32
		WSAPOLLFD fda[1];
		fda[0].fd = sd;
		fda[0].events = POLLIN;
		WSAPoll(fda, 1, 10);
#endif
		keep_retry_quick:

		bytes = recvfrom(sd, buf, sizeof(buf), 0, (struct sockaddr*)&addr, &addrlenval );
		if( bytes == -1 ) continue;
		if( buf[20] != 0 ) continue; //Make sure ping response.
		if( buf[9] != 1 ) continue; //ICMP
		if( addr.sin_addr.s_addr != psaddr.sin_addr.s_addr ) continue;

		if ( bytes > 0 )
			display(buf + 28, bytes - 28 );
		else
    {
			ERRM("Error: recvfrom failed");
		}

		goto keep_retry_quick;
	}
	ERRM( "Fault on listen.\n" );
	exit( 0 );
}

void ping(struct sockaddr_in *addr )
{
#ifdef WIN32
	const char val=255;
#else
	const int val=255;
#endif
	int i, cnt=1;
	struct sockaddr_in r_addr;

#ifdef WIN32
	{
		//Setup windows socket for nonblocking io.
		unsigned long iMode = 1;
		ioctlsocket(sd, FIONBIO, &iMode);
	}
#else
	if ( fcntl(sd, F_SETFL, O_NONBLOCK) != 0 )
		ERRM("Warning: Request nonblocking I/O failed.");
#endif

	double stime = OGGetAbsoluteTime();

	struct packet pckt;
	do
	{
		int rsize = load_ping_packet( pckt.msg, sizeof( pckt.msg ) );
		memset( &pckt.hdr, 0, sizeof( pckt.hdr ) ); //This needs to be here, but I don't know why, since I think the struct is fully populated.

		pckt.hdr.code = 0;
		pckt.hdr.type = ICMP_ECHO;
		pckt.hdr.un.echo.id = pid;
		pckt.hdr.un.echo.sequence = cnt++;
		pckt.hdr.checksum = checksum((const unsigned char *)&pckt, sizeof( pckt.hdr ) + rsize );

		int sr = sendto(sd, (char*)&pckt, sizeof( pckt.hdr ) + rsize , 0, (struct sockaddr*)addr, sizeof(*addr));

		if( sr <= 0 )
		{
			ping_failed_to_send = 1;
		}
		else
		{
			ping_failed_to_send = 0;
		}

		if( precise_ping )
		{
			double ctime;
			do
			{
				ctime = OGGetAbsoluteTime();
				if( pingperiodseconds >= 1000 ) stime = ctime;
			} while( ctime < stime + pingperiodseconds );
			stime += pingperiodseconds;
		}
		else
		{
			if( pingperiodseconds > 0 )
			{
				if (pingperiodseconds < 1)
					OGUSleep( 1000000.0 * pingperiodseconds );
				else
				{
					for ( int i = 1; i <= pingperiodseconds; i++ )
						OGUSleep( 1000000.0 );
					OGUSleep( 1000000.0 * (float)(pingperiodseconds - (uint32_t)pingperiodseconds) );
				}
				notify = 1;
			}
		}
	} 	while( pingperiodseconds >= 0 );
	//close( sd ); //Hacky, we don't close here because SD doesn't come from here, rather  from ping_setup.  We may want to run this multiple times.
}

void ping_setup()
{
	pid = getpid();

#ifdef WIN32
	WSADATA wsaData;
	int r =	WSAStartup(MAKEWORD(2,2), &wsaData);
	if( r )
	{
		ERRM( "Fault in WSAStartup\n" );
		exit( -2 );
	}
#endif


#ifdef WIN32
	sd = WSASocket(AF_INET, SOCK_RAW, IPPROTO_ICMP, 0, 0, WSA_FLAG_OVERLAPPED);
	{
		int lttl = 0xff;
		if (setsockopt(sd, IPPROTO_IP, IP_TTL, (const char*)&lttl, sizeof(lttl)) == SOCKET_ERROR)
		{
			printf( "Warning: No IP_TTL.\n" );
		}
	}
#else
	const int val=255;

	sd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);

	if ( setsockopt(sd, SOL_IP, IP_TTL, &val, sizeof(val)) != 0)
	{
		ERRM("Error: Failed to set TTL option.  Are you root?  Or can do sock_raw sockets?\n");
		exit( -1 );
	}

#endif
	if ( sd < 0 )
	{
		ERRM("Error: Could not create raw socket\n");
		exit(0);
	}

}

void check_hostname ( const char *hostname )
{
	hname = gethostbyname(hostname);
	if (hname == NULL)
	{
		switch (h_errno)
		{
			case HOST_NOT_FOUND:
				ERRM("Error: The specified host is unknown.\n");
				break;
			case NO_DATA:
				ERRM("Error: The requested name is valid but does not have an IP address.\n");
				break;
			case NO_RECOVERY:
				ERRM("Error: A nonrecoverable name server error occurred.\n");
				break;
			case TRY_AGAIN:
				ERRM("Error: A temporary error occurred on an authoritative name server.\nTry again later..\n");
				break;
			default:
				ERRM("Error: An unknown error occured.\n");
		}
		exit(h_errno);
	}
}

void do_pinger()
{
	memset(&psaddr, 0, sizeof(psaddr));
	psaddr.sin_family = hname->h_addrtype;
	psaddr.sin_port = 0;
	psaddr.sin_addr.s_addr = *(long*)hname->h_addr;
	ping(&psaddr );
}

char errbuffer[1024];


