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

#define PRIO_SDK2Z(p)  (((int)(p) >= 31) ? 0 : (31 - (int)(p)))

#define MAX_TASKS      16   /* Concurrent sys_task_create() tasks */

#if defined(CONFIG_WIFI_GDWIFI_TRACE)
#define WTRACE(fmt, ...)						\
	do {								\
		static int n_;						\
		if (n_ < 60) {						\
			n_++;						\
			printk("gdwifi: " fmt "\n", ##__VA_ARGS__);	\
		}							\
	} while (0)
#else
#define WTRACE(fmt, ...)
#endif

/* Per-task bookkeeping: thread + mailbox + notification semaphore */

struct z_task_s {
	k_tid_t tid;
	struct k_thread thread;
	k_thread_stack_t *stack;
	struct k_msgq *mbox;      /* NULL if queue_size == 0 */
	struct k_sem notif;       /* Counting notification */
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

#define GDWIFI_ECLIC_MTH   (0xd2000000UL + 0x000bUL)
#define GDWIFI_MTH_KERNEL  0x10U   /* masks level <= kernel, radio (8<<4) passes */

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

void *sys_malloc(size_t size)
{
	return k_malloc(size);
}

void *sys_calloc(size_t count, size_t size)
{
	return k_calloc(count, size);
}

void *sys_realloc(void *mem, size_t size)
{
	return k_realloc(mem, size);
}

void sys_mfree(void *ptr)
{
	k_free(ptr);
}

int32_t sys_free_heap_size(void)
{
	/* The SDK only uses this for diagnostics and low-memory warnings */
	return 64 * 1024;
}

int32_t sys_min_free_heap_size(void)
{
	return sys_free_heap_size();
}

uint16_t sys_heap_block_size(void)
{
	return 16;
}

void sys_heap_info(int *total_size, int *free_size, int *min_free_size)
{
	if (total_size) {
		*total_size = CONFIG_HEAP_MEM_POOL_SIZE;
	}

	if (free_size) {
		*free_size = sys_free_heap_size();
	}

	if (min_free_size) {
		*min_free_size = sys_free_heap_size();
	}
}

void sys_add_heap_region(uint32_t start, uint32_t size)
{
	/* The BLE bring-up donates its RAM region; the Zephyr system heap
	 * cannot grow at runtime, so this is intentionally ignored (the
	 * driver Kconfig sizes the pool for it instead).
	 */
}

void sys_remove_heap_region(uint32_t start, uint32_t size)
{
}

/* Memory manipulation */

void sys_memset(void *s, uint8_t c, uint32_t count)
{
	memset(s, c, count);
}

void sys_memcpy(void *des, const void *src, uint32_t n)
{
	memcpy(des, src, n);
}

void sys_memmove(void *des, const void *src, uint32_t n)
{
	memmove(des, src, n);
}

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

void *sys_task_create(void *static_tcb, const uint8_t *name,
		      uint32_t *stack_base, uint32_t stack_size,
		      uint32_t queue_size, uint32_t queue_item_size,
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
		    k_msgq_alloc_init(t->mbox, queue_item_size,
				      queue_size) != 0) {
			WTRACE("mbox_create('%s' %ux%u) failed",
			       (const char *)name, (unsigned int)queue_size,
			       (unsigned int)queue_item_size);
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
		WTRACE("stack_alloc('%s' %u) failed", t->name,
		       (unsigned int)stack_bytes);
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

	t->tid = k_thread_create(&t->thread, t->stack, stack_bytes,
				 task_trampoline, t, NULL, NULL,
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

	WTRACE("task '%s' prio=%d stack=%uB q=%ux%u", t->name,
	       PRIO_SDK2Z(priority), (unsigned int)stack_bytes,
	       (unsigned int)queue_size, (unsigned int)queue_item_size);

	k_thread_start(t->tid);

	return t->tid;
}

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

char *sys_task_name_get(void *task)
{
	struct z_task_s *t = task_slot_from_handle(task);

	return t != NULL ? t->name : NULL;
}

void sys_task_list(char *pwrite_buf)
{
	if (pwrite_buf) {
		*pwrite_buf = '\0';
	}
}

int32_t sys_task_wait(uint32_t timeout_ms, void *msg_ptr)
{
	struct z_task_s *t = task_slot_find(k_current_get());
	int ret;

	if (t == NULL || t->mbox == NULL) {
		WTRACE("wait: current task has NO MBOX");
		return OS_ERROR;
	}

	/* timeout 0 = wait forever (FreeRTOS wrapper convention here) */

	ret = k_msgq_get(t->mbox, msg_ptr,
			 timeout_ms == 0 ? K_FOREVER : K_MSEC(timeout_ms));
	return (ret != 0) ? OS_TIMEOUT : OS_OK;
}

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

void sys_task_msg_flush(void *task)
{
	struct z_task_s *t = task_slot_from_handle(task);

	if (t != NULL && t->mbox != NULL) {
		k_msgq_purge(t->mbox);
	}
}

int32_t sys_task_msg_num(void *task, uint8_t from_isr)
{
	struct z_task_s *t = task_slot_from_handle(task);

	if (t == NULL || t->mbox == NULL) {
		return OS_ERROR;
	}

	return k_msgq_num_used_get(t->mbox);
}

os_task_t sys_current_task_handle_get(void)
{
	return (os_task_t)k_current_get();
}

int32_t sys_current_task_stack_depth(unsigned long cur_sp)
{
	return 1024;
}

uint32_t sys_stack_free_get(void *task)
{
	return 1024;
}

int sys_task_init_notification(void *task)
{
	return 0;
}

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

void sys_task_notify(void *task, bool isr)
{
	struct z_task_s *t = task_slot_from_handle(task);

	if (t != NULL) {
		k_sem_give(&t->notif);
	}
}

uint8_t sys_task_exist(const uint8_t *name)
{
	for (int i = 0; i < MAX_TASKS; i++) {
		if (g_tasks[i] != NULL &&
		    strncmp(g_tasks[i]->name, (const char *)name,
			    sizeof(g_tasks[i]->name)) == 0) {
			return 1;
		}
	}

	return 0;
}

void sys_priority_set(void *task, os_prio_t priority)
{
	k_tid_t tid = (task == NULL) ? k_current_get() : (k_tid_t)task;

	k_thread_priority_set(tid, PRIO_SDK2Z(priority));
}

os_prio_t sys_priority_get(void *task)
{
	k_tid_t tid = (task == NULL) ? k_current_get() : (k_tid_t)task;
	int prio = k_thread_priority_get(tid);

	return (os_prio_t)(31 - prio);
}

/* Semaphores */

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

int32_t sys_sema_init(os_sema_t *sema, int32_t init_val)
{
	return sys_sema_init_ext(sema, K_SEM_MAX_LIMIT, init_val);
}

void sys_sema_free(os_sema_t *sema)
{
	if (sema != NULL && *sema != NULL) {
		k_free(*sema);
		*sema = NULL;
	}
}

void sys_sema_up(os_sema_t *sema)
{
	k_sem_give((struct k_sem *)*sema);
}

void sys_sema_up_from_isr(os_sema_t *sema)
{
	k_sem_give((struct k_sem *)*sema);
}

int32_t sys_sema_down(os_sema_t *sema, uint32_t timeout_ms)
{
	int ret;

	/* timeout 0 = wait forever here */

	ret = k_sem_take((struct k_sem *)*sema,
			 timeout_ms == 0 ? K_FOREVER : K_MSEC(timeout_ms));
	return (ret != 0) ? OS_TIMEOUT : OS_OK;
}

int sys_sema_get_count(os_sema_t *sema)
{
	return (int)k_sem_count_get((struct k_sem *)*sema);
}

/* Mutexes (k_mutex is recursive) */

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

void sys_mutex_free(os_mutex_t *mutex)
{
	if (mutex != NULL && *mutex != NULL) {
		k_free(*mutex);
		*mutex = NULL;
	}
}

int32_t sys_mutex_get(os_mutex_t *mutex)
{
	k_mutex_lock((struct k_mutex *)*mutex, K_FOREVER);
	return OS_OK;
}

int32_t sys_mutex_try_get(os_mutex_t *mutex, int timeout)
{
	int ret = k_mutex_lock((struct k_mutex *)*mutex,
			       timeout_int_ms(timeout));

	return (ret != 0) ? OS_ERROR : OS_OK;
}

void sys_mutex_put(os_mutex_t *mutex)
{
	k_mutex_unlock((struct k_mutex *)*mutex);
}

/* Queues */

int32_t sys_queue_init(os_queue_t *queue, int32_t queue_size,
		       uint32_t item_size)
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

void sys_queue_free(os_queue_t *queue)
{
	if (queue != NULL && *queue != NULL) {
		k_msgq_cleanup((struct k_msgq *)*queue);
		k_free(*queue);
		*queue = NULL;
	}
}

int32_t sys_queue_post(os_queue_t *queue, void *msg)
{
	return (k_msgq_put((struct k_msgq *)*queue, msg, K_NO_WAIT) != 0) ?
	       OS_ERROR : OS_OK;
}

int32_t sys_queue_post_with_timeout(os_queue_t *queue, void *msg,
				    int32_t timeout_ms)
{
	k_timeout_t to = k_is_in_isr() ? K_NO_WAIT : timeout_int_ms(timeout_ms);

	return (k_msgq_put((struct k_msgq *)*queue, msg, to) != 0) ?
	       OS_ERROR : OS_OK;
}

int32_t sys_queue_fetch(os_queue_t *queue, void *msg, uint32_t timeout_ms,
			uint8_t is_blocking)
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

	return (k_msgq_get((struct k_msgq *)*queue, msg, to) != 0) ?
	       OS_TIMEOUT : OS_OK;
}

bool sys_queue_is_empty(os_queue_t *queue)
{
	return k_msgq_num_used_get((struct k_msgq *)*queue) == 0;
}

int sys_queue_cnt(os_queue_t *queue)
{
	return (int)k_msgq_num_used_get((struct k_msgq *)*queue);
}

/* Note inverted polarity: 0 = success, non-zero = failure */

int sys_queue_write(os_queue_t *queue, void *msg, int timeout, bool isr)
{
	k_timeout_t to = isr ? K_NO_WAIT : timeout_int_ms(timeout);

	return (k_msgq_put((struct k_msgq *)*queue, msg, to) != 0) ? 1 : 0;
}

int sys_queue_read(os_queue_t *queue, void *msg, int timeout, bool isr)
{
	k_timeout_t to = isr ? K_NO_WAIT : timeout_int_ms(timeout);

	return (k_msgq_get((struct k_msgq *)*queue, msg, to) != 0) ? 1 : 0;
}

/* Time */

uint32_t sys_current_time_get(void)
{
	return k_uptime_get_32();
}

uint32_t sys_time_get(void *p)
{
	return sys_current_time_get();
}

uint32_t sys_os_now(bool isr)
{
	/* OS_MS_PER_TICK is 1 in wrapper_os_config.h: "ticks" == ms */

	return sys_current_time_get();
}

void sys_ms_sleep(int ms)
{
	if (ms <= 0) {
		return;
	}

	k_msleep(ms);
}

void sys_us_delay(uint32_t nus)
{
	k_busy_wait(nus);
}

void sys_yield(void)
{
	k_yield();
}

void sys_sched_lock(void)
{
	k_sched_lock();
}

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

void sys_timer_init(os_timer_t *timer, const uint8_t *name, uint32_t delay,
		    uint8_t periodic, timer_func_t func, void *arg)
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

void sys_timer_start(os_timer_t *timer, uint8_t from_isr)
{
	struct z_timer_s *t = (struct z_timer_s *)*timer;

	t->active = 1;
	k_work_reschedule(&t->dwork, K_MSEC(t->delay_ms));
}

void sys_timer_start_ext(os_timer_t *timer, uint32_t delay, uint8_t from_isr)
{
	struct z_timer_s *t = (struct z_timer_s *)*timer;

	t->delay_ms = delay;
	t->active = 1;
	k_work_reschedule(&t->dwork, K_MSEC(delay));
}

uint8_t sys_timer_stop(os_timer_t *timer, uint8_t from_isr)
{
	struct z_timer_s *t = (struct z_timer_s *)*timer;

	t->active = 0;
	k_work_cancel_delayable(&t->dwork);
	return 1;
}

uint8_t sys_timer_pending(os_timer_t *timer)
{
	struct z_timer_s *t = (struct z_timer_s *)*timer;

	return (t->active && k_work_delayable_is_pending(&t->dwork)) ? 1 : 0;
}

/* Misc */

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

uint32_t sys_in_critical(void)
{
	return (k_is_in_isr() || g_crit_nest > 0) ? 1 : 0;
}

void sys_enter_critical(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_crit_lock);

	if (g_crit_nest == 0) {
		*(volatile uint8_t *)GDWIFI_ECLIC_MTH = GDWIFI_MTH_KERNEL;
	}

	g_crit_nest++;
	k_spin_unlock(&g_crit_lock, key);
}

void sys_exit_critical(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_crit_lock);

	__ASSERT(g_crit_nest > 0, "unbalanced sys_exit_critical");

	if (--g_crit_nest == 0) {
		*(volatile uint8_t *)GDWIFI_ECLIC_MTH = 0;
	}

	k_spin_unlock(&g_crit_lock, key);
}

void sys_int_enter(void)
{
}

void sys_int_exit(void)
{
}

void sys_ps_set(uint8_t mode)
{
	sys_ps_mode = mode;
}

uint8_t sys_ps_get(void)
{
	return sys_ps_mode;
}

void sys_cpu_sleep_time_get(uint32_t *stats_ms, uint32_t *sleep_ms)
{
	if (stats_ms) {
		*stats_ms = sys_current_time_get();
	}

	if (sleep_ms) {
		*sleep_ms = 0;
	}
}

void sys_cpu_stats(void)
{
}

void sys_os_init(void)
{
	memset(g_tasks, 0, sizeof(g_tasks));
}

void sys_os_misc_init(void)
{
}

void sys_os_start(void)
{
	/* The Zephyr scheduler is already running */
}
