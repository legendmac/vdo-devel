/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef COMPLETION_H
#define COMPLETION_H

#include "permassert.h"

#include "kernel-types.h"
#include "status-codes.h"
#include "types.h"
#include "workQueue.h"

enum vdo_completion_type {
	/* Keep VDO_UNSET_COMPLETION_TYPE at the top. */
	VDO_UNSET_COMPLETION_TYPE,

	/*
	 * Keep this block in sorted order. If you add or remove an entry, be
	 * sure to update the corresponding list in completion.c.
	 */
	VDO_ACTION_COMPLETION,
	VDO_ADMIN_COMPLETION,
	VDO_BLOCK_ALLOCATOR_COMPLETION,
	VDO_BLOCK_MAP_RECOVERY_COMPLETION,
	VDO_DATA_VIO_POOL_COMPLETION,
	VDO_DEDUPE_INDEX_COMPLETION,
	VDO_EXTENT_COMPLETION,
	VDO_FLUSH_COMPLETION,
	VDO_FLUSH_NOTIFICATION_COMPLETION,
	VDO_GENERATION_FLUSHED_COMPLETION,
	VDO_LOCK_COUNTER_COMPLETION,
	VDO_PAGE_COMPLETION,
	VDO_PARTITION_COPY_COMPLETION,
	VDO_READ_ONLY_MODE_COMPLETION,
	VDO_READ_ONLY_REBUILD_COMPLETION,
	VDO_RECOVERY_COMPLETION,
	VDO_REFERENCE_COUNT_REBUILD_COMPLETION,
	VDO_SLAB_SCRUBBER_COMPLETION,
	VDO_SUB_TASK_COMPLETION,
	VDO_SYNC_COMPLETION,
	VIO_COMPLETION,

#ifndef __KERNEL__
	/*
	 * Keep this block in sorted order. If you add or remove an entry, be
	 * sure to update the corresponding list in completion.c.
	 */
	VDO_TEST_COMPLETION, /* each unit test may define its own */
	VDO_WRAPPING_COMPLETION,
#endif /* not __KERNEL__ */

	/* Keep VDO_MAX_COMPLETION_TYPE at the bottom. */
	VDO_MAX_COMPLETION_TYPE
} __packed;

/**
 * An asynchronous VDO operation.
 *
 * @param completion    the completion of the operation
 **/
typedef void vdo_action(struct vdo_completion *completion);

struct vdo_completion {
	/** The type of completion this is */
	enum vdo_completion_type type;

	/**
	 * <code>true</code> once the processing of the operation is complete.
	 * This flag should not be used by waiters external to the VDO base as
	 * it is used to gate calling the callback.
	 **/
	bool complete;

	/**
	 * If true, queue this completion on the next callback invocation, even
	 *if it is already running on the correct thread.
	 **/
	bool requeue;

	/** The ID of the thread which should run the next callback */
	thread_id_t callback_thread_id;

	/** The result of the operation */
	int result;

	/** The VDO on which this completion operates */
	struct vdo *vdo;

	/** The callback which will be called once the operation is complete */
	vdo_action *callback;

	/** Callback which, if set, will be called if an error result is set */
	vdo_action *error_handler;

	/** The parent object, if any, that spawned this completion */
	void *parent;

	/** Entry link for lock-free work queue */
	struct funnel_queue_entry work_queue_entry_link;
	enum vdo_completion_priority priority;
	struct vdo_work_queue *my_queue;
	uint64_t enqueue_time;
};

/**
 * Actually run the callback. This function must be called from the correct
 * callback thread.
 **/
static inline void
vdo_run_completion_callback(struct vdo_completion *completion)
{
	if ((completion->result != VDO_SUCCESS) &&
	    (completion->error_handler != NULL)) {
		completion->error_handler(completion);
		return;
	}

	completion->callback(completion);
}

void vdo_set_completion_result(struct vdo_completion *completion, int result);

void vdo_initialize_completion(struct vdo_completion *completion,
			       struct vdo *vdo,
			       enum vdo_completion_type type);

void vdo_reset_completion(struct vdo_completion *completion);

void
vdo_invoke_completion_callback_with_priority(struct vdo_completion *completion,
					     enum vdo_completion_priority priority);

/**
 * Invoke the callback of a completion. If called on the correct thread (i.e.
 * the one specified in the completion's callback_thread_id field), the
 * completion will be run immediately. Otherwise, the completion will be
 * enqueued on the correct callback thread.
 *
 * @param completion  The completion whose callback is to be invoked
 **/
static inline void
vdo_invoke_completion_callback(struct vdo_completion *completion)
{
	vdo_invoke_completion_callback_with_priority(completion,
						     VDO_WORK_Q_DEFAULT_PRIORITY);
}

void vdo_continue_completion(struct vdo_completion *completion, int result);

void vdo_complete_completion(struct vdo_completion *completion);

/**
 * Finish a completion.
 *
 * @param completion The completion to finish
 * @param result     The result of the completion (will not mask older errors)
 **/
static inline void vdo_finish_completion(struct vdo_completion *completion,
					 int result)
{
	vdo_set_completion_result(completion, result);
	vdo_complete_completion(completion);
}

void vdo_finish_completion_parent_callback(struct vdo_completion *completion);

void
vdo_preserve_completion_error_and_continue(struct vdo_completion *completion);

void vdo_noop_completion_callback(struct vdo_completion *completion);

int vdo_assert_completion_type(enum vdo_completion_type actual,
			       enum vdo_completion_type expected);

/**
 * Set the callback for a completion.
 *
 * @param completion  The completion
 * @param callback    The callback to register
 * @param thread_id   The ID of the thread on which the callback should run
 **/
static inline void
vdo_set_completion_callback(struct vdo_completion *completion,
			    vdo_action *callback,
			    thread_id_t thread_id)
{
	completion->callback = callback;
	completion->callback_thread_id = thread_id;
}

/**
 * Set the callback for a completion and invoke it immediately.
 *
 * @param completion  The completion
 * @param callback    The callback to register
 * @param thread_id   The ID of the thread on which the callback should run
 **/
static inline void
vdo_launch_completion_callback(struct vdo_completion *completion,
			       vdo_action *callback,
			       thread_id_t thread_id)
{
	vdo_set_completion_callback(completion, callback, thread_id);
	vdo_invoke_completion_callback(completion);
}

/**
 * Set the callback and parent for a completion.
 *
 * @param completion  The completion
 * @param callback    The callback to register
 * @param thread_id   The ID of the thread on which the callback should run
 * @param parent      The new parent of the completion
 **/
static inline void
vdo_set_completion_callback_with_parent(struct vdo_completion *completion,
					vdo_action *callback,
					thread_id_t thread_id,
					void *parent)
{
	vdo_set_completion_callback(completion, callback, thread_id);
	completion->parent = parent;
}

/**
 * Set the callback and parent for a completion and invoke the callback
 * immediately.
 *
 * @param completion  The completion
 * @param callback    The callback to register
 * @param thread_id   The ID of the thread on which the callback should run
 * @param parent      The new parent of the completion
 **/
static inline void
vdo_launch_completion_callback_with_parent(struct vdo_completion *completion,
					   vdo_action *callback,
					   thread_id_t thread_id,
					   void *parent)
{
	vdo_set_completion_callback_with_parent(completion, callback,
						thread_id, parent);
	vdo_invoke_completion_callback(completion);
}

/**
 * Prepare a completion for launch. Reset it, and then set its callback, error
 * handler, callback thread, and parent.
 *
 * @param completion     The completion
 * @param callback       The callback to register
 * @param error_handler	 The error handler to register
 * @param thread_id      The ID of the thread on which the callback should run
 * @param parent         The new parent of the completion
 **/
static inline void vdo_prepare_completion(struct vdo_completion *completion,
					  vdo_action *callback,
					  vdo_action *error_handler,
					  thread_id_t thread_id,
					  void *parent)
{
	vdo_reset_completion(completion);
	vdo_set_completion_callback_with_parent(completion, callback,
						thread_id, parent);
	completion->error_handler = error_handler;
}

/**
 * Prepare a completion for launch ensuring that it will always be requeued.
 * Reset it, and then set its callback, error handler, callback thread, and
 * parent.
 *
 * @param completion     The completion
 * @param callback       The callback to register
 * @param error_handler  The error handler to register
 * @param thread_id      The ID of the thread on which the callback should run
 * @param parent         The new parent of the completion
 **/
static inline void
vdo_prepare_completion_for_requeue(struct vdo_completion *completion,
				   vdo_action *callback,
				   vdo_action *error_handler,
				   thread_id_t thread_id,
				   void *parent)
{
	vdo_prepare_completion(completion,
			       callback,
			       error_handler,
			       thread_id,
			       parent);
	completion->requeue = true;
}

/**
 * Prepare a completion for launch which will complete its parent when
 * finished.
 *
 * @param completion  The completion
 * @param parent      The parent to complete
 **/
static inline void
vdo_prepare_completion_to_finish_parent(struct vdo_completion *completion,
					struct vdo_completion *parent)
{
	vdo_prepare_completion(completion,
			       vdo_finish_completion_parent_callback,
			       vdo_finish_completion_parent_callback,
			       parent->callback_thread_id,
			       parent);
}

void
vdo_enqueue_completion_with_priority(struct vdo_completion *completion,
				     enum vdo_completion_priority priority);

/**
 * A function to enqueue a vdo_completion to run on the thread specified by its
 * callback_thread_id field at default priority.
 *
 * @param completion  The completion to be enqueued
 **/
static inline void vdo_enqueue_completion(struct vdo_completion *completion)
{
	vdo_enqueue_completion_with_priority(completion,
					     VDO_WORK_Q_DEFAULT_PRIORITY);
}

#endif /* COMPLETION_H */