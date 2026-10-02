#include "nty_coroutine.h"
#include <errno.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
static uint64_t kvstore_ntyco_now_ms(void);
static uint64_t kvstore_ntyco_deadline_ms(unsigned long timeout_ms) {
  const uint64_t now = kvstore_ntyco_now_ms();
  return timeout_ms > UINT64_MAX - now ? UINT64_MAX : now + timeout_ms;
}
static pthread_mutex_t kvstore_ntyco_eventfds_mutex = PTHREAD_MUTEX_INITIALIZER;
static int kvstore_ntyco_eventfds[16];
static nty_schedule *kvstore_ntyco_schedules[16];
static size_t kvstore_ntyco_eventfd_count = 0;
static void kvstore_ntyco_bootstrap(void *argument) { (void)argument; }

int kvstore_ntyco_spawn(void (*callback)(void *), void *argument) {
  nty_coroutine *coroutine = 0;
  return nty_coroutine_create(&coroutine, callback, argument);
}

int kvstore_ntyco_init(unsigned long stack_bytes) {
  void *requested_stack = malloc((size_t)stack_bytes);
  if (requested_stack == 0) return -1;
  nty_coroutine *coroutine = 0;
  if (nty_coroutine_create(&coroutine, kvstore_ntyco_bootstrap, 0) != 0) {
    nty_schedule *failed_schedule = nty_coroutine_get_sched();
    const int failed_eventfd = failed_schedule == 0 ? -1 : failed_schedule->eventfd;
    if (failed_schedule != 0) nty_schedule_free(failed_schedule);
    if (failed_eventfd >= 0) (void)syscall(SYS_close, failed_eventfd);
    free(requested_stack);
    return -1;
  }
  nty_schedule *schedule = nty_coroutine_get_sched();
  if (schedule == 0) {
    free(requested_stack);
    return -1;
  }
  free(schedule->stack);
  schedule->stack = requested_stack;
  schedule->stack_size = (size_t)stack_bytes;
  schedule->default_timeout = 1000;
  pthread_mutex_lock(&kvstore_ntyco_eventfds_mutex);
  if (kvstore_ntyco_eventfd_count >= 16) {
    pthread_mutex_unlock(&kvstore_ntyco_eventfds_mutex);
    const int eventfd = schedule->eventfd;
    nty_coroutine_free(coroutine);
    nty_schedule_free(schedule);
    if (eventfd >= 0) (void)syscall(SYS_close, eventfd);
    return -1;
  }
  kvstore_ntyco_eventfds[kvstore_ntyco_eventfd_count] = schedule->eventfd;
  kvstore_ntyco_schedules[kvstore_ntyco_eventfd_count] = schedule;
  kvstore_ntyco_eventfd_count++;
  pthread_mutex_unlock(&kvstore_ntyco_eventfds_mutex);
  return 0;
}

void kvstore_ntyco_run(void) {
  nty_schedule *schedule = nty_coroutine_get_sched();
  const int eventfd = schedule == 0 ? -1 : schedule->eventfd;
  nty_schedule_run();
  if (eventfd < 0) return;
  pthread_mutex_lock(&kvstore_ntyco_eventfds_mutex);
  for (size_t index = 0; index < kvstore_ntyco_eventfd_count; ++index) {
    if (kvstore_ntyco_eventfds[index] == eventfd) {
      kvstore_ntyco_eventfds[index] = kvstore_ntyco_eventfds[--kvstore_ntyco_eventfd_count];
      kvstore_ntyco_schedules[index] = kvstore_ntyco_schedules[kvstore_ntyco_eventfd_count];
      break;
    }
  }
  // The scheduler patch leaves ownership here. Remove it from the wakeup
  // registry before freeing it so Stop/cancel cannot dereference stale state.
  nty_schedule_free(schedule);
  (void)syscall(SYS_close, eventfd);
  pthread_mutex_unlock(&kvstore_ntyco_eventfds_mutex);
}

void kvstore_ntyco_wakeup(void) {
  const uint64_t value = 1;
  pthread_mutex_lock(&kvstore_ntyco_eventfds_mutex);
  for (size_t index = 0; index < kvstore_ntyco_eventfd_count; ++index)
    (void)syscall(SYS_write, kvstore_ntyco_eventfds[index], &value, sizeof(value));
  pthread_mutex_unlock(&kvstore_ntyco_eventfds_mutex);
}

void kvstore_ntyco_cancel_all(void) {
  pthread_mutex_lock(&kvstore_ntyco_eventfds_mutex);
  for (size_t index = 0; index < kvstore_ntyco_eventfd_count; ++index) {
    nty_schedule_request_stop(kvstore_ntyco_schedules[index]);
    (void)syscall(SYS_write, kvstore_ntyco_eventfds[index], &(uint64_t){1}, sizeof(uint64_t));
  }
  pthread_mutex_unlock(&kvstore_ntyco_eventfds_mutex);
}

void kvstore_ntyco_set_timeout(unsigned long usecs) {
  nty_schedule *schedule = nty_coroutine_get_sched();
  if (schedule != 0) schedule->default_timeout = usecs;
}

int kvstore_ntyco_socket(int domain, int type, int protocol) {
  return nty_socket(domain, type, protocol);
}
int kvstore_ntyco_accept(int fd, struct sockaddr *address, socklen_t *length) {
  return nty_accept(fd, address, length);
}
int kvstore_ntyco_accept_timed(int fd, struct sockaddr *address, socklen_t *length,
                               unsigned long timeout_ms) {
  const uint64_t deadline = kvstore_ntyco_deadline_ms(timeout_ms);
  for (;;) {
    if (nty_schedule_stop_requested(nty_coroutine_get_sched())) {
      errno = ECANCELED;
      return -1;
    }
    const int result = (int)syscall(SYS_accept4, fd, address, length,
                                    SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (result >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return result;
    if (kvstore_ntyco_now_ms() >= deadline) {
      errno = EAGAIN;
      return -1;
    }
    nty_schedule_sched_sleepdown(nty_coroutine_get_sched()->curr_thread, 1);
    nty_coroutine_yield(nty_coroutine_get_sched()->curr_thread);
  }
}
ssize_t kvstore_ntyco_recv(int fd, void *buffer, size_t length, int flags) {
  return nty_recv(fd, buffer, length, flags);
}
ssize_t kvstore_ntyco_send(int fd, const void *buffer, size_t length, int flags) {
  return nty_send(fd, buffer, length, flags);
}
int kvstore_ntyco_close(int fd) { return nty_close(fd); }

static uint64_t kvstore_ntyco_now_ms(void) {
  return nty_coroutine_usec_now() / 1000U;
}

ssize_t kvstore_ntyco_recv_timed(int fd, void *buffer, size_t length, int flags,
                                 unsigned long timeout_ms) {
  const uint64_t deadline = kvstore_ntyco_deadline_ms(timeout_ms);
  for (;;) {
    if (nty_schedule_stop_requested(nty_coroutine_get_sched())) {
      errno = ECANCELED;
      return -1;
    }
    const ssize_t result = syscall(SYS_recvfrom, fd, buffer, length, flags, NULL, NULL);
    if (result >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return result;
    if (kvstore_ntyco_now_ms() >= deadline) {
      errno = EAGAIN;
      return -1;
    }
    nty_schedule_sched_sleepdown(nty_coroutine_get_sched()->curr_thread, 1);
    nty_coroutine_yield(nty_coroutine_get_sched()->curr_thread);
  }
}

ssize_t kvstore_ntyco_send_timed(int fd, const void *buffer, size_t length, int flags,
                                 unsigned long timeout_ms) {
  const char *bytes = buffer;
  if (length > (size_t)SSIZE_MAX) {
    errno = EOVERFLOW;
    return -1;
  }
  size_t sent = 0;
  const uint64_t deadline = kvstore_ntyco_deadline_ms(timeout_ms);
  while (sent < length) {
    if (nty_schedule_stop_requested(nty_coroutine_get_sched())) {
      errno = ECANCELED;
      return sent == 0 ? -1 : (ssize_t)sent;
    }
    const ssize_t result = syscall(SYS_sendto, fd, bytes + sent, length - sent, flags, NULL, 0);
    if (result > 0) {
      sent += (size_t)result;
      continue;
    }
    if (result == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return sent == 0 ? result : (ssize_t)sent;
    if (kvstore_ntyco_now_ms() >= deadline) {
      errno = EAGAIN;
      return sent == 0 ? -1 : (ssize_t)sent;
    }
    nty_schedule_sched_sleepdown(nty_coroutine_get_sched()->curr_thread, 1);
    nty_coroutine_yield(nty_coroutine_get_sched()->curr_thread);
  }
  return (ssize_t)sent;
}
