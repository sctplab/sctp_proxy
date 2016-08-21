/*-
 * Copyright (c) 2016 Michael Tuexen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#ifdef __linux__
#include <sys/select.h>
#endif
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * proxy -i 10
 *       -o 10
 *       -4
 *       -6
 *       -L 127.0.0.1,::1,10.10.10.10:5001  -> sctp_bindx()
 *       -X 127.0.0.1,::1                   -> sctp_bindx()
 *       -S 127.0.0.1,::1:5678              -> sctp_connectx()
 *       -S 127.0.0.1,::1:4567              -> sctp_connectx()
 *
 * The following is preserved:
 * - message boundaries
 * - ordered / unordered
 * - sid/ssn
 * - ppid
 *
 * Supported platforms:
 * - FreeBSD    cc -pthread -o proxy proxy.c
 * - Linux      gcc -pthread -o proxy proxy.c -lsctp 
 * - Solaris:   gcc -o proxy proxy.c -lnsl -lsocket -lsctp 
 */

#define LISTEN_QUEUE        10
#define INITIAL_BUF_SIZE 10240

struct server_info {
	struct sockaddr *server_addrs;
	int number_addrs;
};

struct proxy_info {
	int ipv4only;
	int ipv6only;
	int client_fd;                  /* Used for the client <-> proxy association */
	int server_fd;                  /* Used for the proxy <-> server association */
	struct sockaddr *local_addrs;   /* Used for the proxy <-> server association */
	int number_local_addrs;
	struct sockaddr *server_addrs;  /* Used for the proxy <-> server association */
	int number_server_addrs;
	char *client_buf;               /* Used for messages received from the client */
	size_t client_buf_size;
	size_t client_buf_offset;
	char *server_buf;               /* User for messages received from the server */
	size_t server_buf_size;
	size_t server_buf_offset;
};

static void *
proxy(void *arg)
{
	struct proxy_info *info;
	fd_set rset;
	int max_fd, flags;
	ssize_t n;
	struct sctp_sndrcvinfo sndrcvinfo;
	struct sctp_status status;
	struct sctp_initmsg initmsg;
	socklen_t len;

	pthread_detach(pthread_self());
	info = (struct proxy_info *)arg;

	/* First, get the number of streams used by the client */
	len = (socklen_t)sizeof(struct sctp_status);
	if (getsockopt(info->client_fd, IPPROTO_SCTP, SCTP_STATUS, &status, &len) < 0) {
		fprintf(stderr, "proxy: Can't get number of streams: %s\n", strerror(errno));
		goto cleanup;
	}
	/* Second allocate message buffers for the client and server */
	info->client_buf_size = INITIAL_BUF_SIZE;
	info->client_buf_offset = 0;
	info->client_buf = malloc(info->client_buf_size);
	if (info->client_buf == NULL) {
		fprintf(stderr, "proxy: Can't allocate memory for messages from the client\n");
		goto cleanup;
	}
	info->server_buf_size = INITIAL_BUF_SIZE;
	info->server_buf_offset = 0;
	info->server_buf = malloc(info->server_buf_size);
	if (info->server_buf == NULL) {
		fprintf(stderr, "proxy: Can't allocate memory for messages from the server\n");
		goto cleanup;
	}
	/* Now establish an SCTP association with the server */
	if ((info->server_fd = socket(info->ipv4only ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_SCTP)) < 0) {
		fprintf(stderr, "proxy: Can't open a socket: %s\n", strerror(errno));
		goto cleanup;
	}
	if (!info->ipv4only) {
		if (setsockopt(info->server_fd, IPPROTO_IPV6, IPV6_V6ONLY, (const void *)&info->ipv6only, (socklen_t)sizeof(int)) < 0) {
			fprintf(stderr, "proxy: Can't set IPV6 mode: %s\n", strerror(errno));
			goto cleanup;
		}
	}
	memset(&initmsg, 0, sizeof(struct sctp_initmsg));
	initmsg.sinit_num_ostreams = status.sstat_instrms;
	initmsg.sinit_max_instreams = status.sstat_outstrms;
	if (setsockopt(info->server_fd, IPPROTO_SCTP, SCTP_INITMSG, (const void *)&initmsg, (socklen_t)sizeof(struct sctp_initmsg)) < 0) {
		fprintf(stderr, "proxy: Can't set the number of streams: %s.\n", strerror(errno));
		goto cleanup;
	}
	if (sctp_bindx(info->server_fd, info->local_addrs, info->number_local_addrs, SCTP_BINDX_ADD_ADDR) < 0) {
		fprintf(stderr, "proxy: Can't bind local addresses: %s\n", strerror(errno));
		goto cleanup;
	}
	if (sctp_connectx(info->server_fd, info->server_addrs, info->number_server_addrs, NULL) < 0) {
		fprintf(stderr, "proxy: Can't connect to server: %s\n", strerror(errno));
		goto cleanup;
	}
	/* Time for message relaying */
	for (;;) {
		FD_ZERO(&rset);
		max_fd = -1;
		FD_SET(info->client_fd, &rset);
		if (info->client_fd > max_fd) {
			max_fd = info->client_fd;
		}
		FD_SET(info->server_fd, &rset);
		if (info->server_fd > max_fd) {
			max_fd = info->server_fd;
		}
		select(max_fd + 1, &rset, NULL, NULL, NULL);
		if (FD_ISSET(info->client_fd, &rset)) {
			flags = 0;
			n = sctp_recvmsg(info->client_fd,
			                 info->client_buf + info->client_buf_offset,
			                 info->client_buf_size - info->client_buf_offset,
			                 NULL, NULL,
			                 &sndrcvinfo,
			                 &flags);
			if (n <-0) {
				fprintf(stderr, "proxy: sctp_recvmsg: %s\n", strerror(errno));
				goto cleanup;
			} else if (n == 0) {
				goto cleanup;
			} else {
				info->client_buf_offset += n;
				if (flags & MSG_EOR) {
					n = sctp_sendmsg(info->server_fd,
					                 info->client_buf,
					                 info->client_buf_offset,
					                 NULL, 0,
					                 sndrcvinfo.sinfo_ppid,
					                 sndrcvinfo.sinfo_flags & SCTP_UNORDERED,
					                 sndrcvinfo.sinfo_stream,
					                 0, 0);
					if (n < 0) {
						fprintf(stderr, "proxy: sctp_sendmsg: %s\n", strerror(errno));
						goto cleanup;
					} else {
						info->client_buf_offset = 0;
					}
				} else {
					if (info->client_buf_offset == info->client_buf_size) {
						info->client_buf_size *= 2;
						info->client_buf = realloc(info->client_buf, info->client_buf_size);
						if (info->client_buf == NULL) {
							fprintf(stderr, "proxy: Can't re-allocate memory for messages from the client\n");
							goto cleanup;
						}
					}
				}
			}
		}
		if (FD_ISSET(info->server_fd, &rset)) {
			flags = 0;
			n = sctp_recvmsg(info->server_fd,
			                 info->server_buf + info->server_buf_offset,
			                 info->server_buf_size - info->server_buf_offset,
			                 NULL, NULL,
			                 &sndrcvinfo,
			                 &flags);
			if (n <-0) {
				fprintf(stderr, "proxy: sctp_recvmsg: %s\n", strerror(errno));
				goto cleanup;
			} else if (n == 0) {
				goto cleanup;
			} else {
				info->server_buf_offset += n;
				if (flags & MSG_EOR) {
					n = sctp_sendmsg(info->client_fd,
					                 info->server_buf,
					                 info->server_buf_offset,
					                 NULL, 0,
					                 sndrcvinfo.sinfo_ppid,
					                 sndrcvinfo.sinfo_flags & SCTP_UNORDERED,
					                 sndrcvinfo.sinfo_stream,
					                 0, 0);
					if (n < 0) {
						fprintf(stderr, "proxy: sctp_sendmsg: %s\n", strerror(errno));
						goto cleanup;
					} else {
						info->server_buf_offset = 0;
					}
				} else {
					if (info->server_buf_offset == info->server_buf_size) {
						info->server_buf_size *= 2;
						info->server_buf = realloc(info->server_buf, info->server_buf_size);
						if (info->server_buf == NULL) {
							fprintf(stderr, "proxy: Can't re-allocate memory for messages from the client\n");
							goto cleanup;
						}
					}
				}
			}
		}
	}
cleanup:
	/* Free all resources */
	if (info->server_fd >= 0) {
		close(info->server_fd);
	}
	if (info->client_fd >= 0) {
		close(info->client_fd);
	}
	free(info->client_buf);
	free(info->server_buf);
	free(info);
	return (NULL);
}

static int
parse_addrs_list(char *addr_list, char *port, int ipv4only, int ipv6only, struct sockaddr **addrs)
{
	struct addrinfo hints, *res;
	char *addr;
	char *tmp;
	size_t size;
	int number_local_addrs;

	if ((addr_list == NULL) || (port == NULL)) {
		return (-1);
	}
	number_local_addrs = 1;
	for (tmp = addr_list; *tmp != '\0'; tmp++) {
		if (*tmp == ',') {
			number_local_addrs++;
		}
	}
	if (ipv4only) {
		size = number_local_addrs * sizeof(struct sockaddr_in);
	} else {
		size = number_local_addrs * sizeof(struct sockaddr_in6);
	}
	*addrs = (struct sockaddr *)malloc(size);
	memset(&hints, 0, sizeof(hints));
	if (ipv4only) {
		hints.ai_family = AF_INET;
	} else {
		hints.ai_family = AF_INET6;
	}
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_SCTP;
	hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
	if (!ipv4only && !ipv6only) {
		hints.ai_flags |= AI_V4MAPPED;
	}
	tmp = (char *)*addrs;
	for (addr = strtok(addr_list, ","); addr != NULL; addr = strtok(NULL, ",")) {
		if (getaddrinfo(addr, port, &hints, &res) != 0) {
			return (-1);
		}
		memcpy(tmp, res->ai_addr, res->ai_addrlen);
		tmp += res->ai_addrlen;
		freeaddrinfo(res);
	}
	return (number_local_addrs);
}

static int
parse_addrs_list_port(char *addr_list, int ipv4only, int ipv6only, struct sockaddr **addrs)
{
	char *port;
	char *last_colon;

	if (addr_list == NULL) {
		return (-1);
	}
	last_colon = strrchr(addr_list, ':');
	if (last_colon == NULL) {
		return (-1);
	} else {
		port = last_colon + 1;
		*last_colon = '\0';
		return (parse_addrs_list(addr_list, port, ipv4only, ipv6only, addrs));
	}
}


/* This functions implements a trivial round robin selection algorithm.
 * We might add other policies later
 */
 
static struct server_info *
select_a_server(struct server_info server_infos[], int number_servers) {
	static int i = 0;

	if (i >= number_servers) {
		i = 0;
	}
	return (&server_infos[i++]);
}

int
main(int argc, char **argv)
{
	int fd;
	struct sockaddr *local_addrs;
	int number_local_addrs;
	pthread_t tid;
	int c;
	int ipv4only;
	int ipv6only;
	uint16_t i_streams, o_streams;
	struct sctp_initmsg initmsg;
	long value;
	int number_servers, i;
	struct server_info *server_infos, *server_info;
	char *L_arg, *X_arg, **S_args;
	struct proxy_info *proxy_info;

	ipv4only = 0;
	ipv6only = 0;
	i_streams = 1;
	o_streams = 1;
	L_arg = NULL;
	X_arg = NULL;
	S_args = calloc(argc, sizeof(char *));
	number_servers = 0;

	while ((c = getopt(argc, argv, "i:L:o:S:X:46")) != -1) {
		switch(c) {
		case 'i':
			value = strtol(optarg, NULL, 10);
			if ((0 < value) && (value < 65536)) {
				i_streams = (uint16_t)value;
			} else {
				fprintf(stderr, "number of incoming streams out of range.\n");
				return (1);
			}
			break;
		case 'L':
			if (L_arg != NULL) {
				fprintf(stderr, "addresses to be listening on provided multiple times.\n");
				return (1);
			} else {
				L_arg = optarg;
			}
			break;
		case 'o':
			value = strtol(optarg, NULL, 10);
			if ((0 < value) && (value < 65536)) {
				o_streams = (uint16_t)value;
			} else {
				fprintf(stderr, "number of outgoing streams out of range.\n");
				return (1);
			}
			break;
		case 'S':
			S_args[number_servers++]= optarg;
			break;
		case 'X':
			if (X_arg != NULL) {
				fprintf(stderr, "addresses to be uses as a client provided multiple times.\n");
				return (1);
			} else {
				X_arg = optarg;
			}
			break;
		case '4':
			ipv4only = 1;
			if (ipv6only) {
				fprintf(stderr, "-4 and -6 can't be specified together.\n");
				return (1);
			}
			break;
		case '6':
			ipv6only = 1;
			if (ipv4only) {
				fprintf(stderr, "-4 and -6 can't be specified together.\n");
				return (1);
			}
			break;
		default:
			fprintf(stderr, "%s", "Unknown option.\n");
			return (1);
		}
	}

	if ((fd = socket(ipv4only ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_SCTP)) < 0) {
		fprintf(stderr, "Can't open the listening socket: %s.\n", strerror(errno));
		return (1);
	}
	if (!ipv4only) {
		if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const void *)&ipv6only, (socklen_t)sizeof(int)) < 0) {
			fprintf(stderr, "Can't set the listening socket to IPv6 only: %s.\n", strerror(errno));
		}
	}
	memset(&initmsg, 0, sizeof(struct sctp_initmsg));
	initmsg.sinit_num_ostreams = o_streams;
	initmsg.sinit_max_instreams = i_streams;
	if (setsockopt(fd, IPPROTO_SCTP, SCTP_INITMSG, (const void *)&initmsg, (socklen_t)sizeof(struct sctp_initmsg)) < 0) {
		fprintf(stderr, "Can't set the number of streams: %s.\n", strerror(errno));
		return (1);
	}
	
	number_local_addrs = parse_addrs_list_port(L_arg, ipv4only, ipv6only, &local_addrs);
	if (number_local_addrs > 0) {
		if (sctp_bindx(fd, local_addrs, number_local_addrs, SCTP_BINDX_ADD_ADDR) < 0) {
			fprintf(stderr, "Can't bind the listening socket: %s.\n", strerror(errno));
			return (1);
		}
		free(local_addrs);
	} else {
		fprintf(stderr, "No valid local addresses to listen on are specified.\n");
		return (1);
	}
	number_local_addrs = parse_addrs_list(X_arg, "0", ipv4only, ipv6only, &local_addrs);
	if (number_local_addrs <= 0) {
		fprintf(stderr, "No local addresses to be used as a client are specified.\n");
		return (1);
	}
	server_infos = calloc(number_servers, sizeof(struct server_info));
	for (i = 0; i < number_servers; i++) {
		server_infos[i].number_addrs = parse_addrs_list_port(S_args[i], ipv4only, ipv6only, &server_infos[i].server_addrs);
		if (server_infos[i].number_addrs <= 0) {
			fprintf(stderr, "No valid remote addresses specified for server %d.\n", i);
			return (1);
		}
	}
	if (listen(fd, LISTEN_QUEUE) < 0) {
		fprintf(stderr, "Can't set the listening socket to the LISTEN state: %s.\n", strerror(errno));
		return (1);
	}
	for (;;) {
		proxy_info = calloc(1, sizeof(struct proxy_info));
		if ((proxy_info->client_fd = accept(fd, NULL, NULL)) < 0) {
			fprintf(stderr, "Couldn't accept an association: %s.\n", strerror(errno));
			free(proxy_info);
		} else {
			server_info = select_a_server(server_infos, number_servers);
			proxy_info->ipv4only = ipv4only;
			proxy_info->ipv6only = ipv6only;
			proxy_info->server_fd = -1;
			proxy_info->local_addrs = local_addrs;
			proxy_info->number_local_addrs = number_local_addrs;
			proxy_info->server_addrs = server_info->server_addrs;
			proxy_info->number_server_addrs = server_info->number_addrs;
			if (pthread_create(&tid, NULL, &proxy, (void *)proxy_info) != 0) {
				fprintf(stderr, "Couldn't start a thread.\n");
				close(proxy_info->client_fd);
				free(proxy_info);
			}
		}
	}
	if (close(fd) < 0) {
		fprintf(stderr, "Couldn't close the listening socket.\n");
	}
	return (0);
}
