/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for the GigaDevice GD32VW55x SDK OS wrapper (the sys_*
 * facade declared in rtos/rtos_wrapper/wrapper_os.h).
 *
 * Validated HAL: SDK V1.0.3g (2026-04-23, commit 945c6e2).  ilp32f ABI.
 *
 * The prebuilt Wi-Fi and BLE libraries link exclusively against these
 * symbols; semantics follow wrapper_freertos.c (and the hardware-validated
 * reference port), including the quirks:
 *
 *  - task stack sizes arrive in 32-bit WORDS (converted to bytes here)
 *  - sys_sema_down()/sys_task_wait()/blocking sys_queue_fetch() treat
 *    timeout 0 as "wait forever", while the int-timeout APIs
 *    (sys_mutex_try_get, sys_queue_write/read, sys_task_wait_notification)
 *    use -1 = forever and 0 = no wait
 *  - sys_queue_write/read return 0 on success, 1 on failure;
 *    sys_timer_stop returns 1 on success
 *  - sys_task_create returns the task handle; the per-task mailbox is
 *    looked up internally (k_tid_t table here, TLS in FreeRTOS)
 *  - sys_enter_critical() masks by ECLIC level threshold (MTH), never by
 *    disabling all interrupts: the radio sources run at level 8 with
 *    microsecond deadlines and must keep firing
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/irq.h>
#include <zephyr/random/random.h>

#include <string.h>
#include <stdlib.h>

#include "wrapper_os.h"

/* SDK priorities are 0..31 (higher = higher priority, idle = 0).  Zephyr
 * preemptive priorities are 0..N-1 (LOWER = higher).  Map linearly keeping
 * direction; CONFIG_NUM_PREEMPT_PRIORITIES must cover the range (the
 * driver Kconfig raises it to >= 33).
 */

#define PRIO_SDK2Z(p) (((int)(p) >= 31) ? 0 : (31 - (int)(p)))

#define MAX_TASKS 16 /* Concurrent sys_task_create() tasks */

#if defined(CONFIG_WIFI_GDWIFI_TRACE)
#define WTRACE(fmt, ...)                                                                           \
	do {                                                                                       \
		static int n_;                                                                     \
		if (n_ < 60) {                                                                     \
			n_++;                                                                      \
			printk("gdwifi: " fmt "\n", ##__VA_ARGS__);                                \
		}                                                                                  \
	} while (0)
#else
#define WTRACE(fmt, ...)
#endif

/* Per-task bookkeeping: thread + mailbox + notification semaphore */

struct z_task_s {
	k_tid_t tid;
	struct k_thread thread;
	k_thread_stack_t *stack;
	struct k_msgq *mbox; /* NULL if queue_size == 0 */
	struct k_sem notif;  /* Counting notification */
	task_func_t func;
	void *ctx;
	char name[16];
};

/* Timer: k_work_delayable so the user callback runs in thread context
 * (system workqueue), as the FreeRTOS timer daemon does.
 */

struct z_timer_s {
	struct k_work_delayable dwork;
	uint32_t delay_ms;
	uint8_t periodic;
	volatile uint8_t active;
	timer_func_t func;
	void *arg;
};

static struct z_task_s *g_tasks[MAX_TASKS];
static struct k_spinlock g_tasks_lock;

/* Nestable global critical section by ECLIC level threshold */

static volatile uint32_t g_crit_nest;
static struct k_spinlock g_crit_lock;

#define GDWIFI_ECLIC_MTH  (0xd2000000UL + 0x000bUL)
#define GDWIFI_MTH_KERNEL 0x10U /* masks level <= kernel, radio (8<<4) passes */

/** Power-save mode requested by the SDK through sys_ps_set() */
uint8_t sys_ps_mode = SYS_PS_OFF;

/* Helpers */

static struct z_task_s *task_slot_find(k_tid_t tid)
{
	for (int i = 0; i < MAX_TASKS; i++) {
		if (g_tasks[i] != NULL && g_tasks[i]->tid == tid) {
			return g_tasks[i];
		}
	}

	return NULL;
}

static struct z_task_s *task_slot_from_handle(void *task)
{
	return task_slot_find(task == NULL ? k_current_get() : (k_tid_t)task);
}

static k_timeout_t timeout_int_ms(int timeout_ms)
{
	if (timeout_ms < 0) {
		return K_FOREVER;
	} else if (timeout_ms == 0) {
		return K_NO_WAIT;
	}

	return K_MSEC(timeout_ms);
}

/* Heap */

/**
 * @brief Allocate memory from the Zephyr system heap.
 *
 * @param size Number of bytes.
 * @return Pointer to the block, or NULL when the heap is exhausted.
 */
void *sys_malloc(size_t size)
{
	return k_malloc(size);
}

/**
 * @brief Allocate zero-filled memory from the Zephyr system heap.
 *
 * @param count Number of elements.
 * @param size Size of one element in bytes.
 * @return Pointer to the block, or NULL when the heap is exhausted.
 */
void *sys_calloc(size_t count, size_t size)
{
	return k_calloc(count, size);
}

/**
 * @brief Resize a block allocated by sys_malloc() or sys_calloc().
 *
 * @param mem Block to resize, or NULL to allocate.
 * @param size New size in bytes.
 * @return Pointer to the resized block, or NULL on failure.
 */
void *sys_realloc(void *mem, size_t size)
{
	return k_realloc(mem, size);
}

/**
 * @brief Free a block allocated by the sys_* heap functions.
 *
 * @param ptr Block to free; NULL is ignored.
 */
void sys_mfree(void *ptr)
{
	k_free(ptr);
}

/**
 * @brief Free heap estimate the SDK uses for diagnostics and low-memory
 * warnings only. The Zephyr heap has no cheap free-size query, so a
 * constant is returned.
 *
 * @return Free heap estimate in bytes.
 */
int32_t sys_free_heap_size(void)
{
	/* The SDK only uses this for diagnostics and low-memory warnings */
	return 64 * 1024;
}

/**
 * @brief Minimum free heap estimate; see sys_free_heap_size().
 *
 * @return Free heap estimate in bytes.
 */
int32_t sys_min_free_heap_size(void)
{
	return sys_free_heap_size();
}

/**
 * @brief Allocation granularity reported to the SDK.
 *
 * @return Block size in bytes.
 */
uint16_t sys_heap_block_size(void)
{
	return 16;
}

/**
 * @brief Report the heap size to the SDK.
 *
 * @param total_size Receives the system heap size, or NULL.
 * @param free_size Receives the free heap estimate, or NULL.
 * @param min_free_size Receives the minimum free heap estimate, or NULL.
 */
void sys_heap_info(int *total_size, int *free_size, int *min_free_size)
{
	if (total_size) {
		*total_size = K_HEAP_MEM_POOL_SIZE;
	}

	if (free_size) {
		*free_size = sys_free_heap_size();
	}

	if (min_free_size) {
		*min_free_size = sys_free_heap_size();
	}
}

/**
 * @brief Donate a RAM region to the heap. Ignored: the Zephyr system heap
 * cannot grow at runtime, the driver Kconfig sizes it for the radio.
 *
 * @param start Region start address.
 * @param size Region size in bytes.
 */
void sys_add_heap_region(uint32_t start, uint32_t size)
{
	/* The BLE bring-up donates its RAM region; the Zephyr system heap
	 * cannot grow at runtime, so this is intentionally ignored (the
	 * driver Kconfig sizes the pool for it instead).
	 */
}

/**
 * @brief Withdraw a RAM region from the heap; no-op, see sys_add_heap_region().
 *
 * @param start Region start address.
 * @param size Region size in bytes.
 */
void sys_remove_heap_region(uint32_t start, uint32_t size)
{
}

/* Memory manipulation */

/**
 * @brief Fill memory with a byte value.
 *
 * @param s Destination.
 * @param c Byte value.
 * @param count Number of bytes.
 */
void sys_memset(void *s, uint8_t c, uint32_t count)
{
	memset(s, c, count);
}

/**
 * @brief Copy memory between non-overlapping buffers.
 *
 * @param des Destination.
 * @param src Source.
 * @param n Number of bytes.
 */
void sys_memcpy(void *des, const void *src, uint32_t n)
{
	memcpy(des, src, n);
}

/**
 * @brief Copy memory between possibly overlapping buffers.
 *
 * @param des Destination.
 * @param src Source.
 * @param n Number of bytes.
 */
void sys_memmove(void *des, const void *src, uint32_t n)
{
	memmove(des, src, n);
}

/**
 * @brief Compare two buffers.
 *
 * @param buf1 First buffer.
 * @param buf2 Second buffer.
 * @param count Number of bytes.
 * @return 0 when equal, otherwise the sign of the first difference.
 */
int32_t sys_memcmp(const void *buf1, const void *buf2, uint32_t count)
{
	return memcmp(buf1, buf2, count);
}

/* Tasks */

static void task_trampoline(void *p1, void *p2, void *p3)
{
	struct z_task_s *t = p1;

	t->func(t->ctx);

	/* Entry returned: clean up as if sys_task_delete(NULL) */

	sys_task_delete(NULL);
}

/**
 * @brief Create and start an SDK task as a Zephyr thread.
 *
 * The stack and the optional mailbox are allocated from the system heap;
 * static_tcb and stack_base are ignored. The thread is registered in the
 * task table before it starts, so its entry can wait on the mailbox or
 * on notifications right away.
 *
 * @param static_tcb Ignored.
 * @param name Task name (up to 15 characters are kept).
 * @param stack_base Ignored.
 * @param stack_size Stack size in 32-bit words.
 * @param queue_size Mailbox depth in messages; 0 for no mailbox.
 * @param queue_item_size Mailbox message size in bytes.
 * @param priority SDK priority, 0 (lowest) to 31.
 * @param func Task entry.
 * @param ctx Argument passed to the entry.
 * @return Task handle, or NULL when an allocation fails.
 */
void *sys_task_create(void *static_tcb, const uint8_t *name, uint32_t *stack_base,
		      uint32_t stack_size, uint32_t queue_size, uint32_t queue_item_size,
		      uint32_t priority, task_func_t func, void *ctx)
{
	struct z_task_s *t;
	k_spinlock_key_t key;
	size_t stack_bytes;
	int i;

	t = k_calloc(1, sizeof(*t));
	if (t == NULL) {
		return NULL;
	}

	if (queue_size > 0) {
		t->mbox = k_malloc(sizeof(struct k_msgq));
		if (t->mbox == NULL ||
		    k_msgq_alloc_init(t->mbox, queue_item_size, queue_size) != 0) {
			WTRACE("mbox_create('%s' %ux%u) failed", (const char *)name,
			       (unsigned int)queue_size, (unsigned int)queue_item_size);
			k_free(t->mbox);
			k_free(t);
			return NULL;
		}
	}

	k_sem_init(&t->notif, 0, K_SEM_MAX_LIMIT);
	strncpy(t->name, (const char *)name, sizeof(t->name) - 1);
	t->func = func;
	t->ctx = ctx;

	/* stack_size arrives in 32-bit words (FreeRTOS convention) */

	stack_bytes = stack_size * sizeof(uint32_t);

	t->stack = k_thread_stack_alloc(stack_bytes, 0);
	if (t->stack == NULL) {
		WTRACE("stack_alloc('%s' %u) failed", t->name, (unsigned int)stack_bytes);
		if (t->mbox) {
			k_msgq_cleanup(t->mbox);
			k_free(t->mbox);
		}

		k_free(t);
		return NULL;
	}

	/* Create suspended (K_FOREVER), register in the table, then start:
	 * the entry may immediately call sys_task_wait()/wait_notification
	 * and must find its slot.
	 */

	t->tid = k_thread_create(&t->thread, t->stack, stack_bytes, task_trampoline, t, NULL, NULL,
				 PRIO_SDK2Z(priority), 0, K_FOREVER);

	k_thread_name_set(t->tid, t->name);

	key = k_spin_lock(&g_tasks_lock);
	for (i = 0; i < MAX_TASKS; i++) {
		if (g_tasks[i] == NULL) {
			g_tasks[i] = t;
			break;
		}
	}

	k_spin_unlock(&g_tasks_lock, key);
	__ASSERT(i < MAX_TASKS, "gdwifi task table full");

	WTRACE("task '%s' prio=%d stack=%uB q=%ux%u", t->name, PRIO_SDK2Z(priority),
	       (unsigned int)stack_bytes, (unsigned int)queue_size, (unsigned int)queue_item_size);

	k_thread_start(t->tid);

	return t->tid;
}

/**
 * @brief Delete a task created by sys_task_create() and release its resources.
 * A task deleting itself aborts its thread and leaves its stack allocated.
 *
 * @param task Task handle, or NULL for the calling task.
 */
void sys_task_delete(void *task)
{
	struct z_task_s *t = task_slot_from_handle(task);
	bool self = (task == NULL) || ((k_tid_t)task == k_current_get());
	k_spinlock_key_t key;
	k_tid_t tid = NULL;

	if (t != NULL) {
		key = k_spin_lock(&g_tasks_lock);
		for (int i = 0; i < MAX_TASKS; i++) {
			if (g_tasks[i] == t) {
				g_tasks[i] = NULL;
				break;
			}
		}

		k_spin_unlock(&g_tasks_lock, key);

		tid = t->tid;

		if (!self) {
			k_thread_abort(tid);
		}

		if (t->mbox) {
			k_msgq_cleanup(t->mbox);
			k_free(t->mbox);
		}

		/* The stack cannot be freed while the thread runs on it;
		 * leak-free handling of self-delete: free it from the
		 * joining side.  In practice the SDK never deletes its
		 * long-lived service tasks, so keep it simple.
		 */

		if (!self) {
			k_thread_stack_free(t->stack);
		}

		k_free(t);
	}

	if (self) {
		k_thread_abort(k_current_get());
	}
}

/**
 * @brief Get the name of a task.
 *
 * @param task Task handle, or NULL for the calling task.
 * @return The name, or NULL when the task is unknown.
 */
char *sys_task_name_get(void *task)
{
	struct z_task_s *t = task_slot_from_handle(task);

	return t != NULL ? t->name : NULL;
}

/**
 * @brief List the tasks; not supported, writes an empty string.
 *
 * @param pwrite_buf Output buffer, or NULL.
 */
void sys_task_list(char *pwrite_buf)
{
	if (pwrite_buf) {
		*pwrite_buf = '\0';
	}
}

/**
 * @brief Wait for a message in the mailbox of the calling task.
 *
 * @param timeout_ms Timeout in milliseconds; 0 waits forever.
 * @param msg_ptr Receives the message.
 * @return OS_OK, OS_TIMEOUT, or OS_ERROR when the task has no mailbox.
 */
int32_t sys_task_wait(uint32_t timeout_ms, void *msg_ptr)
{
	struct z_task_s *t = task_slot_find(k_current_get());
	int ret;

	if (t == NULL || t->mbox == NULL) {
		WTRACE("wait: current task has NO MBOX");
		return OS_ERROR;
	}

	/* timeout 0 = wait forever (FreeRTOS wrapper convention here) */

	ret = k_msgq_get(t->mbox, msg_ptr, timeout_ms == 0 ? K_FOREVER : K_MSEC(timeout_ms));
	return (ret != 0) ? OS_TIMEOUT : OS_OK;
}

/**
 * @brief Post a message to the mailbox of a task without waiting.
 *
 * @param receiver_task Task handle, or NULL for the calling task.
 * @param msg_ptr Message to copy.
 * @param from_isr Ignored; the call never waits.
 * @return OS_OK, or OS_ERROR when the mailbox is full or missing.
 */
int32_t sys_task_post(void *receiver_task, void *msg_ptr, uint8_t from_isr)
{
	struct z_task_s *t = task_slot_from_handle(receiver_task);
	int ret;

	if (t == NULL || t->mbox == NULL) {
		WTRACE("post to %p NO MBOX", receiver_task);
		return OS_ERROR;
	}

	ret = k_msgq_put(t->mbox, msg_ptr, K_NO_WAIT);
	return (ret != 0) ? OS_ERROR : OS_OK;
}

/**
 * @brief Discard every message queued in the mailbox of a task.
 *
 * @param task Task handle, or NULL for the calling task.
 */
void sys_task_msg_flush(void *task)
{
	struct z_task_s *t = task_slot_from_handle(task);

	if (t != NULL && t->mbox != NULL) {
		k_msgq_purge(t->mbox);
	}
}

/**
 * @brief Count the messages queued in the mailbox of a task.
 *
 * @param task Task handle, or NULL for the calling task.
 * @param from_isr Ignored.
 * @return Number of queued messages, or OS_ERROR when the task has no mailbox.
 */
int32_t sys_task_msg_num(void *task, uint8_t from_isr)
{
	struct z_task_s *t = task_slot_from_handle(task);

	if (t == NULL || t->mbox == NULL) {
		return OS_ERROR;
	}

	return k_msgq_num_used_get(t->mbox);
}

/**
 * @brief Get the handle of the calling task.
 *
 * @return The handle of the current thread.
 */
os_task_t sys_current_task_handle_get(void)
{
	return (os_task_t)k_current_get();
}

/**
 * @brief Stack usage of the calling task; not tracked, a constant is returned.
 *
 * @param cur_sp Ignored.
 * @return 1024.
 */
int32_t sys_current_task_stack_depth(unsigned long cur_sp)
{
	return 1024;
}

/**
 * @brief Free stack of a task; not tracked, a constant is returned.
 *
 * @param task Ignored.
 * @return 1024.
 */
uint32_t sys_stack_free_get(void *task)
{
	return 1024;
}

/**
 * @brief Prepare task notifications; nothing to do, the notification
 * semaphore is initialized by sys_task_create().
 *
 * @param task Ignored.
 * @return 0.
 */
int sys_task_init_notification(void *task)
{
	return 0;
}

/**
 * @brief Wait for notifications sent to the calling task and consume them all.
 *
 * @param timeout Timeout in milliseconds, -1 to wait forever, 0 not to wait.
 * @return Number of notifications consumed, 0 on timeout.
 */
int sys_task_wait_notification(int timeout)
{
	struct z_task_s *t = task_slot_find(k_current_get());
	int count;

	if (t == NULL) {
		return 0;
	}

	if (k_sem_take(&t->notif, timeout_int_ms(timeout)) != 0) {
		return 0;
	}

	/* Drain remaining posts to emulate the counting "take all" */

	count = 1;
	while (k_sem_take(&t->notif, K_NO_WAIT) == 0) {
		count++;
	}

	return count;
}

/**
 * @brief Send a notification to a task.
 *
 * @param task Task handle, or NULL for the calling task.
 * @param isr Ignored; the call never waits.
 */
void sys_task_notify(void *task, bool isr)
{
	struct z_task_s *t = task_slot_from_handle(task);

	if (t != NULL) {
		k_sem_give(&t->notif);
	}
}

/**
 * @brief Tell whether a task with the given name exists.
 *
 * @param name Task name.
 * @return 1 when found, 0 otherwise.
 */
uint8_t sys_task_exist(const uint8_t *name)
{
	for (int i = 0; i < MAX_TASKS; i++) {
		if (g_tasks[i] != NULL &&
		    strncmp(g_tasks[i]->name, (const char *)name, sizeof(g_tasks[i]->name)) == 0) {
			return 1;
		}
	}

	return 0;
}

/**
 * @brief Set the priority of a task.
 *
 * @param task Task handle, or NULL for the calling task.
 * @param priority SDK priority, 0 (lowest) to 31.
 */
void sys_priority_set(void *task, os_prio_t priority)
{
	k_tid_t tid = (task == NULL) ? k_current_get() : (k_tid_t)task;

	k_thread_priority_set(tid, PRIO_SDK2Z(priority));
}

/**
 * @brief Get the priority of a task.
 *
 * @param task Task handle, or NULL for the calling task.
 * @return SDK priority, 0 (lowest) to 31.
 */
os_prio_t sys_priority_get(void *task)
{
	k_tid_t tid = (task == NULL) ? k_current_get() : (k_tid_t)task;
	int prio = k_thread_priority_get(tid);

	return (os_prio_t)(31 - prio);
}

/* Semaphores */

/**
 * @brief Allocate and initialize a counting semaphore.
 *
 * @param sema Receives the semaphore.
 * @param max_count Maximum count; 0 or less means unlimited.
 * @param init_count Initial count.
 * @return OS_OK, or OS_ERROR when the allocation fails.
 */
int32_t sys_sema_init_ext(os_sema_t *sema, int max_count, int init_count)
{
	struct k_sem *s = k_malloc(sizeof(struct k_sem));

	if (s == NULL) {
		return OS_ERROR;
	}

	k_sem_init(s, init_count, max_count > 0 ? max_count : K_SEM_MAX_LIMIT);
	*sema = s;
	return OS_OK;
}

/**
 * @brief Allocate and initialize an unlimited counting semaphore.
 *
 * @param sema Receives the semaphore.
 * @param init_val Initial count.
 * @return OS_OK, or OS_ERROR when the allocation fails.
 */
int32_t sys_sema_init(os_sema_t *sema, int32_t init_val)
{
	return sys_sema_init_ext(sema, K_SEM_MAX_LIMIT, init_val);
}

/**
 * @brief Free a semaphore.
 *
 * @param sema Semaphore; NULL or already freed is ignored.
 */
void sys_sema_free(os_sema_t *sema)
{
	if (sema != NULL && *sema != NULL) {
		k_free(*sema);
		*sema = NULL;
	}
}

/**
 * @brief Give a semaphore.
 *
 * @param sema Semaphore.
 */
void sys_sema_up(os_sema_t *sema)
{
	k_sem_give((struct k_sem *)*sema);
}

/**
 * @brief Give a semaphore from interrupt context.
 *
 * @param sema Semaphore.
 */
void sys_sema_up_from_isr(os_sema_t *sema)
{
	k_sem_give((struct k_sem *)*sema);
}

/**
 * @brief Take a semaphore.
 *
 * @param sema Semaphore.
 * @param timeout_ms Timeout in milliseconds; 0 waits forever.
 * @return OS_OK, or OS_TIMEOUT when the timeout expires.
 */
int32_t sys_sema_down(os_sema_t *sema, uint32_t timeout_ms)
{
	int ret;

	/* timeout 0 = wait forever here */

	ret = k_sem_take((struct k_sem *)*sema, timeout_ms == 0 ? K_FOREVER : K_MSEC(timeout_ms));
	return (ret != 0) ? OS_TIMEOUT : OS_OK;
}

/**
 * @brief Get the count of a semaphore.
 *
 * @param sema Semaphore.
 * @return Current count.
 */
int sys_sema_get_count(os_sema_t *sema)
{
	return (int)k_sem_count_get((struct k_sem *)*sema);
}

/* Mutexes (k_mutex is recursive) */

/**
 * @brief Allocate and initialize a recursive mutex.
 *
 * @param mutex Receives the mutex.
 * @return OS_OK, or OS_ERROR when the allocation fails.
 */
int sys_mutex_init(os_mutex_t *mutex)
{
	struct k_mutex *m = k_malloc(sizeof(struct k_mutex));

	if (m == NULL) {
		return OS_ERROR;
	}

	k_mutex_init(m);
	*mutex = m;
	return OS_OK;
}

/**
 * @brief Free a mutex.
 *
 * @param mutex Mutex; NULL or already freed is ignored.
 */
void sys_mutex_free(os_mutex_t *mutex)
{
	if (mutex != NULL && *mutex != NULL) {
		k_free(*mutex);
		*mutex = NULL;
	}
}

/**
 * @brief Lock a mutex, waiting forever.
 *
 * @param mutex Mutex.
 * @return OS_OK.
 */
int32_t sys_mutex_get(os_mutex_t *mutex)
{
	k_mutex_lock((struct k_mutex *)*mutex, K_FOREVER);
	return OS_OK;
}

/**
 * @brief Lock a mutex with a timeout.
 *
 * @param mutex Mutex.
 * @param timeout Timeout in milliseconds, -1 to wait forever, 0 not to wait.
 * @return OS_OK, or OS_ERROR when the mutex could not be locked in time.
 */
int32_t sys_mutex_try_get(os_mutex_t *mutex, int timeout)
{
	int ret = k_mutex_lock((struct k_mutex *)*mutex, timeout_int_ms(timeout));

	return (ret != 0) ? OS_ERROR : OS_OK;
}

/**
 * @brief Unlock a mutex.
 *
 * @param mutex Mutex.
 */
void sys_mutex_put(os_mutex_t *mutex)
{
	k_mutex_unlock((struct k_mutex *)*mutex);
}

/* Queues */

/**
 * @brief Allocate and initialize a message queue.
 *
 * @param queue Receives the queue.
 * @param queue_size Depth in messages.
 * @param item_size Message size in bytes.
 * @return OS_OK, or OS_ERROR when an allocation fails.
 */
int32_t sys_queue_init(os_queue_t *queue, int32_t queue_size, uint32_t item_size)
{
	struct k_msgq *q = k_malloc(sizeof(struct k_msgq));

	if (q == NULL) {
		return OS_ERROR;
	}

	if (k_msgq_alloc_init(q, item_size, queue_size) != 0) {
		k_free(q);
		return OS_ERROR;
	}

	*queue = q;
	return OS_OK;
}

/**
 * @brief Free a message queue.
 *
 * @param queue Queue; NULL or already freed is ignored.
 */
void sys_queue_free(os_queue_t *queue)
{
	if (queue != NULL && *queue != NULL) {
		k_msgq_cleanup((struct k_msgq *)*queue);
		k_free(*queue);
		*queue = NULL;
	}
}

/**
 * @brief Post a message without waiting.
 *
 * @param queue Queue.
 * @param msg Message to copy.
 * @return OS_OK, or OS_ERROR when the queue is full.
 */
int32_t sys_queue_post(os_queue_t *queue, void *msg)
{
	return (k_msgq_put((struct k_msgq *)*queue, msg, K_NO_WAIT) != 0) ? OS_ERROR : OS_OK;
}

/**
 * @brief Post a message, waiting for room when called from a thread.
 *
 * @param queue Queue.
 * @param msg Message to copy.
 * @param timeout_ms Timeout in milliseconds, -1 to wait forever, 0 not to
 *                   wait; never waits from interrupt context.
 * @return OS_OK, or OS_ERROR when the queue stays full.
 */
int32_t sys_queue_post_with_timeout(os_queue_t *queue, void *msg, int32_t timeout_ms)
{
	k_timeout_t to = k_is_in_isr() ? K_NO_WAIT : timeout_int_ms(timeout_ms);

	return (k_msgq_put((struct k_msgq *)*queue, msg, to) != 0) ? OS_ERROR : OS_OK;
}

/**
 * @brief Fetch a message.
 *
 * @param queue Queue.
 * @param msg Receives the message.
 * @param timeout_ms Timeout in milliseconds when blocking; 0 waits forever.
 * @param is_blocking 0 to return at once when the queue is empty; never
 *                    waits from interrupt context.
 * @return OS_OK, or OS_TIMEOUT when no message arrived in time.
 */
int32_t sys_queue_fetch(os_queue_t *queue, void *msg, uint32_t timeout_ms, uint8_t is_blocking)
{
	k_timeout_t to;

	if (!is_blocking) {
		to = K_NO_WAIT;
	} else if (timeout_ms == 0) {
		to = K_FOREVER;
	} else {
		to = K_MSEC(timeout_ms);
	}

	if (k_is_in_isr()) {
		to = K_NO_WAIT;
	}

	return (k_msgq_get((struct k_msgq *)*queue, msg, to) != 0) ? OS_TIMEOUT : OS_OK;
}

/**
 * @brief Tell whether a queue is empty.
 *
 * @param queue Queue.
 * @return true when no message is queued.
 */
bool sys_queue_is_empty(os_queue_t *queue)
{
	return k_msgq_num_used_get((struct k_msgq *)*queue) == 0;
}

/**
 * @brief Count the messages in a queue.
 *
 * @param queue Queue.
 * @return Number of queued messages.
 */
int sys_queue_cnt(os_queue_t *queue)
{
	return (int)k_msgq_num_used_get((struct k_msgq *)*queue);
}

/* Note inverted polarity: 0 = success, non-zero = failure */

/**
 * @brief Post a message; note the inverted result polarity of this call.
 *
 * @param queue Queue.
 * @param msg Message to copy.
 * @param timeout Timeout in milliseconds, -1 to wait forever, 0 not to wait.
 * @param isr true from interrupt context, where the call never waits.
 * @return 0 on success, 1 on failure.
 */
int sys_queue_write(os_queue_t *queue, void *msg, int timeout, bool isr)
{
	k_timeout_t to = isr ? K_NO_WAIT : timeout_int_ms(timeout);

	return (k_msgq_put((struct k_msgq *)*queue, msg, to) != 0) ? 1 : 0;
}

/**
 * @brief Fetch a message; note the inverted result polarity of this call.
 *
 * @param queue Queue.
 * @param msg Receives the message.
 * @param timeout Timeout in milliseconds, -1 to wait forever, 0 not to wait.
 * @param isr true from interrupt context, where the call never waits.
 * @return 0 on success, 1 on failure.
 */
int sys_queue_read(os_queue_t *queue, void *msg, int timeout, bool isr)
{
	k_timeout_t to = isr ? K_NO_WAIT : timeout_int_ms(timeout);

	return (k_msgq_get((struct k_msgq *)*queue, msg, to) != 0) ? 1 : 0;
}

/* Time */

/**
 * @brief Get the uptime.
 *
 * @return Milliseconds since boot, 32-bit.
 */
uint32_t sys_current_time_get(void)
{
	return k_uptime_get_32();
}

/**
 * @brief Get the uptime; same as sys_current_time_get().
 *
 * @param p Ignored.
 * @return Milliseconds since boot, 32-bit.
 */
uint32_t sys_time_get(void *p)
{
	return sys_current_time_get();
}

/**
 * @brief Get the uptime in OS ticks; one tick is one millisecond here.
 *
 * @param isr Ignored.
 * @return Milliseconds since boot, 32-bit.
 */
uint32_t sys_os_now(bool isr)
{
	/* OS_MS_PER_TICK is 1 in wrapper_os_config.h: "ticks" == ms */

	return sys_current_time_get();
}

/**
 * @brief Put the calling task to sleep.
 *
 * @param ms Milliseconds; 0 or less returns at once.
 */
void sys_ms_sleep(int ms)
{
	if (ms <= 0) {
		return;
	}

	k_msleep(ms);
}

/**
 * @brief Busy-wait.
 *
 * @param nus Microseconds.
 */
void sys_us_delay(uint32_t nus)
{
	k_busy_wait(nus);
}

/**
 * @brief Yield the processor to threads of the same priority.
 */
void sys_yield(void)
{
	k_yield();
}

/**
 * @brief Prevent preemption of the calling task until sys_sched_unlock().
 */
void sys_sched_lock(void)
{
	k_sched_lock();
}

/**
 * @brief Allow preemption again.
 */
void sys_sched_unlock(void)
{
	k_sched_unlock();
}

/* Timers: delayable work so the callback runs in thread context */

static void timer_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct z_timer_s *t = CONTAINER_OF(dwork, struct z_timer_s, dwork);

	if (!t->active) {
		return;
	}

	if (t->periodic) {
		k_work_schedule(&t->dwork, K_MSEC(t->delay_ms));
	} else {
		t->active = 0;
	}

	t->func(t, t->arg);
}

/**
 * @brief Allocate a software timer. The callback runs from the system
 * workqueue, in thread context, as with the vendor timer daemon.
 *
 * @param timer Receives the timer; NULL when the allocation fails.
 * @param name Ignored.
 * @param delay Period or delay in milliseconds.
 * @param periodic Non-zero for a periodic timer.
 * @param func Callback, called with the timer and arg.
 * @param arg Callback argument.
 */
void sys_timer_init(os_timer_t *timer, const uint8_t *name, uint32_t delay, uint8_t periodic,
		    timer_func_t func, void *arg)
{
	struct z_timer_s *t = k_calloc(1, sizeof(*t));

	if (t == NULL) {
		*timer = NULL;
		return;
	}

	t->delay_ms = delay;
	t->periodic = periodic;
	t->func = func;
	t->arg = arg;
	k_work_init_delayable(&t->dwork, timer_work_handler);
	*timer = t;
}

/**
 * @brief Stop and free a timer.
 *
 * @param timer Timer; NULL or already freed is ignored.
 */
void sys_timer_delete(os_timer_t *timer)
{
	struct z_timer_s *t;

	if (timer == NULL || *timer == NULL) {
		return;
	}

	t = (struct z_timer_s *)*timer;
	*timer = NULL;

	t->active = 0;
	k_work_cancel_delayable(&t->dwork);
	k_free(t);
}

/**
 * @brief Start or restart a timer with its configured delay.
 *
 * @param timer Timer.
 * @param from_isr Ignored.
 */
void sys_timer_start(os_timer_t *timer, uint8_t from_isr)
{
	struct z_timer_s *t = (struct z_timer_s *)*timer;

	t->active = 1;
	k_work_reschedule(&t->dwork, K_MSEC(t->delay_ms));
}

/**
 * @brief Start or restart a timer with a new delay.
 *
 * @param timer Timer.
 * @param delay Delay in milliseconds, kept as the new period.
 * @param from_isr Ignored.
 */
void sys_timer_start_ext(os_timer_t *timer, uint32_t delay, uint8_t from_isr)
{
	struct z_timer_s *t = (struct z_timer_s *)*timer;

	t->delay_ms = delay;
	t->active = 1;
	k_work_reschedule(&t->dwork, K_MSEC(delay));
}

/**
 * @brief Stop a timer.
 *
 * @param timer Timer.
 * @param from_isr Ignored.
 * @return 1 (success).
 */
uint8_t sys_timer_stop(os_timer_t *timer, uint8_t from_isr)
{
	struct z_timer_s *t = (struct z_timer_s *)*timer;

	t->active = 0;
	k_work_cancel_delayable(&t->dwork);
	return 1;
}

/**
 * @brief Tell whether a timer is armed.
 *
 * @param timer Timer.
 * @return 1 when armed, 0 otherwise.
 */
uint8_t sys_timer_pending(os_timer_t *timer)
{
	struct z_timer_s *t = (struct z_timer_s *)*timer;

	return (t->active && k_work_delayable_is_pending(&t->dwork)) ? 1 : 0;
}

/* Misc */

/**
 * @brief Fill a buffer with random bytes from the hardware TRNG, through the
 * SDK driver.
 *
 * @param dst Destination.
 * @param size Number of bytes.
 * @return 0 on success, otherwise the SDK driver error.
 */
int32_t sys_random_bytes_get(void *dst, uint32_t size)
{
	/* Hardware TRNG via the SDK driver (self-initializing) */

	extern int random_get(unsigned char *dst, unsigned int size);
	return random_get(dst, size);
}

/* The Wi-Fi/BLE firmware polls MAC status inside sys_enter_critical()
 * sections while its own interrupts (programmed at ECLIC level 8 by the
 * SDK) must keep running -- the FreeRTOS port implements this with
 * configMAX_SYSCALL_INTERRUPT_PRIORITY.  Model it with the ECLIC MTH
 * threshold: kernel-level sources are masked, the radio sources still
 * fire.  mstatus.MIE is only touched inside the tiny spinlock window, so
 * Zephyr's own irq_lock() remains the stronger global gate.
 */

/**
 * @brief Tell whether the caller is in interrupt context or inside a
 * sys_enter_critical() section.
 *
 * @return 1 when so, 0 otherwise.
 */
uint32_t sys_in_critical(void)
{
	return (k_is_in_isr() || g_crit_nest > 0) ? 1 : 0;
}

/**
 * @brief Enter a nestable critical section. Kernel-level interrupts are masked
 * by raising the ECLIC threshold; the radio interrupts (level 8) keep
 * running, as with the vendor FreeRTOS port.
 */
void sys_enter_critical(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_crit_lock);

	if (g_crit_nest == 0) {
		*(volatile uint8_t *)GDWIFI_ECLIC_MTH = GDWIFI_MTH_KERNEL;
	}

	g_crit_nest++;
	k_spin_unlock(&g_crit_lock, key);
}

/**
 * @brief Leave a critical section entered with sys_enter_critical().
 */
void sys_exit_critical(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_crit_lock);

	__ASSERT(g_crit_nest > 0, "unbalanced %s", __func__);

	if (--g_crit_nest == 0) {
		*(volatile uint8_t *)GDWIFI_ECLIC_MTH = 0;
	}

	k_spin_unlock(&g_crit_lock, key);
}

/**
 * @brief Interrupt entry hook; no-op, Zephyr tracks interrupt context itself.
 */
void sys_int_enter(void)
{
}

/**
 * @brief Interrupt exit hook; no-op.
 */
void sys_int_exit(void)
{
}

/**
 * @brief Record the power-save mode requested by the SDK.
 *
 * @param mode SYS_PS_OFF or another SYS_PS_* value.
 */
void sys_ps_set(uint8_t mode)
{
	sys_ps_mode = mode;
}

/**
 * @brief Get the power-save mode recorded by sys_ps_set().
 *
 * @return The mode.
 */
uint8_t sys_ps_get(void)
{
	return sys_ps_mode;
}

/**
 * @brief Report CPU sleep statistics; no sleep is accounted.
 *
 * @param stats_ms Receives the uptime in milliseconds, or NULL.
 * @param sleep_ms Receives 0, or NULL.
 */
void sys_cpu_sleep_time_get(uint32_t *stats_ms, uint32_t *sleep_ms)
{
	if (stats_ms) {
		*stats_ms = sys_current_time_get();
	}

	if (sleep_ms) {
		*sleep_ms = 0;
	}
}

/**
 * @brief Print CPU statistics; no-op.
 */
void sys_cpu_stats(void)
{
}

/**
 * @brief Initialize the OS wrapper: reset the task table.
 */
void sys_os_init(void)
{
	memset(g_tasks, 0, sizeof(g_tasks));
}

/**
 * @brief Late OS wrapper initialization; no-op.
 */
void sys_os_misc_init(void)
{
}

/**
 * @brief Start the scheduler; no-op, the Zephyr scheduler is already running.
 */
void sys_os_start(void)
{
	/* The Zephyr scheduler is already running */
}
