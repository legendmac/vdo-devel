/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef THREAD_REGISTRY_H
#define THREAD_REGISTRY_H

#include <linux/list.h>
#include <linux/spinlock.h>

/*
 * We don't expect this set to ever get really large, so a linked list
 * is adequate.
 */

struct thread_registry {
	struct list_head links;
	spinlock_t lock;
};

struct registered_thread {
	struct list_head links;
	const void *pointer;
	struct task_struct *task;
};

/**
 * Initialize a registry of threads and associated data pointers.
 *
 * @param  registry  The registry to initialize
 **/
void uds_initialize_thread_registry(struct thread_registry *registry);

/**
 * Register the current thread and associate it with a data pointer.
 *
 * This call will log messages if the thread is already registered.
 *
 * @param registry    The thread registry
 * @param new_thread  registered_thread structure to use for the current thread
 * @param pointer     The value to associate with the current thread
 **/
void uds_register_thread(struct thread_registry *registry,
			 struct registered_thread *new_thread,
			 const void *pointer);

/**
 * Remove the registration for the current thread.
 *
 * A message may be logged if the thread was not registered.
 *
 * @param  registry  The thread registry
 **/
void uds_unregister_thread(struct thread_registry *registry);

/**
 * Fetch a pointer that may have been registered for the current
 * thread. If the thread is not registered, a null pointer is returned.
 *
 * @param  registry  The thread registry
 *
 * @return  the registered pointer, if any, or NULL
 **/
const void *uds_lookup_thread(struct thread_registry *registry);

#endif /* THREAD_REGISTRY_H */