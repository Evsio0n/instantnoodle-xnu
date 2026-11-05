/*
 * hkhttpd: a small web server on the phone's own loopback, for tests of Safari that need no network (the radio
 * joins nothing unless the owner gives it a network: run 152737). WebKit's network, web content and GPU processes all
 * work as they do for a site. It serves the files of one directory, flat, to 127.0.0.1 only, and says what it was
 * asked for.
 *
 *   hkhttpd DIR [PORT]     goes into the background (a job list goes on) and serves until the run ends
 *
 * One request a connection, one connection at a time: a test page's few files do not need more.
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *
type_of(const char *name)
{
	const char *dot = strrchr(name, '.');

	if (dot == NULL) {
		return "application/octet-stream";
	}
	if (strcmp(dot, ".html") == 0) {
		return "text/html; charset=utf-8";
	}
	if (strcmp(dot, ".png") == 0) {
		return "image/png";
	}
	if (strcmp(dot, ".css") == 0) {
		return "text/css";
	}
	if (strcmp(dot, ".js") == 0) {
		return "text/javascript";
	}
	return "application/octet-stream";
}

static void
answer(int client, const char *dir)
{
	char request[4096], path[1024], head[256], buffer[65536];
	size_t have = 0;
	struct stat st;
	int file = -1, length;

	while (have + 1 < sizeof(request)) {
		ssize_t got = read(client, request + have, sizeof(request) - 1 - have);

		if (got <= 0) {
			return;
		}
		have += (size_t)got;
		request[have] = 0;
		if (strstr(request, "\r\n\r\n") != NULL) {
			break;
		}
	}

	char *name = strncmp(request, "GET /", 5) == 0 ? request + 5 : NULL;

	if (name != NULL) {
		name[strcspn(name, " ?#\r\n")] = 0;
		if (*name == 0) {
			name = "index.html";
		}
		/* flat: nothing above or beside the directory can be named */
		if (strchr(name, '/') == NULL && strchr(name, '%') == NULL && name[0] != '.') {
			snprintf(path, sizeof(path), "%s/%s", dir, name);
			file = open(path, O_RDONLY);
		}
	}
	if (file < 0 || fstat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
		printf("hkhttpd: %s: no such file\n", name != NULL ? name : "(not a GET)");
		length = snprintf(head, sizeof(head), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
		(void)write(client, head, (size_t)length);
		if (file >= 0) {
			close(file);
		}
		return;
	}
	printf("hkhttpd: %s, %lld bytes\n", name, (long long)st.st_size);
	length = snprintf(head, sizeof(head),
	    "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
	    type_of(name), (long long)st.st_size);
	(void)write(client, head, (size_t)length);
	for (ssize_t got; (got = read(file, buffer, sizeof(buffer))) > 0;) {
		const char *at = buffer;

		while (got > 0) {
			ssize_t put = write(client, at, (size_t)got);

			if (put <= 0) {
				close(file);
				return;
			}
			at += put;
			got -= put;
		}
	}
	close(file);
}

int
main(int argc, char **argv)
{
	struct sockaddr_in address = { .sin_len = sizeof(address), .sin_family = AF_INET };
	int port = argc > 2 ? atoi(argv[2]) : 8000, one = 1, server;

	if (argc < 2) {
		printf("hkhttpd: hkhttpd DIR [PORT]\n");
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);
	server = socket(AF_INET, SOCK_STREAM, 0);
	address.sin_port = htons((uint16_t)port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (server < 0 || bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(server, 16) != 0) {
		perror("hkhttpd");
		return 1;
	}
	printf("hkhttpd: %s at http://127.0.0.1:%d/\n", argv[1], port);
	if (fork() != 0) {
		return 0;
	}
	setsid();
	for (;;) {
		int client = accept(server, NULL, NULL);

		if (client >= 0) {
			answer(client, argv[1]);
			close(client);
		}
	}
}
