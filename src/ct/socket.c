/*
 * Socket handling routines
 *
 * Copyright (C) 2003, Olaf Kirch <okir@caldera.de>
 */

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <fcntl.h>
#include <assert.h>

#include <openct/logging.h>

#include "socket.h"

#define IFD_MAX_SOCKETS	256
#define SOCK_BUFSIZ	512

static unsigned int	ifd_xid = 0;

static inline unsigned int
min(unsigned int a, unsigned int b)
{
	return (a < b)? a : b;
}

static void	ct_socket_link(ct_socket_t *, ct_socket_t *);
static void	ct_socket_unlink(ct_socket_t *);
static int	ct_socket_getcreds(ct_socket_t *);

/*
 * Create a socket object
 */
ct_socket_t *
ct_socket_new(unsigned int bufsize)
{
	ct_socket_t	*sock;

	sock = (ct_socket_t *) calloc(1, sizeof(*sock) + bufsize);
	if (sock == NULL)
		return NULL;

	/* Initialize socket buffer */
	ct_buf_init(&sock->buf, (sock + 1), bufsize);
	sock->fd = -1;

	return sock;
}

/*
 * Free a socket object
 */
void
ct_socket_free(ct_socket_t *sock)
{
	ct_socket_unlink(sock);
	if (sock->close)
		sock->close(sock);
	ct_socket_close(sock);
	free(sock);
}

/*
 * Create a client socket
 */
int
ct_socket_connect(ct_socket_t *sock, const char *path)
{
	struct sockaddr_un un;
	int		fd = -1;

	ct_socket_close(sock);

	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	strcpy(un.sun_path, path);

	if ((fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0
	 || fcntl(fd, F_SETFD, 1) < 0 /* close on exec */
	 || connect(fd, (struct sockaddr *) &un, sizeof(un)) < 0)
		return -1;

	sock->fd   = fd;
	return 0;
}

/*
 * Listen on a socket
 */
int
ct_socket_listen(ct_socket_t *sock, const char *pathname)
{
	struct sockaddr_un suns;
	int		fd;

	ct_socket_close(sock);

	memset(&suns, 0, sizeof(suns));
	suns.sun_family = AF_UNIX;
	strcpy(suns.sun_path, pathname);
	unlink(pathname);

	if ((fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0
	 || bind(fd, (struct sockaddr *) &suns, sizeof(suns)) < 0
	 || listen(fd, 5) < 0)
		return -1;

	chmod(pathname, 0666);

	sock->events = POLLIN;
	sock->fd = fd;
	return 0;
}

/*
 * Accept incoming connection
 */
ct_socket_t *
ct_socket_accept(ct_socket_t *sock)
{
	ct_socket_t	*svc;
	int		fd;

	if (!(svc = ct_socket_new(SOCK_BUFSIZ)))
		return NULL;

	if ((fd = accept(sock->fd, NULL, NULL)) < 0) {
		ct_socket_free(svc);
		return NULL;;
	}

	svc->events = POLLIN;
	svc->fd = fd;

	/* obtain client credentials */
	svc->client_uid = -2;
	ct_socket_getcreds(svc);

	/* Add socket to list */
	ct_socket_link(sock, svc);

	return svc;
}

/*
 * Get client credentials
 * Should move this to a platform specific file
 */
int
ct_socket_getcreds(ct_socket_t *sock)
{
#ifdef SO_PEERCRED
	struct ucred	creds;
	socklen_t	len;

	len = sizeof(creds);
	if (getsockopt(sock->fd, SOL_SOCKET, SO_PEERCRED, &creds, &len) < 0)
		return -1;
	sock->client_uid = creds.uid;
#endif
	return 0;
}

/*
 * Close socket
 */
void
ct_socket_close(ct_socket_t *sock)
{
	ct_buf_clear(&sock->buf);
	if (sock->fd >= 0)
		close(sock->fd);
	sock->fd = -1;
}

/*
 * Transmit a call and receive the response
 */
int
ct_socket_call(ct_socket_t *sock, ct_buf_t *args, ct_buf_t *resp)
{
	ct_buf_t	*bp = &sock->buf, data;
	unsigned int	xid = ifd_xid++, avail;
	header_t	header;
	int		rc;

	/* Compact send buffer */
	ct_buf_compact(bp);

	/* Build header - note there's no need to convert
	 * integers to network byte order: everything happens
	 * on the same host, so there's no byte order issue */
	header.xid   = xid;
	header.count = ct_buf_avail(args);
	header.dest  = 0;

	/* Put everything into send buffer and transmit */
	if (ct_socket_put_packet(sock, &header, args) < 0
	 || ct_socket_flsbuf(sock, 1) < 0)
		return -1;

	/* Loop until we receive a complete packet with the
	 * right xid in it */
	do {
		if (ct_socket_filbuf(sock) < 0)
			return -1;

		ct_buf_clear(resp);
		if ((rc = ct_socket_get_packet(sock, &header, &data)) < 0)
			return -1;
	}  while (rc == 0 || header.xid != xid);

	if (header.error)
		return header.error;

	avail = ct_buf_avail(&data);
	if (avail > ct_buf_tailroom(resp)) {
		ct_error("received truncated reply (%u out of %u bytes)",
				rc, header.count);
		return -1;
	}

	ct_buf_put(resp, ct_buf_head(&data), avail);
	return header.count;
}

/*
 * Put packet into send buffer
 */
int
ct_socket_put_packet(ct_socket_t *sock, header_t *hdr, ct_buf_t *data)
{
	ct_buf_t	*bp = &sock->buf;

	ct_buf_clear(bp);
	if (ct_buf_put(bp, hdr, sizeof(*hdr)) < 0
	 || (data &&
	     ct_buf_put(bp, ct_buf_head(data), ct_buf_avail(data)) < 0)) {
		ct_error("packet too large for buffer");
		return -1;
	}

	sock->events = POLLOUT;
	return 0;
}

/*
 * Get packet from buffer
 */
int
ct_socket_get_packet(ct_socket_t *sock, header_t *hdr, ct_buf_t *data)
{
	ct_buf_t	*bp = &sock->buf;
	unsigned int	avail;
	header_t	th;

	avail = ct_buf_avail(bp);
	if (avail < sizeof(header_t))
		return 0;

	memcpy(&th, ct_buf_head(bp), sizeof(th));
	if (avail >= sizeof(header_t) + th.count) {
		/* There's enough data in the buffer
		 * Extract header... */
		ct_buf_get(bp, hdr, sizeof(*hdr));

		/* ... set data buffer (don't copy, just set pointers) ... */
		ct_buf_set(data, ct_buf_head(bp), hdr->count);

		/* ... and advance head pointer */
		ct_buf_get(bp, NULL, hdr->count);
		return 1;
	}

	/* Check if this packet will ever fit into this buffer */
	if (ct_buf_size(bp) < sizeof(header_t) + th.count) {
		ct_error("packet too large for buffer");
		return -1;
	}

	return 0;
}

/*
 * Read some data from socket and put it into buffer
 */
int
ct_socket_filbuf(ct_socket_t *sock)
{
	ct_buf_t	*bp = &sock->buf;
	unsigned int	count;
	int		n;

	if (!(count = ct_buf_tailroom(bp))) {
		ct_buf_compact(bp);
		if (!(count = ct_buf_tailroom(bp))) {
			ct_error("packet too large");
			return -1;
		}
	}

	n = read(sock->fd, ct_buf_tail(bp), count);
	if (n < 0) {
		ct_error("socket recv error: %m");
		return -1;
	}

	/* EOF should only occur when there's no data in 
	 * the buffer (otherwise we're probably waiting
	 * on a partial request) */
	if (n == 0) {
		if (ct_buf_avail(bp)) {
			ct_error("Peer closed connection");
			return -1;
		}
		return 0;
	}

	/* Advance buffer tail pointer */
	ct_buf_put(bp, NULL, n);
	return n;
}

/*
 * Flush data from buffer to socket
 * FIXME - ignore SIGPIPE while writing
 */
int
ct_socket_flsbuf(ct_socket_t *sock, int all)
{
	ct_buf_t	*bp = &sock->buf;
	int		n;

	do {
		if (!(n = ct_buf_avail(bp))) {
			sock->events = POLLIN;
			break;
		}
		n = write(sock->fd, ct_buf_head(bp), n);
		if (n < 0) {
			ct_error("socket send error: %m");
			break;
		}
		/* Advance head pointer */
		ct_buf_get(bp, NULL, n);
	} while (all);

	return n;
}

/*
 * Send/receive request
 */
int
ct_socket_send(ct_socket_t *sock, header_t *hdr, ct_buf_t *data)
{
	if (ct_socket_write(sock, hdr, sizeof(*hdr)) < 0
	 || ct_socket_write(sock, ct_buf_head(data), hdr->count) < 0)
		return -1;
	return 0;
}

int
ct_socket_recv(ct_socket_t *sock, header_t *hdr, ct_buf_t *resp)
{
	unsigned int	left, count, n;
	unsigned char	c;
	int		rc;

	if (ct_socket_write(sock, hdr, sizeof(*hdr)) < 0)
		return -1;

	if (hdr->count > 1024) {
		ct_error("oversize packet, discarding");
		ct_socket_close(sock);
		return -1;
	}

	/* Read the data following the packet header. If
	 * there's more data than the receive buffer can hold,
	 * truncate the packet.
	 * We return the number of bytes stored - the true packet
	 * length is available from the header
	 */
	left = hdr->count;
	count = 0;
	while (left) {
		n = min(left, ct_buf_tailroom(resp));
		if (n == 0) {
			rc = ct_socket_read(sock, &c, 1);
		} else {
			rc = ct_socket_read(sock, ct_buf_tail(resp), n);
		}
		if (rc < 0)
			return -1;
		count += n;
		left -= rc;
	}

	return count;
}

/*
 * Socket read/write routines
 */
int
ct_socket_write(ct_socket_t *sock, void *ptr, size_t len)
{
	unsigned int	count = 0;
	int		rc;

	if (sock->fd < 0)
		return -1;

	while (count < len) {
		/* XXX block SIGPIPE */
		rc = write(sock->fd, ptr, len);
		if (rc < 0) {
			ct_error("send error: %m");
			goto done;
		}
		(caddr_t) ptr += rc;
		count += rc;
	}
	rc = count;
done:	return rc;
}

int
ct_socket_write_nb(ct_socket_t *sock, void *ptr, size_t len)
{
	int	rc;

	if (sock->fd < 0)
		return -1;

	/* XXX block SIGPIPE */
	rc = write(sock->fd, ptr, len);
	if (rc < 0)
		ct_error("send error: %m");

	return rc;
}

int
ct_socket_read(ct_socket_t *sock, void *ptr, size_t size)
{
	unsigned int	count = 0;
	int		rc;

	if (sock->fd < 0)
		return -1;

	while (count < size) {
		rc = read(sock->fd, ptr, size - count);
		if (rc < 0) {
			ct_error("recv error: %m");
			goto done;
		}
		if (rc == 0) {
			ct_error("peer closed connection");
			rc = -1;
			goto done;
		}
		(caddr_t) ptr += rc;
		count += rc;
	}
	rc = count;

done:	return rc;
}

int
ct_socket_read_nb(ct_socket_t *sock, void *ptr, size_t size)
{
	int	rc;

	if (sock->fd < 0)
		return -1;

	rc = read(sock->fd, ptr, size);
	if (rc < 0)
		ct_error("recv error: %m");
	return rc;
}

/*
 * This is the main server loop
 */
void
ct_socket_server_loop(ct_socket_t *listener)
{
	unsigned int	nsockets = 1;
	ct_socket_t	head;
	struct pollfd	pfd[IFD_MAX_SOCKETS];

	head.next = head.prev = NULL;
	ct_socket_link(&head, listener);

	while (1) {
		ct_socket_t	*sock, *next;
		unsigned int	n = 0;
		int		rc;

		/* Count active sockets */
		for (nsockets = 0, sock = head.next; sock; sock = next) {
			next = sock->next;
			if (sock->fd < 0)
				ct_socket_free(sock);
			else
				nsockets++;
		}

		if (nsockets == 0)
			break;

		/* Stop accepting new connections if there are
		 * too many already */
		listener->events = (nsockets < IFD_MAX_SOCKETS)?  POLLIN : 0;

		/* Make sure we don't exceed the max socket limit */
		assert(nsockets < IFD_MAX_SOCKETS);

		/* Set up the poll structure */
		for (n = 0, sock = head.next; sock; sock = sock->next, n++) {
			pfd[n].fd = sock->fd;
			pfd[n].events = sock->events;
		}

		rc = poll(pfd, n, 1000);
		if (rc < 0) {
			if (rc == EINTR)
				continue;
			ct_error("poll: %m");
			exit(1);
		}

		for (n = 0, sock = head.next; sock; sock = next, n++) {
			next = sock->next;
			if (pfd[n].revents & POLLOUT) {
				if (sock->send(sock) < 0) {
					ct_socket_free(sock);
					continue;
				}
			}
			if (pfd[n].revents & POLLIN) {
				if ((rc = sock->recv(sock)) < 0) {
					ct_socket_free(sock);
					continue;
				}
			}
		}
	}
}

/*
 * Link/unlink socket
 */
void
ct_socket_link(ct_socket_t *prev, ct_socket_t *sock)
{
	ct_socket_t	*next = prev->next;

	if (next)
		next->prev = sock;
	if (prev)
		prev->next = sock;
	sock->prev = prev;
	sock->next = next;
}

void
ct_socket_unlink(ct_socket_t *sock)
{
	ct_socket_t	*next = sock->next,
			*prev = sock->prev;

	if (next)
		next->prev = prev;
	if (prev)
		prev->next = next;
	sock->prev = sock->next = NULL;
}

#if 0
	FD_ZERO(&servfds);
	FD_SET(fd, &servfds);
	maxfd = fd;

	while (1) {
		fd_set	rfds = servfds;

		n = select(maxfd+1, &rfds, NULL, NULL, NULL);
		if (n < 0) {
			/* XXX is this sufficient/correct? */
			if (errno != EINTR)
				return 0;
			continue;
		}
		if (n == 0)
			continue;

		/* Handle new connection attempt */
		if (FD_ISSET(fd, &rfds)) {
			if (!(svc = lpc_server_accept(fd, handler)))
				continue;
			if (svc->fd >= maxfd) {
				maxfd = svc->fd;
				conn = (lpc_server **) realloc(conn,
						(maxfd+1) * sizeof(svc));
			}
			FD_SET(svc->fd, &servfds);
			conn[svc->fd] = svc;
		}

		/* Handle data on a connection */
		for (n = 0; n <= maxfd; n++) {
			if ((svc = conn[n]) && FD_ISSET(svc->fd, &rfds)) {
				if (!lpc_server_request(svc)) {
					FD_CLR(svc->fd, &servfds);
					lpc_server_close(svc);
				}
			}
		}
	}
}
#endif

#if 0
/*
 * Set up the server struct
 */
void
lpc_server_setup(lpc_server *svc, lpc_proc *prog, lpc_dispatch_t dispatch,
			unsigned int argsize, unsigned int ressize,
			void *appdata)
{
	if (!(svc->buf = NEW(ct_buf_t)))
		return;
	svc->prog	= prog;
	svc->dispatch	= dispatch;
	svc->args	= malloc(argsize);
	svc->resp	= malloc(ressize);
	svc->argsize	= argsize;
	svc->ressize	= ressize;
	svc->appdata	= appdata;

	if (!svc->args || !svc->resp) {
		if (svc->args)
			free(svc->args);
		if (svc->resp)
			free(svc->resp);
		free(svc->buf);
		return;
	}
}

/*
 * Fork a new process that handles all requests on this socket
 */
void
lpc_server_fork(lpc_server *svc, void (*runfunc)(lpc_server *))
{
	pid_t	pid;

	if (!svc->buf || !svc->dispatch || !svc->args || !svc->resp) {
		/* XXX: log a warning? */
		goto over_and_out;
	}

	if ((pid = fork()) < 0) {
		/* XXX: log a warning? */
		goto over_and_out;
	}

	if (pid == 0) {
		/* We're the child process.
		 * First, close parent's listen socket */
		close(svc->xfd);
		svc->xfd = -1;

		/* Call back the application to service all requests */
		if (runfunc) {
			runfunc(svc);
		} else {
			while(lpc_server_request(svc))
				;
		}
		exit(0);
	}

over_and_out:
	close(svc->fd);
	svc->fd = -1;
}

/*
 * Service a request
 */
int
lpc_server_request(lpc_server *svc)
{
	ct_buf_t		*bp = svc->buf;
	lpc_header	hdr;
	lpc_proc	*pc;
	const char	*msg = NULL;
	int		res = 0;

	/* Zap the serialization buffer */
	mem_buf_free(bp);

	if (!lpc_recvpdu(svc->fd, &hdr, bp))
		return 0;

	for (pc = svc->prog; pc->cmd && pc->cmd != hdr.cmd; pc++)
		;
	if (pc->cmd == 0) {
		hdr.cmd = LPC_BADPROC;
		msg = "unsupported server procedure";
		goto respond;
	}

	/* Decode the arguments */
	memset(svc->args, 0, svc->argsize);
	if (!pc->args(bp, 0, svc->args)) {
		hdr.cmd = LPC_DECODE_ERR;
		msg = "unable to decode request packet";
		goto respond;
	}

	/* Process the request */
	memset(svc->resp, 0, sizeof(svc->ressize));
	res = svc->dispatch(svc, &hdr, svc->args, svc->resp);

	/* Return immediately if the dispatched shut down the
	 * service socket */
	if (svc->fd < 0)
		return 0;

	/* Assume everything worked well */
	hdr.cmd = LPC_SUCCESS;

respond:
	/* Zap the argument buffer */
	mem_buf_free(bp);

	/* If we have a response, try to encode it */
	if (res && !pc->resp(bp, 1, svc->resp)) {
		hdr.cmd = LPC_ENCODE_ERR;
		mem_buf_free(bp);
		res = 0;
	}

	/* Transmit the response PDU */
	return lpc_sendpdu(svc->fd, &hdr, bp);
}

/*
 * Mark this server for shutdown
 */
void
lpc_server_shutdown(lpc_server *svc)
{
	if (svc->fd >= 0) {
		close(svc->fd);
		svc->fd = -1;
	}
}

/*
 * Close a server socket
 */
void
lpc_server_close(lpc_server *svc)
{
	if (svc->fd >= 0)
		close(svc->fd);
	if (svc->buf) {
		mem_buf_free(svc->buf);
		free(svc->buf);
	}
	if (svc->args)
		free(svc->args);
	if (svc->resp)
		free(svc->resp);
	memset(svc, 0, sizeof(*svc));
	free(svc);
}

/*
 * Send a PDU
 */
int
lpc_sendpdu(int fd, lpc_header *hdr, ct_buf_t *bp)
{
	unsigned int	len = bp->tail - bp->head;

	hdr->cmd = htonl(hdr->cmd);
	hdr->len = htonl(len);

	return lpc_send(fd, hdr, sizeof(*hdr))
	    && lpc_send(fd, bp->head, len);
}

/*
 * Receive a PDU
 */
int
lpc_recvpdu(int fd, lpc_header *hdr, ct_buf_t *bp)
{
	unsigned int	count, n;

	if (!lpc_recv(fd, hdr, sizeof(*hdr)))
		return 0;
	hdr->cmd = ntohl(hdr->cmd);
	hdr->len = ntohl(hdr->len);

	for (count = hdr->len; count; count -= n) {
		char	buffer[1024];

		if ((n = count) > sizeof(buffer))
			n = sizeof(buffer);
		if (!lpc_recv(fd, buffer, n))
			return 0;
		mem_buf_put(bp, buffer, n);
	}
	return 1;
}

/*
 * Send the specified amount of bytes.
 * Make sure to block SIGPIPE while writing.
 */
static int
lpc_send(int fd, void *data, unsigned int count)
{
	unsigned char	*buffer;
	sigset_t	set, oset;
	int		n, res = 0;

	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	sigprocmask(SIG_BLOCK, &set, &oset);

	buffer = (unsigned char *) data;
	while (count) {
		n = write(fd, buffer, count);
		if (n < 0)
			goto out;
		buffer += n;
		count -= n;
	}
	res = 1;
out:	sigprocmask(SIG_SETMASK, &oset, NULL);
	return res;
}

/*
 * Receive a given amount of bytes and store them in the buffer.
 */
static int
lpc_recv(int fd, void *data, unsigned int count)
{
	unsigned char	*buffer;
	int		n;

	buffer = (unsigned char *) data;
	while (count) {
		n = read(fd, buffer, count);
		if (n <= 0)
			return 0;
		buffer += n;
		count -= n;
	}
	return 1;
}

/*
 * Codec functions
 */
int
lpc_c_void(ct_buf_t *bp, int enc, void *ptr)
{
	return 1;
}

int
lpc_c_int(ct_buf_t *bp, int enc, void *ptr)
{
	u_int32_t	value;

	if (enc) {
		value = htonl(*(u_int32_t *) ptr);
		return mem_buf_put(bp, &value, 4);
	} else {
		if (!mem_buf_get(bp, &value, 4))
			return 0;
		*(u_int32_t *) ptr = ntohl(value);
	}
	return 1;
}

int
lpc_c_string(ct_buf_t *bp, int enc, void *ptr)
{
	u_int32_t	len;
	char		*str;

	if (enc) {
		str = *(char **) ptr;
		len = str? strlen(str) : 0;
		if (!lpc_c_int(bp, 1, &len)
		 || !mem_buf_put(bp, str, len))
			return 0;
	} else {
		/* Decode in place: first, get the length,
		 * then move the string forward 4 bytes and
		 * NUL terminate it
		 */
		if (bp->head + 4 > bp->tail)
			return 0;

		memcpy(&len, bp->head, 4);
		len = ntohl(len);
		if (len == 0) {
			str = NULL;
		} else if (bp->head + 4 + len <= bp->tail) {
			memmove(bp->head, bp->head + 4, len);
			str = bp->head;
			str[len] = '\0';
		} else {
			return 0;
		}
		bp->head += 4 + len;
		*(char **) ptr = str;
	}
	return 1;
}

int
lpc_c_blob(ct_buf_t *bp, int enc, void *ptr)
{
	lpc_blob	*blob = (lpc_blob *) ptr;

	if (!lpc_c_int(bp, enc, &blob->type)
	 || !lpc_c_int(bp, enc, &blob->size))
		return 0;

	if (enc) {
		mem_buf_put(bp, blob->data, blob->size);
	} else {
		if (bp->head + blob->size > bp->tail)
			return 0;
		blob->data = bp->head;
		bp->head += blob->size;
	}

	return 1;
}
#endif
