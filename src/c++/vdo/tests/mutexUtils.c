/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "mutexUtils.h"

#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include "memory-alloc.h"
#include "uds-threads.h"

#include "asyncLayer.h"
#include "testBIO.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

typedef void ClearHook(void);

typedef struct {
  BlockCondition *condition;
  ClearHook      *clearHook;
} HookContext;

typedef struct {
  struct vio *vio;
  bool        blockedAsBIO;
} FetchContext;

struct task_struct {
  pthread_t id;
  int state;
};

static struct mutex mutex = {
  .mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
};

static struct cond_var  condition;
static struct vio      *blockedVIO;
static bool             blockedAsBIO;
static HookContext      callbackEnqueueContext;
static HookContext      bioSubmitContext;
static pthread_key_t    taskKey;
static uint32_t         blockedThreadCount;

/**********************************************************************/
static void freeTask(void *task)
{
  UDS_FREE(task);
}

/**********************************************************************/
static void tearDownMutexUtils(void)
{
  pthread_key_delete(taskKey);
  uds_destroy_cond(&condition);
  uds_destroy_mutex(&mutex);
}

/**********************************************************************/
void initializeMutexUtils(void)
{
  blockedVIO         = NULL;
  blockedAsBIO       = false;
  blockedThreadCount = 0;

  // The mutex needs to be recursive.
  pthread_mutexattr_t attr;
  UDS_ASSERT_SUCCESS(pthread_mutexattr_init(&attr));
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
  UDS_ASSERT_SUCCESS(pthread_mutex_init(&mutex.mutex, &attr));
  UDS_ASSERT_SUCCESS(pthread_mutexattr_destroy(&attr));
  UDS_ASSERT_SUCCESS(uds_init_cond(&condition));
  UDS_ASSERT_SUCCESS(pthread_key_create(&taskKey, freeTask));
  registerTearDownAction(tearDownMutexUtils);
}

/**********************************************************************/
void lockMutex(void)
{
  uds_lock_mutex(&mutex);
}

/**
 * Unlock the mutex locked by lockMutex().
 **/
void unlockMutex(void)
{
  uds_unlock_mutex(&mutex);
}

/**********************************************************************/
bool runLocked(LockedMethod *method, void *context)
{
  uds_lock_mutex(&mutex);
  bool result = method(context);
  if (result) {
    uds_broadcast_cond(&condition);
  }
  uds_unlock_mutex(&mutex);
  return result;
}

/**
 * Set a state variable to true and send a broadcast.
 *
 * Implements LockedMethod.
 **/
static bool setState(void *context)
{
  *((bool *) context) = true;
  return true;
}

/**********************************************************************/
void signalState(bool *state)
{
  runLocked(setState, state);
}

/**
 * Reset a state variable (set it to false).
 *
 * Implements LockedMethod.
 **/
static bool resetState(void *context)
{
  *((bool *) context) = false;
  return false;
}

/**********************************************************************/
void clearState(bool *state)
{
  runLocked(resetState, state);
}

/**********************************************************************/
void broadcast(void)
{
  uds_lock_mutex(&mutex);
  uds_broadcast_cond(&condition);
  uds_unlock_mutex(&mutex);
}

/**********************************************************************/
bool checkCondition(WaitCondition *waitCondition, void *context)
{
  uds_lock_mutex(&mutex);
  bool result = waitCondition(context);
  uds_unlock_mutex(&mutex);
  return result;
}

/**********************************************************************/
void waitForCondition(WaitCondition *waitCondition, void *context)
{
  uds_lock_mutex(&mutex);
  while (!waitCondition(context)) {
    uds_wait_cond(&condition, &mutex);
  }
  uds_unlock_mutex(&mutex);
}

/**********************************************************************/
void runOnCondition(WaitCondition *waitCondition,
                    LockedMethod  *method,
                    void          *context)
{
  uds_lock_mutex(&mutex);
  while (!waitCondition(context)) {
    uds_wait_cond(&condition, &mutex);
  }
  if (method(context)) {
    uds_broadcast_cond(&condition);
  }
  uds_unlock_mutex(&mutex);
}

/**********************************************************************/
bool runIfCondition(WaitCondition *waitCondition,
                    LockedMethod  *method,
                    void          *context)
{
  uds_lock_mutex(&mutex);
  bool result = waitCondition(context);
  if (result && method(context)) {
    uds_broadcast_cond(&condition);
  }
  uds_unlock_mutex(&mutex);
  return result;
}

/**********************************************************************/
bool checkState(bool *state)
{
  uds_lock_mutex(&mutex);
  // It just so happens that state is a bool, so we can just return it instead
  // of needing a more complicated check.
  bool result = *state;
  uds_unlock_mutex(&mutex);
  return result;
}

/**********************************************************************/
void waitForState(bool *state)
{
  uds_lock_mutex(&mutex);
  while (!*state) {
    uds_wait_cond(&condition, &mutex);
  }
  uds_unlock_mutex(&mutex);
}

/**********************************************************************/
void waitForStateAndClear(bool *state)
{
  uds_lock_mutex(&mutex);
  while (!*state) {
    uds_wait_cond(&condition, &mutex);
  }
  *state = false;
  uds_unlock_mutex(&mutex);
}

/**********************************************************************/
void waitForNotNull(void **ptr)
{
  uds_lock_mutex(&mutex);
  while (*ptr == NULL) {
    uds_wait_cond(&condition, &mutex);
  }
  uds_unlock_mutex(&mutex);
}

/**
 * Block a vio if we should. The mutex must be held when calling this method.
 *
 * @param vio             The vio to check
 * @param blockCondition  The condition to use to determine whether this vio
 *                        should be blocked
 * @param context         The context for the condition
 *
 * @return <code>true</code> if the VIO was blocked
 **/
static bool attemptVIOBlock(struct vio     *vio,
                            BlockCondition *blockCondition,
                            void           *context)
{
  if ((blockCondition == NULL) || blockCondition(&vio->completion, context)) {
    CU_ASSERT_PTR_NULL(blockedVIO);
    blockedVIO = vio;
    return true;
  }

  return false;
}

/**********************************************************************/
void blockVIOOnCondition(struct vio     *vio,
                         BlockCondition *blockCondition,
                         void           *context)
{
  uds_lock_mutex(&mutex);
  if (attemptVIOBlock(vio, blockCondition, context)) {
    uds_broadcast_cond(&condition);
  }
  uds_unlock_mutex(&mutex);
}

/**
 * Attempt to block a vio.
 *
 * @param vio          The vio
 * @param hookContext  The context in which to determine whether or not to
 *                     block the vio
 *
 * @return <code>true</code> if the VIO was blocked
 **/
static bool blockVIOLocked(struct vio *vio, HookContext *hookContext)
{
  if (!attemptVIOBlock(vio, hookContext->condition, NULL)) {
    return false;
  }

  if (hookContext->clearHook != NULL) {
    hookContext->clearHook();
  }

  uds_broadcast_cond(&condition);

  return true;
}

/**
 * Implements CompletionHook.
 **/
static bool blockVIOCompletionHook(struct vdo_completion *completion) {
  if (!is_vio(completion)) {
    return true;
  }

  uds_lock_mutex(&mutex);
  bool wasBlocked = blockVIOLocked(as_vio(completion),
                                   &callbackEnqueueContext);
  uds_unlock_mutex(&mutex);
  return !wasBlocked;
}

/*
 * Implements ClearHook.
 */
static void removeBlockVIOHook(void)
{
  removeCompletionEnqueueHook(blockVIOCompletionHook);
}

/**********************************************************************/
void addBlockVIOCompletionEnqueueHook(BlockCondition *condition, bool takeOut)
{
  callbackEnqueueContext = (HookContext) {
    .condition = condition,
    .clearHook = (takeOut ? removeBlockVIOHook : NULL),
  };
  addCompletionEnqueueHook(blockVIOCompletionHook);
}


/**********************************************************************/
void setBlockVIOCompletionEnqueueHook(BlockCondition *condition, bool takeOut)
{
  callbackEnqueueContext = (HookContext) {
    .condition = condition,
    .clearHook = (takeOut ? clearCompletionEnqueueHooks : NULL),
  };
  setCompletionEnqueueHook(blockVIOCompletionHook);
}

/**
 * Implements BioSubmiHook.
 **/
static bool blockBIOSubmitHook(struct bio *bio)
{
  uds_lock_mutex(&mutex);
  bool wasBlocked = blockVIOLocked(bio->bi_private, &bioSubmitContext);
  if (wasBlocked) {
    blockedAsBIO = true;
  }
  uds_unlock_mutex(&mutex);
  return !wasBlocked;
}

/**********************************************************************/
void setBlockBIO(BlockCondition *condition, bool takeOut)
{
  bioSubmitContext = (HookContext) {
    .condition = condition,
    .clearHook = (takeOut ? clearBIOSubmitHook : NULL),
  };
  setBIOSubmitHook(blockBIOSubmitHook);
}

/**********************************************************************/
void blockVIO(struct vio *vio)
{
  blockVIOOnCondition(vio, NULL, NULL);
}

/**********************************************************************/
void waitForBlockedVIO(void)
{
  waitForNotNull((void **) &blockedVIO);
}

/**
 * Check for a blocked VIO.
 *
 * Implements WaitCondition.
 **/
static bool checkForBlockedVIO(void *context __attribute__((unused)))
{
  return (blockedVIO != NULL);
}

/**
 * Get the blocked VIO and reset to block another.
 *
 * Implements LockedMethod.
 **/
static bool fetchBlockedVIO(void *context)
{
  FetchContext *fetchContext = context;
  fetchContext->vio = UDS_FORGET(blockedVIO);
  fetchContext->blockedAsBIO = blockedAsBIO;
  blockedAsBIO = false;
  return false;
}

/**********************************************************************/
struct vio *getBlockedVIO(void)
{
  FetchContext fetchContext;
  runOnCondition(checkForBlockedVIO, fetchBlockedVIO, &fetchContext);
  return fetchContext.vio;
}

/**********************************************************************/
void releaseBlockedVIO(void)
{
  FetchContext fetchContext;
  runOnCondition(checkForBlockedVIO, fetchBlockedVIO, &fetchContext);
  if (fetchContext.blockedAsBIO) {
    reallyEnqueueBIO(fetchContext.vio->bio);
  } else {
    reallyEnqueueVIO(fetchContext.vio);
  }
}

/**********************************************************************/
void assertNoBlockedVIOs(void)
{
  uds_lock_mutex(&mutex);
  CU_ASSERT_PTR_NULL(blockedVIO);
  uds_unlock_mutex(&mutex);
}

//////////////////////////////////////////////////////////////////////////
// Implementation of struct completion methods from linux/completion.h. //
//////////////////////////////////////////////////////////////////////////

/**********************************************************************/
void init_completion(struct completion *completion)
{
  VDO_ASSERT_SUCCESS(uds_init_mutex(&completion->mutex));
  VDO_ASSERT_SUCCESS(uds_init_cond(&completion->condition));
  completion->done = false;
}

/**********************************************************************/
void reinit_completion(struct completion *completion)
{
  completion->done = false;
}

/**********************************************************************/
void wait_for_completion(struct completion *completion)
{
  uds_lock_mutex(&completion->mutex);
  while (!completion->done) {
    uds_wait_cond(&completion->condition, &completion->mutex);
  }
  uds_unlock_mutex(&completion->mutex);
}

/**********************************************************************/
void complete(struct completion *completion)
{
  uds_lock_mutex(&completion->mutex);
  completion->done = true;
  uds_broadcast_cond(&completion->condition);
  uds_unlock_mutex(&completion->mutex);
}

/**********************************************************************/
bool checkBlockedThreadCount(void *context)
{
  return (blockedThreadCount == *((uint32_t *) context));
}

///////////////////////////////////////////////////////////////////////////
// Implementation of sleep and wake from linux/wait.h and linux/sched.h  //
///////////////////////////////////////////////////////////////////////////

void init_waitqueue_head(wait_queue_head_t *head)
{
  VDO_ASSERT_SUCCESS(uds_init_mutex(&head->lock));
  INIT_LIST_HEAD(&head->head);
}

/**********************************************************************/
void io_schedule(void)
{
  struct task_struct *task = getCurrentTaskStruct();
  lockMutex();

  CU_ASSERT(task->state != TASK_RUNNING);

  blockedThreadCount++;
  uds_broadcast_cond(&condition);
  while (task->state != TASK_RUNNABLE) {
    uds_wait_cond(&condition, &mutex);
  }

  current->state = TASK_RUNNING;
  blockedThreadCount--;
  uds_broadcast_cond(&condition);

  unlockMutex();
}

/**********************************************************************/
void wake_up_nr(wait_queue_head_t *head, int32_t count)
{
  uds_lock_mutex(&head->lock);
  struct list_head *entry;
  list_for_each(entry, &head->head) {
    struct task_struct *task = container_of(entry,
                                            struct wait_queue_entry,
                                            entry)->private;
    if (task->state == TASK_UNINTERRUPTIBLE) {
      task->state = TASK_RUNNABLE;
      if (--count == 0) {
        break;
      }
    }
  }
  uds_unlock_mutex(&head->lock);

  broadcast();
}

/**********************************************************************/
void prepare_to_wait_exclusive(wait_queue_head_t *queue,
			       struct wait_queue_entry *entry,
			       int state)
{
  uds_lock_mutex(&queue->lock);
  list_add_tail(&entry->entry, &queue->head);
  set_current_state(state);
  uds_unlock_mutex(&queue->lock);
}

/**********************************************************************/
void finish_wait(wait_queue_head_t *queue, struct wait_queue_entry *entry)
{
  uds_lock_mutex(&queue->lock);
  list_del_init(&entry->entry);
  uds_unlock_mutex(&queue->lock);
}

/**********************************************************************/
struct task_struct *getCurrentTaskStruct(void)
{
  struct task_struct *task = pthread_getspecific(taskKey);
  if (task == NULL) {
    VDO_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct task_struct, __func__, &task));
    VDO_ASSERT_SUCCESS(pthread_setspecific(taskKey, task));
    task->state = TASK_RUNNING;
    task->id = pthread_self();
  }

  return task;
}

/**********************************************************************/
static bool setCurrentStateLocked(void *context) {
  struct task_struct *task = getCurrentTaskStruct();
  task->state = *((int *) context);
  return true;
}

/**********************************************************************/
void set_current_state(int state_value)
{
  runLocked(setCurrentStateLocked, &state_value);
}
