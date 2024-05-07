#include "user_process.h"
#include "globals.h"
#include "shared.h"

void signalSafeLog(int level, const char *message) {
  if (level < currentLogLevel) {
    return;
  }

  char buffer[LOG_BUFFER_SIZE];
  int msgLength = strlen(message);

  if (msgLength > LOG_BUFFER_SIZE - 2) {
    msgLength = LOG_BUFFER_SIZE - 2;
  }

  memcpy(buffer, message, msgLength);
  buffer[msgLength] = '\n';
  buffer[msgLength + 1] = '\0';

  write(STDERR_FILENO, buffer, strlen(buffer));
}

void signalHandler(int sig) {
  char buffer[256];
  if (sig == SIGINT || sig == SIGTERM) {
    snprintf(buffer, sizeof(buffer),
             "Child process (PID: %d): Termination requested by signal %d.\n",
             getpid(), sig);
    signalSafeLog(LOG_LEVEL_INFO, buffer);
    _exit(EXIT_SUCCESS); // For testing, exit immediately on SIGINT or SIGTERM
  } else if (sig == SIGCHLD) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {
    }
  }
}

void setupSignalHandlers(void) {
  struct sigaction sa;
  sigemptyset(&sa.sa_mask); // Do not block other signals during handling
  sa.sa_flags = 0;          // No special flags
  sa.sa_handler = signalHandler;

  if (sigaction(SIGINT, &sa, NULL) == -1 ||
      sigaction(SIGTERM, &sa, NULL) == -1 ||
      sigaction(SIGCHLD, &sa, NULL) == -1) {
    perror("Failed to set signal handlers");
    exit(EXIT_FAILURE);
  }
}

int better_sem_wait(sem_t *sem) {
  int result;
  log_message(LOG_LEVEL_ANNOY, "Attempting to acquire semaphore...");
  result = sem_wait(sem);
  if (result == 0) {
    log_message(LOG_LEVEL_ANNOY, "Semaphore acquired successfully.");
  } else {
    log_message(LOG_LEVEL_ERROR, "Failed to acquire semaphore.");
  }
  return result;
}
