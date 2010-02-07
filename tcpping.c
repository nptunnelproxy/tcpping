/*
Copyright (c) 2004, Steven Kehlet
Copyright (c) 2010, Jim Wyllie
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the
   distribution.
3. The names of Steven Kehlet or Jim Wylliemay not be used to endorse
   or promote products derived from this software without specific prior
   written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <pcap.h>
#include <sys/ioctl.h>
#include <net/ethernet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#ifdef linux
#define __FAVOR_BSD
#endif
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <libnet.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>

#define tcp_flag_isset(tcpptr, flag) (((tcpptr->th_flags) & (flag)) == (flag))

int timeout = 2;
char *myname;
pid_t child_pid;
int keep_going = 1;
int verbose = 0;
int notify_fd;
struct timeval tv_syn, tv_synack;
int sequence_offset = 0;
char *dest_name;
in_addr_t dest_ip = 0;
u_short dest_port = 80;

float min_ping = -1;
float avg_ping = 0;
float max_ping = 0;
int total_syns = 0;
int total_synacks = 0;
int total_rsts = 0;
int successful_pings = 0;

/* Global handle to libnet -- libnet1 requires only one instantiation per process */
libnet_t *l;
libnet_ptag_t tcp_pkt;
libnet_ptag_t ip_pkt;

void handle_sigalrm(int junk)
{
    /* do nothing */
}

/* wait for child to exit so the user's prompt doesn't
   come back before the stats */
void handle_sigint(int junk)
{
    waitpid(child_pid, NULL, 0);
    libnet_destroy(l);
    exit(0);
}

void print_stats(int junk)
{
    printf("\n");
    
    printf("--- %s TCP ping statistics ---\n", dest_name);
    total_syns = (total_syns != 0 ? total_syns : 1);
    printf("%d SYN packets transmitted, %d SYN/ACKs and %d RSTs received, %.1f%% packet loss\n", 
           total_syns, total_synacks, total_rsts, 
           (1 - (successful_pings*1.0/total_syns))*100);
    printf("round-trip min/avg/max = %.3f/%.3f/%.3f ms\n",
           min_ping, avg_ping, max_ping);

    exit(0);
}

char *inet_ntoa2(in_addr_t addr)
{
    struct in_addr iaddr;
    iaddr.s_addr = addr;
    return inet_ntoa(iaddr);
}

void showPacket(struct ip *ip, struct tcphdr *tcp)
{
    int r;
    struct timeval tv;
    char flags[32];

    r = gettimeofday(&tv, NULL);
    if (r < 0) {
        perror("gettimeofday");
        exit(1);
    }
    snprintf(flags, sizeof(flags), "[%s%s%s%s%s%s]", 
             (tcp_flag_isset(tcp, TH_FIN) ? "F" : ""),
             (tcp_flag_isset(tcp, TH_SYN) ? "S" : ""),
             (tcp_flag_isset(tcp, TH_RST) ? "R" : ""),
             (tcp_flag_isset(tcp, TH_PUSH) ? "P" : ""),
             (tcp_flag_isset(tcp, TH_ACK) ? "A" : ""),
             (tcp_flag_isset(tcp, TH_URG) ? "U" : "")
             );
    printf("%ld.%ld", tv.tv_sec, tv.tv_usec);
    printf(" %s:%d", inet_ntoa(ip->ip_src), ntohs(tcp->th_sport));
    printf(" -> %s:%d", inet_ntoa(ip->ip_dst), ntohs(tcp->th_dport));
    printf(" %s\n", flags);
}

/* callback to pcap_loop() */
void got_packet(u_char *args, const struct pcap_pkthdr *header, const u_char *packet)
{
    int r;
    struct ether_header *ethernet;
    struct ip *ip;
    struct tcphdr *tcp;
    u_char *payload;
    float ms;
    char *units = "ms";
    char *flags;

    int size_ethernet = sizeof(struct ether_header);
    int size_ip = sizeof(struct ip);
    int size_tcp = sizeof(struct tcphdr);

    ethernet = (struct ether_header*)(packet);
    ip = (struct ip*)(packet + size_ethernet);
    tcp = (struct tcphdr*)(packet + size_ethernet + size_ip);
    payload = (u_char *)(packet + size_ethernet + size_ip + size_tcp);

    if (verbose) {
        showPacket(ip, tcp);
    }

    if (ip->ip_dst.s_addr == dest_ip && tcp_flag_isset(tcp, TH_SYN)) {
        /* SYN from us */
        r = gettimeofday(&tv_syn, NULL);
        if (r < 0) {
            perror("gettimeofday");
            exit(1);
        }
        total_syns++;

    } else if (ip->ip_src.s_addr == dest_ip &&
               ((tcp_flag_isset(tcp, TH_SYN) && tcp_flag_isset(tcp, TH_ACK)) || 
                tcp_flag_isset(tcp, TH_RST))) {
        /* SYN/ACK or RST from the other guy */
        r = gettimeofday(&tv_synack, NULL);
        if (r < 0) {
            perror("gettimeofday");
            exit(1);
        }

        ms = (tv_synack.tv_sec - tv_syn.tv_sec) * 1000;
        ms += (tv_synack.tv_usec - tv_syn.tv_usec)*1.0/1000;

        if (ms > 1000) {
            units = "s";
            ms /= 1000;
        }

        if (tcp_flag_isset(tcp, TH_SYN)) {
            flags = "SYN/ACK";
            total_synacks++;
        } else {
            flags = "RST";
            total_rsts++;
        }

        printf("%s from %s: seq=%u ttl=%d time=%.3f%s\n", 
               flags,
               inet_ntoa(ip->ip_src), 
               (ntohl(tcp->th_ack) - sequence_offset)/100,
               ip->ip_ttl,
               ms, units);

        if (ms < min_ping || min_ping == -1) {
            min_ping = ms;
        }
        if (ms > max_ping) {
            max_ping = ms;
        }
        
        avg_ping = ((avg_ping * successful_pings) + ms)/(successful_pings+1);
        successful_pings++;

        /* tell parent to continue */
        write(notify_fd, "foo", 3);
    }
}



void sniffPackets(char *devName)
{
     int r;
     pcap_t *handle;
     char errbuf[PCAP_ERRBUF_SIZE];
     char filterExpr[1024];
     struct bpf_program filter;
     bpf_u_int32 mask;
     bpf_u_int32 net;

     r = pcap_lookupnet(devName, &net, &mask, errbuf);
     if (r < 0) {
         fprintf(stderr, "pcap_lookupnet: %s\n", errbuf);
         exit(1);
     }
     
     handle = pcap_open_live(devName, BUFSIZ, 1, 0, errbuf);
     if (!handle) {
         fprintf(stderr, "pcap_open_live: %s\n", errbuf);
         exit(1);
     }

     /* set non-blocking */
#ifdef BIOCIMMEDIATE
     r = 1;
     ioctl(pcap_fileno(handle), BIOCIMMEDIATE, &r);
#else
     r = pcap_setnonblock(handle, 1, errbuf);
#endif
     if (r < 0) {
         fprintf(stderr, "pcap_lookupnet: %s\n", errbuf);
         exit(1);
     }

     /* compile and apply the filterExpr */
     snprintf(filterExpr, sizeof(filterExpr), 
              "host %s and port %u", inet_ntoa2(dest_ip), dest_port);
     r = pcap_compile(handle, &filter, filterExpr, 0, mask);
     if (r < 0) {
         fprintf(stderr, "pcap_compile: %s\n", pcap_geterr(handle));
         exit(1);
     }

     r = pcap_setfilter(handle, &filter);
     if (r < 0) {
         fprintf(stderr, "pcap_setfilter: %s\n", pcap_geterr(handle));
         exit(1);
     }

     /* wake up parent, we're ready */
     write(notify_fd, "foo", 3);

     /* begin sniffing */
     r = pcap_loop(handle, -1, got_packet, NULL);

     pcap_close(handle);
}

/* use libnet to determine what device we'll be using to get to
 * dest_ip 
 * WARNING:  This function will wreck your libnet stack! we
 * therefore only call it in the child process */
char *findDevice()
{
    libnet_ptag_t t;
    char *deviceName;

    t = libnet_autobuild_ipv4(
        LIBNET_IPV4_H + LIBNET_TCP_H, /* length */
        IPPROTO_TCP,                  /* protocol */
        dest_ip,                      /* destination IP */
        l);                           /* libnet handle */
    if (t == -1) {
        fprintf(stderr, "libnet_autobuild_ipv4: %s\n", libnet_geterror(l));
        exit(1);
    }

    deviceName = strdup((char *)libnet_getdevice(l));

    return deviceName;
}

void injectSYNPacket(int sequence)
{
    int c;

    /* custom TCP header */
    /* we use the sequence to number the packets */
    tcp_pkt = libnet_build_tcp(
        random() % 65536,                 /* source port */
        dest_port,                        /* destination port */
        sequence_offset + (sequence*100), /* sequence number */
        0,                                /* acknowledgement num */
        TH_SYN,                           /* control flags */
        32768,                            /* window size */
        0,                                /* checksum */
        0,                                /* urgent pointer */
        LIBNET_TCP_H,                     /* TCP packet size */
        NULL,                             /* payload */
        0,                                /* payload size */
        l,                                /* libnet handle */
        tcp_pkt);                         /* libnet packet ref */
    if (tcp_pkt == -1) {
        fprintf(stderr, "libnet_build_tcp: %s\n", libnet_geterror(l));
        exit(1);
    }

    /* custom IP header; I couldn't get autobuild_ipv4 to work */
    ip_pkt = libnet_build_ipv4(
         LIBNET_IPV4_H + LIBNET_TCP_H,        /* packet length */
         0,                                   /* tos */
	 htons((l->ptag_state) & 0x0000ffff), /* IP id */
	 0,                                   /* fragmentation */
	 64,                                  /* TTL */
	 IPPROTO_TCP,                         /* encap protocol */
	 0,                                   /* checksum */
	 libnet_get_ipaddr4(l),               /* source IP */
	 dest_ip,                             /* destination IP */
         NULL,                                /* payload */
	 0,                                   /* payload size */
	 l,                                   /* libnet pointer */
	 ip_pkt);                             /* libnet packet ref */
    if (ip_pkt == -1) {
        fprintf(stderr, "libnet_autobuild_ipv4: %s\n", libnet_geterror(l));
        exit(1);
    }

    /* send it */
    c = libnet_write(l);
    if (c == -1) {
        fprintf(stderr, "libnet_write: %s\n", libnet_geterror(l));
        exit(1);
    }
}

void usage()
{
    fprintf(stderr, "%s: [-v] [-c count] [-p port] [-i interval] [-I interface] [-W timeout] remote_host\n", myname);
    exit(0);
}

int main(int argc, char *argv[])
{
    int r;
    int c;
    char *deviceName = NULL;
    int count = -1;
    int interval = 1;
    struct hostent *he;
    int pipefds[2];
    char junk[256];
    int sequence = 1;
    int timed_out = 0;

    myname = argv[0];

    while ((c = getopt(argc, argv, "c:p:i:vI:W:")) != -1) {
        switch (c) {
            case 'c':
                count = atoi(optarg);
                break;
            case 'p':
                dest_port = atoi(optarg);
                break;
            case 'i':
                interval = atoi(optarg);
                break;
            case 'I':
                deviceName = optarg;
                break;
            case 'v':
                verbose = 1;
                break;
            case 'W':
                timeout = atoi(optarg);
                break;
            default:
                usage();
        }
    }

    argc -= optind;
    argv += optind;

    if (argc != 1) {
        usage();
    }

    if (geteuid() != 0) {
        fprintf(stderr, "You must run %s as root.\n", myname);
        exit(1);
    }

    he = gethostbyname(argv[0]);
    if (!he) {
        herror("gethostbyname");
        exit(1);
    }
    if (!he->h_addr) {
        fprintf(stderr, "No address associated with name: %s\n", argv[0]);
        exit(1);
    }
    bcopy(he->h_addr, &dest_ip, sizeof(dest_ip));
    if (dest_ip == INADDR_NONE) {
        perror("bad address");
        exit(1);
    }

    /* set up the libnet pointer and stack */
    char errbuf[LIBNET_ERRBUF_SIZE];

    l = libnet_init(LIBNET_RAW4, NULL, errbuf);
    if (l == NULL) {
        fprintf(stderr, "libnet_init: %s", errbuf);
        exit(1); 
    }

    dest_name = he->h_name;
    printf("TCP PING %s (%s:%u)\n", dest_name, 
           inet_ntoa2(dest_ip), dest_port);

    /* start seq# somewhere random so we're not SO obvious */
    srandom(time(NULL));
    sequence_offset = random();

    /* pipe is to synchronize with our child */
    r = pipe(pipefds);
    if (r < 0) {
        perror("pipe");
        exit(1);
    }

    child_pid = fork();
    if (child_pid < 0) {
        perror("fork");
        exit(1);
    }

    if (child_pid) {
        /* parent */
        close(pipefds[1]);

        /* wait for child sniffer to be ready */
        r = read(pipefds[0], junk, sizeof(junk));

        signal(SIGINT, handle_sigint);
        signal(SIGALRM, handle_sigalrm);
        /* stop read() from restarting upon SIGALRM */
        siginterrupt(SIGALRM, 1);

        for (;;) {
            injectSYNPacket(sequence++);

            /* wait for child to receive response */
            timed_out = 0;
            alarm(timeout);
            r = read(pipefds[0], junk, sizeof(junk));
            if (r == 0) {
                /* child died */
                fprintf(stderr, "child exited.\n");
                exit(1);
            } else if (r < 0) {
                printf("Timed out.\n");
                timed_out = 1;
            }
            alarm(0);

            if (--count == 0) {
                /* tell child to display stats */
                kill(child_pid, SIGINT);
                /* make sure we wait until it died and closed */
                kill(getpid(), SIGINT);
                break;
            }

            if (timed_out) {
                timed_out = interval - timeout;
                if (timed_out > 0) {
                    sleep(timed_out);
                }
            } else {
                sleep(interval);
            }
        }

    } else {
        /* child */
        close(pipefds[0]);
        notify_fd = pipefds[1];

        signal(SIGINT, print_stats);

	/* Find the name of the device to listen on */
	if (!deviceName) {
	    deviceName = findDevice();
	}

        sniffPackets(deviceName);
    }

    return(0);
}
