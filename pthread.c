/* Linuxthreads - a simple clone()-based implementation of Posix        */
/* threads for Linux.                                                   */
/* Copyright (C) 1996 Xavier Leroy (Xavier.Leroy@inria.fr)              */
/*                                                                      */
/* This program is free software; you can redistribute it and/or        */
/* modify it under the terms of the GNU Library General Public License  */
/* as published by the Free Software Foundation; either version 2       */
/* of the License, or (at your option) any later version.               */
/*                                                                      */
/* This program is distributed in the hope that it will be useful,      */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of       */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        */
/* GNU Library General Public License for more details.                 */

/* Thread creation, initialization, and basic low-level routines */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "pthread.h"
#include "internals.h"
#include "restart.h"

/* Descriptor of the initial thread */

struct _pthread __pthread_initial_thread = {
  &__pthread_initial_thread,  /* pthread_t p_nextlive */
  &__pthread_initial_thread,  /* pthread_t p_prevlive */
  NULL,                       /* pthread_t p_nextwaiting */
  0,                          /* int p_pid */
  0,                          /* int p_spinlock */
  0,                          /* int p_signal */
  NULL,                       /* sigjmp_buf * p_signal_buf */
  NULL,                       /* sigjmp_buf * p_cancel_buf */
  0,                          /* char p_terminated */
  0,                          /* char p_detached */
  0,                          /* char p_exited */
  NULL,                       /* void * p_retval */
  0,                          /* int p_retval */
  NULL,                       /* pthread_t p_joining */
  NULL,                       /* struct _pthread_cleanup_buffer * p_cleanup */
  0,                          /* char p_cancelstate */
  0,                          /* char p_canceltype */
  0,                          /* char p_canceled */
  0,                          /* int p_errno */
  0,                          /* int p_h_errno */
  NULL,                       /* void *(*p_initial_fn)(void *) */
  NULL,                       /* void *p_initial_fn_arg */
  {{0, }},                    /* sigset_t p_initial_mask */
  {NULL}                      /* void * p_specific[PTHREAD_KEYS_MAX] */
};

/* Descriptor of the manager thread; none of this is used but the error
   variables and the address for identification.  */

struct _pthread __pthread_manager_thread = {
  NULL,                       /* pthread_t p_nextlive */
  NULL,                       /* pthread_t p_prevlive */
  NULL,                       /* pthread_t p_nextwaiting */
  0,                          /* int p_pid */
  0,                          /* int p_spinlock */
  0,                          /* int p_signal */
  NULL,                       /* sigjmp_buf * p_signal_buf */
  NULL,                       /* sigjmp_buf * p_cancel_buf */
  0,                          /* char p_terminated */
  0,                          /* char p_detached */
  0,                          /* char p_exited */
  NULL,                       /* void * p_retval */
  0,                          /* int p_retval */
  NULL,                       /* pthread_t p_joining */
  NULL,                       /* struct _pthread_cleanup_buffer * p_cleanup */
  0,                          /* char p_cancelstate */
  0,                          /* char p_canceltype */
  0,                          /* char p_canceled */
  0,                          /* int p_errno */
  0,                          /* int p_h_errno */
  NULL,                       /* void *(*p_initial_fn)(void *) */
  NULL,                       /* void *p_initial_fn_arg */
  {{0, }},                    /* sigset_t p_initial_mask */
  {NULL}                      /* void * p_specific[PTHREAD_KEYS_MAX] */
};

/* Pointer to the main thread (the father of the thread manager thread) */
/* Originally, this is the initial thread, but this changes after fork() */

pthread_t __pthread_main_thread = &__pthread_initial_thread;

/* Limit between the stack of the initial thread (above) and the
   stacks of other threads (below). Aligned on a STACK_SIZE boundary. */

char *__pthread_initial_thread_bos = NULL;

/* File descriptor for sending requests to the thread manager. */
/* Initially -1, meaning that the thread manager is not running. */

int __pthread_manager_request = -1;

/* Other end of the pipe for sending requests to the thread manager. */

int __pthread_manager_reader;

/* Limits of the thread manager stack */

char *__pthread_manager_thread_bos = NULL;
char *__pthread_manager_thread_tos = NULL;

/* For process-wide exit() */

int __pthread_exit_requested = 0;
int __pthread_exit_code = 0;

/* Forward declarations */

static void pthread_exit_process(int retcode, void *arg);
static void pthread_handle_sigcancel(int sig);

/* Initialize the pthread library.
   Initialization is split in two functions:
   - a constructor function that blocks the PTHREAD_SIG_RESTART signal
     (must do this very early, since the program could capture the signal
      mask with e.g. sigsetjmp before creating the first thread);
   - a regular function called from pthread_create when needed. */
// 在main函数之前执行该函数
void __pthread_initialize(void) __attribute__((constructor));

void __pthread_initialize(void)
{
  struct sigaction sa;
  sigset_t mask;

  /* We may be called by others.  This may happen if the constructors
     are not called in the order we need.  */
  if (__pthread_initial_thread_bos != NULL)
    return;

  /* For the initial stack, reserve at least STACK_SIZE bytes of stack
     below the current stack address, and align that on a
     STACK_SIZE boundary. */
  // 主线程的栈顶，即栈的低地址,CURRENT_STACK_FRAME是当前esp
  __pthread_initial_thread_bos =
    // 按STACK_SIZE大小对齐
    (char *)(((long)CURRENT_STACK_FRAME - 2 * STACK_SIZE) & ~(STACK_SIZE - 1));
  /* Update the descriptor for the initial thread. */
  // 即main函数代表的主进程id
  __pthread_initial_thread.p_pid = getpid();
  /* If we have special thread_self processing, initialize that for the
     main thread now.  */
#ifdef INIT_THREAD_SELF
  INIT_THREAD_SELF(&__pthread_initial_thread);
#endif
  /* Setup signal handlers for the initial thread.
     Since signal handlers are shared between threads, these settings
     will be inherited by all other threads. */
  // 为两个信号注册处理函数
  sa.sa_handler = __pthread_sighandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART; /* does not matter for regular threads, but
                               better for the thread manager */
  sigaction(PTHREAD_SIG_RESTART, &sa, NULL);
  sa.sa_handler = pthread_handle_sigcancel;
  sa.sa_flags = 0;
  sigaction(PTHREAD_SIG_CANCEL, &sa, NULL);

  /* Initially, block PTHREAD_SIG_RESTART. Will be unblocked on demand. */
  // 屏蔽restart信号
  sigemptyset(&mask);
  sigaddset(&mask, PTHREAD_SIG_RESTART);
  sigprocmask(SIG_BLOCK, &mask, NULL);
  /* Register an exit function to kill all other threads. */
  /* Do it early so that user-registered atexit functions are called
     before pthread_exit_process. */
  // 注册退出时执行的函数
  __on_exit(pthread_exit_process, NULL);
}

static int pthread_initialize_manager(void)
{
  int manager_pipe[2];

  /* Setup stack for thread manager */
  // 在堆上分配一块内存用于manager线程的栈，__pthread_manager_thread_bos是栈顶
  __pthread_manager_thread_bos = malloc(THREAD_MANAGER_STACK_SIZE);
  if (__pthread_manager_thread_bos == NULL) return -1;
  // 栈底，即栈的开始地址，地址从大到小增长
  __pthread_manager_thread_tos =
    __pthread_manager_thread_bos + THREAD_MANAGER_STACK_SIZE;
  /* Setup pipe to communicate with thread manager */
  if (pipe(manager_pipe) == -1) {
    free(__pthread_manager_thread_bos);
    return -1;
  }
  __pthread_manager_request = manager_pipe[1]; /* writing end */
  __pthread_manager_reader = manager_pipe[0]; /* reading end */
  /* Start the thread manager */
  // 新建一个manager线程,manager_pipe是__thread_manager函数的入参
  if (__clone(__pthread_manager,
        // 栈底
	      __pthread_manager_thread_tos,
        // 共享内存，文件系统信息，文件描述符，信号处理函数集（屏蔽码和已收到的信号不共享）
	      CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND,
	      (void *)(long)manager_pipe[0]) == -1) {
    free(__pthread_manager_thread_bos);
    __libc_close(manager_pipe[0]);
    __libc_close(manager_pipe[1]);
    __pthread_manager_request = -1;
    return -1;
  }
  return 0;
}

/* Thread creation */

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void * (*start_routine)(void *), void *arg)
{
  pthread_t self = thread_self();
  struct pthread_request request;
  // 还没执行过pthread_initialize_manager则执行,用于初始化manager线程
  if (__pthread_manager_request < 0) {
    if (pthread_initialize_manager() < 0) return EAGAIN;
  }
  // 给manager发一下请求
  request.req_thread = self;
  request.req_kind = REQ_CREATE;
  request.req_args.create.attr = attr;
  request.req_args.create.fn = start_routine;
  request.req_args.create.arg = arg;
  // 获取当前线程的信号掩码
  sigprocmask(SIG_SETMASK, (const sigset_t *) NULL,
              &request.req_args.create.mask);
  // 通过管道写入，通知manager线程，新建一个线程
  __libc_write(__pthread_manager_request, (char *) &request, sizeof(request));
  // 挂起,等待manager唤醒
  suspend(self);
  // 等于0说明创建成功，否则返回失败的错误码,p_retval在pthread_handle_create中设置
  if (self->p_retcode == 0) *thread = (pthread_t) self->p_retval;
  return self->p_retcode;
}

/* Simple operations on thread identifiers */

pthread_t pthread_self(void)
{
  return thread_self();
}

int pthread_equal(pthread_t thread1, pthread_t thread2)
{
  return thread1 == thread2;
}

/* Thread scheduling */
// 设置线程调度策略
int pthread_setschedparam(pthread_t target_thread, int policy,
                          const struct sched_param *param)
{
  if (sched_setscheduler(target_thread->p_pid, policy, param) == -1)
    return errno;
  else
    return 0;
}

int pthread_getschedparam(pthread_t target_thread, int *policy,
                          struct sched_param *param)
{
  int p = sched_getscheduler(target_thread->p_pid);
  if (p == -1) return errno;
  if (sched_getparam(target_thread->p_pid, param) == -1) return errno;
  *policy = p;
  return 0;
}

/* Process-wide exit() request */
// 杀死除了自己的其他线程,见manager中关于REQ_PROCESS_EXIT的处理
static void pthread_exit_process(int retcode, void *arg)
{
  struct pthread_request request;
  pthread_t self = thread_self();
  // 管道写端，通知manager线程
  if (__pthread_manager_request >= 0) {
    request.req_thread = self;
    request.req_kind = REQ_PROCESS_EXIT;
    request.req_args.exit.code = retcode;
    __libc_write(__pthread_manager_request,
		 (char *) &request, sizeof(request));
    suspend(self);
  }
}

/* The handler for the RESTART signal just records the signal received
   in the thread descriptor, and optionally performs a siglongjmp
   (for pthread_cond_timedwait). Also used in sigwait.
   For the thread manager thread, redirect the signal to
   pthread_manager_sighandler. */
// PTHREAD_SIG_RESTART信号的处理函数
void __pthread_sighandler(int sig)
{
  pthread_t self = thread_self();
  // 管理线程收到PTHREAD_SIG_RESTART信号，则终止所有子线程
  if (self == &__pthread_manager_thread) {
    __pthread_manager_sighandler(sig);
  } else {
    // 一般线程收到信号，记录收到的信号
    self->p_signal = sig;
    // 该线程在等待restart信号则直接跳回到调用处，即sigsetjump处
    if (self->p_signal_jmp != NULL) siglongjmp(*self->p_signal_jmp, 1);
  }
}

/* The handler for the CANCEL signal checks for cancellation
   (in asynchronous mode) and for process-wide exit and exec requests. */
// 处理收获到的取消信号量。
static void pthread_handle_sigcancel(int sig)
{
  pthread_t self = thread_self();
  sigjmp_buf * jmpbuf;
  // pthread_handle_exit=1说明是除了自己外的其他线程已经退出，则收到取消信号时整个进程退出，在pthread_handle_exit中设置
  if (__pthread_exit_requested)
    _exit(__pthread_exit_code);
  // 某个线程退出了
  // 该线程被取消了（在pthread_cancel设置），
  if (self->p_canceled && self->p_cancelstate == PTHREAD_CANCEL_ENABLE) {
    // 需要立即取消，则线程退出
    if (self->p_canceltype == PTHREAD_CANCEL_ASYNCHRONOUS)
      pthread_exit(PTHREAD_CANCELED);
    // 延迟取消 1 在等待cancel信号则跳回到sigsetjump 2 不处理该信号
    jmpbuf = self->p_cancel_jmp;
    if (jmpbuf != NULL) {
      self->p_cancel_jmp = NULL;
      siglongjmp(*jmpbuf, 1);
    }
  }
}

/* Reset the state of the thread machinery after a fork().
   Close the pipe used for requests and set the main thread to the forked
   thread.
   Notice that we can't free the stack segments, as the forked thread
   may hold pointers into them. */
// 主线程默认是main函数执行时的线程，这里重置当前线程为主线程
void __pthread_reset_main_thread()
{
  pthread_t self = thread_self();

  /* Free the thread manager stack */
  // 释放管理线程的栈，因为已经不需要管理线程了
  free(__pthread_manager_thread_bos);
  __pthread_manager_thread_bos = __pthread_manager_thread_tos = NULL;
  /* Close the two ends of the pipe */
  // 关闭管道的读写端
  __libc_close(__pthread_manager_request);
  __libc_close(__pthread_manager_reader);
  __pthread_manager_request = __pthread_manager_reader = -1;
  /* Update the pid of the main thread */
  // 修改当前线程对应的进程pid
  self->p_pid = getpid();
  /* Make the forked thread the main thread */
  // 设置当前线程为主线程 
  __pthread_main_thread = self;
  // 链表中只有一个线程，就是自己，相当于一个进程里只有一个线程
  self->p_nextlive = self;
  self->p_nextlive = self;
}

/* Process-wide exec() request */
// 某个线程执行exec时，杀死除了自己之外的其他线程
void __pthread_kill_other_threads_np(void)
{
  /* Terminate all other threads and thread manager */
  // 杀死除了执行exec线程外的其他线程
  pthread_exit_process(0, NULL);
  /* Make current thread the main thread in case the calling thread
     changes its mind, does not exec(), and creates new threads instead. */
  // 执行exec的线程使自己为主线程
  __pthread_reset_main_thread();
}
weak_alias (__pthread_kill_other_threads_np, pthread_kill_other_threads_np)

/* Debugging aid */

#ifdef DEBUG
#include <stdarg.h>

void __pthread_message(char * fmt, long arg)
{
  char buffer[1024];
  va_list args;
  sprintf(buffer, "%05d : ", getpid());
  va_start(args, fmt);
  vsnprintf(buffer + 8, sizeof(buffer) - 8, fmt, args);
  va_end(args);
  __libc_write(2, buffer, strlen(buffer));
}
weak_alias (__pthread_message, pthread_message)

#endif


#ifndef PIC
/* We need a suspendhook to force the cancelation wrappers to be linked in when
   static libpthread is used.  */
extern const int __pthread_provide_wrappers;
static const int *const __pthread_require_wrappers =
  &__pthread_provide_wrappers;
#endif
