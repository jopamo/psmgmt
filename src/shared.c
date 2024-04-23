#include "shared.h"

ProcessType gProcessType;
struct timeval startTime;
struct timeval lastLogTime;

SimulatedClock *simClock = NULL;
ActualTime *actualTime = NULL;

PCB *processTable;

MLFQ mlfq;

FILE *logFile = NULL;
char logFileName[256] = DEFAULT_LOG_FILE_NAME;

int msqId = -1;

int simulatedTimeShmId = -1;
int actualTimeShmId = -1;
int processTableShmId = -1;
int resourceTableShmId = -1;
int deadlockShmId = -1;

ResourceDescriptor *resourceTable = NULL;

volatile sig_atomic_t childTerminated = 0;
volatile sig_atomic_t keepRunning = 1;

int maxProcesses = DEFAULT_MAX_PROCESSES;
int maxSimultaneous = DEFAULT_MAX_SIMULTANEOUS;
int childTimeLimit = DEFAULT_CHILD_TIME_LIMIT;
int launchInterval = DEFAULT_LAUNCH_INTERVAL;

int currentChildren = 0;

int currentLogLevel = LOG_LEVEL_INFO;

sem_t *clockSem = SEM_FAILED;
const char *clockSemName = "/simClockSem";

pthread_mutex_t logMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t resourceTableMutex = PTHREAD_MUTEX_INITIALIZER;

int getCurrentChildren(void) { return currentChildren; }
void setCurrentChildren(int value) { currentChildren = value; }

void *attachSharedMemory(const char *path, int proj_id, size_t size,
                         const char *segmentName) {
  key_t key = getSharedMemoryKey(path, proj_id);
  int shmId = shmget(key, size, 0666 | IPC_CREAT);
  if (shmId < 0) {
    log_message(LOG_LEVEL_ERROR, "Failed to obtain shmId for %s: %s",
                segmentName, strerror(errno));
    return NULL;
  }
  void *shmPtr = shmat(shmId, NULL, 0);
  if (shmPtr == (void *)-1) {
    log_message(LOG_LEVEL_ERROR, "Failed to attach to %s shared memory: %s",
                segmentName, strerror(errno));
    return NULL;
  }
  return shmPtr;
}

int detachSharedMemory(void **shmPtr, const char *segmentName) {
  if (shmPtr == NULL || *shmPtr == NULL) {
    log_message(LOG_LEVEL_ERROR,
                "Invalid pointer for detaching %s shared memory.", segmentName);
    return -1;
  }

  if (shmdt(*shmPtr) == -1) {
    log_message(LOG_LEVEL_ERROR, "Detaching from %s shared memory failed: %s",
                segmentName, strerror(errno));
    return -1;
  }

  *shmPtr = NULL;
  log_message(LOG_LEVEL_INFO, "Successfully detached from %s shared memory.",
              segmentName);
  return 0;
}

const char *processTypeToString(ProcessType type) {
  switch (type) {
  case PROCESS_TYPE_OSS:
    return "OSS";
  case PROCESS_TYPE_WORKER:
    return "Worker";
  case PROCESS_TYPE_TIMEKEEPER:
    return "Timekeeper";
  default:
    return "Unknown";
  }
}

void log_message(int level, const char *format, ...) {
  if (level < currentLogLevel)
    return;

  if (pthread_mutex_lock(&logMutex) != 0) {
    fprintf(stderr, "Error locking log mutex\n");
    return;
  }

  char buffer[LOG_BUFFER_SIZE];
  int offset = snprintf(buffer, sizeof(buffer), "[%s] ",
                        processTypeToString(gProcessType));

  va_list args;
  va_start(args, format);
  vsnprintf(buffer + offset, sizeof(buffer) - offset, format, args);
  va_end(args);

  buffer[sizeof(buffer) - 1] = '\0';

  fprintf(stderr, "%s\n", buffer);
  if (logFile) {
    fprintf(logFile, "%s\n", buffer);
    fflush(logFile);
  }

  if (pthread_mutex_unlock(&logMutex) != 0) {
    fprintf(stderr, "Error unlocking log mutex\n");
  }
}

key_t getSharedMemoryKey(const char *path, int proj_id) {
  key_t key = ftok(path, proj_id);
  if (key == -1) {
    log_message(LOG_LEVEL_ERROR, "ftok failed for proj_id %d: %s", proj_id,
                strerror(errno));
  } else {
    log_message(LOG_LEVEL_INFO,
                "ftok success for proj_id %d: Key generated: %d", proj_id, key);
  }
  return key;
}

int initMessageQueue(void) {
  key_t msg_key = ftok(MSG_PATH, MSG_PROJ_ID);
  if (msg_key == -1) {
    log_message(LOG_LEVEL_ERROR, "ftok for msg_key failed: %s",
                strerror(errno));
    return -1;
  }

  msqId = msgget(msg_key, IPC_CREAT | 0666);
  if (msqId < 0) {
    log_message(LOG_LEVEL_ERROR, "msgget failed: %s", strerror(errno));
    return -1;
  }

  log_message(LOG_LEVEL_DEBUG, "Message queue initialized successfully");
  return msqId;
}

int sendMessage(int msqId, Message *msg) {
  log_message(
      LOG_LEVEL_DEBUG,
      "[SEND] Attempting to send message. msqId: %d, Type: %ld, Content: %d",
      msqId, msg->mtype, msg->mtext);

  size_t messageSize = sizeof(*msg) - sizeof(long);
  if (msgsnd(msqId, msg, messageSize, 0) == -1) {
    log_message(LOG_LEVEL_ERROR,
                "[SEND] Error: Failed to send message. msqId: %d, Type: %ld, "
                "Content: %d, Error: %s (%d)",
                msqId, msg->mtype, msg->mtext, strerror(errno), errno);
    return -1;
  }

  log_message(LOG_LEVEL_INFO,
              "[SEND] Success: Message sent. msqId: %d, Type: %ld, Content: %d",
              msqId, msg->mtype, msg->mtext);
  return 0;
}

int receiveMessage(int msqId, Message *msg, long msgType, int flags) {
  log_message(LOG_LEVEL_DEBUG,
              "[RECEIVE] Attempting to receive message. msqId: %d, Expected "
              "Type: %ld, Flags: %d",
              msqId, msgType, flags);

  while (keepRunning) {
    ssize_t result =
        msgrcv(msqId, msg, sizeof(*msg) - sizeof(long), msgType, flags);

    if (result == -1) {
      if (errno == EINTR) {
        log_message(
            LOG_LEVEL_INFO,
            "[RECEIVE] Interrupted by signal, checking if should terminate.");
        if (!keepRunning) {
          log_message(LOG_LEVEL_INFO,
                      "[RECEIVE] Terminating due to signal interruption.");
          break;
        }
        continue;
      } else {
        log_message(LOG_LEVEL_ERROR,
                    "[RECEIVE] Error: Failed to receive message. msqId: %d, "
                    "Expected Type: %ld, Error: %s (%d)",
                    msqId, msgType, strerror(errno), errno);
        return -1;
      }
    } else {
      log_message(LOG_LEVEL_INFO,
                  "[RECEIVE] Success: Message received. msqId: %d, Type: %ld, "
                  "Content: %d",
                  msqId, msg->mtype, msg->mtext);
      return 0;
    }
  }
  return -1;
}

void cleanupSharedResources(void) {
  if (simClock) {
    if (detachSharedMemory((void **)&simClock, "Simulated Clock") == 0) {
      log_message(LOG_LEVEL_INFO,
                  "Detached from Simulated Clock shared memory.");
    }
    simClock = NULL;
  }

  if (actualTime) {
    if (detachSharedMemory((void **)&actualTime, "Actual Time") == 0) {
      log_message(LOG_LEVEL_INFO, "Detached from Actual Time shared memory.");
    }
    actualTime = NULL;
  }
}

void initializeSemaphore(void) {
  clockSem = sem_open(clockSemName, 0);
  if (clockSem == SEM_FAILED) {
    log_message(LOG_LEVEL_ERROR, "Failed to create or open semaphore: %s",
                strerror(errno));
    exit(EXIT_FAILURE);
  }
}

void initializeSimulatedClock(void) {
  key_t simClockKey = ftok(SHM_PATH, SHM_PROJ_ID_SIM_CLOCK);
  if (simClockKey == -1) {
    log_message(LOG_LEVEL_ERROR,
                "Failed to generate key for Simulated Clock: %s",
                strerror(errno));
    exit(EXIT_FAILURE);
  }

  simulatedTimeShmId =
      shmget(simClockKey, sizeof(SimulatedClock), IPC_CREAT | 0666);
  if (simulatedTimeShmId < 0) {
    log_message(
        LOG_LEVEL_ERROR,
        "Failed to create or open shared memory for Simulated Clock: %s",
        strerror(errno));
    exit(EXIT_FAILURE);
  }

  simClock = shmat(simulatedTimeShmId, NULL, 0);
  if (simClock == (void *)-1) {
    log_message(LOG_LEVEL_ERROR,
                "Failed to attach to shared memory for Simulated Clock: %s",
                strerror(errno));
    exit(EXIT_FAILURE);
  }
}

void initializeActualTime(void) {
  key_t actualTimeKey = ftok(SHM_PATH, SHM_PROJ_ID_ACT_TIME);
  if (actualTimeKey == -1) {
    log_message(LOG_LEVEL_ERROR, "Failed to generate key for Actual Time: %s",
                strerror(errno));
    exit(EXIT_FAILURE);
  }

  actualTimeShmId = shmget(actualTimeKey, sizeof(ActualTime), IPC_CREAT | 0666);
  if (actualTimeShmId < 0) {
    log_message(LOG_LEVEL_ERROR,
                "Failed to create or open shared memory for Actual Time: %s",
                strerror(errno));
    exit(EXIT_FAILURE);
  }

  actualTime = shmat(actualTimeShmId, NULL, 0);
  if (actualTime == (void *)-1) {
    log_message(LOG_LEVEL_ERROR,
                "Failed to attach to shared memory for Actual Time: %s",
                strerror(errno));
    exit(EXIT_FAILURE);
  }
}

void initializeProcessTable(void) {
  key_t processTableKey =
      getSharedMemoryKey(SHM_PATH, SHM_PROJ_ID_PROCESS_TABLE);
  processTableShmId = shmget(
      processTableKey, DEFAULT_MAX_PROCESSES * sizeof(PCB), IPC_CREAT | 0666);
  if (processTableShmId < 0) {
    log_message(LOG_LEVEL_ERROR,
                "Failed to create shared memory for processTable: %s",
                strerror(errno));
    exit(EXIT_FAILURE);
  }

  processTable = (PCB *)shmat(processTableShmId, NULL, 0);
  if (processTable == (void *)-1) {
    log_message(LOG_LEVEL_ERROR,
                "Failed to attach to processTable shared memory: %s",
                strerror(errno));
    exit(EXIT_FAILURE);
  }
}

void initializeResourceTable(void) {
  key_t resourceTableKey =
      getSharedMemoryKey(SHM_PATH, SHM_PROJ_ID_RESOURCE_TABLE);
  resourceTableShmId =
      shmget(resourceTableKey, sizeof(ResourceDescriptor), IPC_CREAT | 0666);
  if (resourceTableShmId < 0) {
    log_message(LOG_LEVEL_ERROR,
                "Failed to create shared memory for resourceTable: %s",
                strerror(errno));
    exit(EXIT_FAILURE);
  }

  resourceTable = (ResourceDescriptor *)shmat(resourceTableShmId, NULL, 0);
  if (resourceTable == (void *)-1) {
    log_message(LOG_LEVEL_ERROR,
                "Failed to attach to resourceTable shared memory: %s",
                strerror(errno));
    exit(EXIT_FAILURE);
  }

  for (int i = 0; i < MAX_RESOURCES; i++) {
    resourceTable->total[i] = INSTANCES_PER_RESOURCE;
    resourceTable->available[i] = INSTANCES_PER_RESOURCE;
    for (int j = 0; j < MAX_USER_PROCESSES; j++) {
      resourceTable->allocated[j][i] = 0;
    }
  }
}

void initializeResourceDescriptors(ResourceDescriptor *rd) {
  if (rd == NULL) {
    log_message(
        LOG_LEVEL_ERROR,
        "Resource Descriptor initialization failed: Null pointer provided.");
    return;
  }

  for (int i = 0; i < MAX_RESOURCES; i++) {

    rd->total[i] = INSTANCES_PER_RESOURCE;
    rd->available[i] = INSTANCES_PER_RESOURCE;

    for (int j = 0; j < MAX_USER_PROCESSES; j++) {
      rd->allocated[j][i] = 0;
    }
  }

  log_message(LOG_LEVEL_INFO, "Resource Descriptors successfully initialized.");
}

void initializeSharedResources(void) {
  msqId = initMessageQueue();

  initializeSemaphore();

  initializeSimulatedClock();

  initializeActualTime();

  initializeProcessTable();

  initializeResourceTable();

  log_message(LOG_LEVEL_INFO,
              "All shared resources have been successfully initialized.");
}

int requestResource(int resourceType, int quantity, int pid) {
  if (resourceType < 0 || resourceType >= MAX_RESOURCES || quantity <= 0) {
    log_message(LOG_LEVEL_ERROR, "Invalid request parameters.");
    return -1;
  }

  pthread_mutex_lock(&resourceTableMutex);

  if (resourceTable->available[resourceType] >= quantity) {

    resourceTable->available[resourceType] -= quantity;
    resourceTable->allocated[pid][resourceType] += quantity;
    log_message(LOG_LEVEL_INFO, "Resource %d allocated to process %d.",
                resourceType, pid);

    pthread_mutex_unlock(&resourceTableMutex);

    return 0;
  } else {
    log_message(
        LOG_LEVEL_WARN,
        "Resource %d request by process %d cannot be satisfied currently.",
        resourceType, pid);

    pthread_mutex_unlock(&resourceTableMutex);

    return 1;
  }
}

int releaseResource(int resourceType, int quantity, int pid) {
  if (resourceType < 0 || resourceType >= MAX_RESOURCES || quantity <= 0) {
    log_message(LOG_LEVEL_ERROR, "Invalid release parameters.");
    return -1;
  }

  pthread_mutex_lock(&resourceTableMutex);

  if (resourceTable->allocated[pid][resourceType] >= quantity) {

    resourceTable->allocated[pid][resourceType] -= quantity;
    resourceTable->available[resourceType] += quantity;
    log_message(LOG_LEVEL_INFO, "Resource %d released by process %d.",
                resourceType, pid);

    pthread_mutex_unlock(&resourceTableMutex);

    return 0;
  } else {
    log_message(
        LOG_LEVEL_ERROR,
        "Process %d attempted to release more of resource %d than allocated.",
        pid, resourceType);

    pthread_mutex_unlock(&resourceTableMutex);

    return -1;
  }
}
