#ifndef SYSTEM_SYSAPI_H
#define SYSTEM_SYSAPI_H

#include <cpu/idt.h>
#include <include/types.h>

enum socket_type;

void syscall_init();
pid_t sys_fork();
int32_t sys_socket(int32_t family, enum socket_type type, int32_t protocal);
int32_t sys_sbrk(intptr_t increment);
int32_t sys_dup2(int oldfd, int newfd);

#endif
