#include <sys/socket.h>
#include <unistd.h>

_syscall3(socket, int, enum socket_type, int);
int socket(int family, enum socket_type type, int protocal)
{
	return syscall_socket(family, type, protocal);
}

_syscall3(bind, int, struct sockaddr *, unsigned int);
int bind(int sockfd, struct sockaddr *addr, unsigned int addrlen)
{
	return syscall_bind(sockfd, addr, addrlen);
}

_syscall3(connect, int, struct sockaddr *, unsigned int);
int connect(int sockfd, struct sockaddr *addr, unsigned int addrlen)
{
	return syscall_connect(sockfd, addr, addrlen);
}

_syscall3(send, int, void *, size_t);
int send(int sockfd, void *msg, size_t len)
{
	return syscall_send(sockfd, msg, len);
}

_syscall3(recv, int, void *, size_t);
int recv(int sockfd, void *msg, size_t len)
{
	return syscall_recv(sockfd, msg, len);
}
