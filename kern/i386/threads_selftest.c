/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/threads_selftest.c
 *
 * `test=threads`: exercises the thread, scheduler, ring 3 and system-call
 * machinery of the i386 port. Runs after i386_init_threads().
 *
 *   1  the shared scheduler self-tests (run queue, MLFQ)
 *   2  machine_thread_* API contracts
 *   3  kernel threads ping-ponging through sched_yield: FIFO ordering, TSS
 *      esp0 following every switch, termination and reaping
 *   4  MLFQ demotion by charged ticks and boost, and preempt-on-return
 *      (i386_trap_exit) driven with synthetic frames
 *   5  the system-call request builder and dispatch, from a kernel frame
 *   6  ring 3: a process that makes system calls and exits, a second one that
 *      receives its initial argument, processes that fault (#GP, #UD, #DE,
 *      a bad segment load) and are terminated while the kernel keeps
 *      running, and a real user thread preempted through the real trap path
 *
 * Paging is off in this bring-up, so everything is identity-mapped and ring 3
 * can run code linked into the kernel image with flat segments. The "user
 * programs" below are ordinary functions that only talk to the kernel through
 * `int $0x80`. What this cannot test: separate user address spaces and page
 * faults (vm area), and the timer tick (interrupts area); the tick is
 * emulated where needed, see i386_trap_set_user_exception_hook().
 */

#include <kern/i386/boot_info.h>

#include <kern/i386/gdt.h>
#include <kern/i386/idt.h>
#include <kern/i386/io.h>
#include <kern/i386/syscall_trap.h>
#include <kern/i386/thread.h>
#include <kern/i386/trap.h>

#include <kern/console/console.h>
#include <kern/process/proc.h>
#include <kern/process/task.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <kern/syscall/syscall.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PIC_MASTER_DATA 0x21U
#define PIC_SLAVE_DATA 0xA1U

/* An exception vector the kernel does not use, borrowed as a software tick. */
#define SELFTEST_TICK_VECTOR 22U

#define SELFTEST_PINGPONG_THREADS 3U
#define SELFTEST_PINGPONG_ROUNDS 4U

#define SELFTEST_USER_STACK_SIZE 4096U
#define SELFTEST_USER_STACKS 8U

#define SELFTEST_LOG_SIZE 64U

#define SELFTEST_CHECK(condition, what) \
	do { \
		if (!(condition)) { \
			kprintf("i386_init_threads_selftest: FAIL %s (line %u)\n", (what), (uint32_t)__LINE__); \
			return false; \
		} \
	} while (0)

/* ---- shared state -------------------------------------------------------- */

static volatile char g_log[SELFTEST_LOG_SIZE];
static volatile uint32_t g_log_length;
static volatile uint32_t g_thread_failures;

static uint8_t g_user_stacks[SELFTEST_USER_STACKS][SELFTEST_USER_STACK_SIZE] __attribute__((aligned(16)));
static uint32_t g_user_stack_next;

static void log_reset(void)
{
	g_log_length = 0U;
	g_log[0] = '\0';
}

static void log_put(char value)
{
	uint32_t index = g_log_length;

	if (index + 1U >= SELFTEST_LOG_SIZE) return;

	g_log[index] = value;
	g_log[index + 1U] = '\0';
	g_log_length = index + 1U;
}

static bool log_equals(const char *expected)
{
	uint32_t index = 0U;

	while (expected[index] != '\0') {
		if (index >= g_log_length || g_log[index] != expected[index]) return false;
		index++;
	}

	return index == g_log_length;
}

static void thread_check(bool condition, const char *what)
{
	if (condition) return;

	kprintf("i386_init_threads_selftest: FAIL (in thread) %s\n", what);
	g_thread_failures++;
}

/*
 * A stray legacy interrupt would land on a BIOS-default vector (IRQ0 on
 * vector 8 is #DF) unless the interrupt area has remapped the PIC, and the
 * threads below run with interrupts enabled. None of these tests needs a
 * hardware interrupt, so mask the lines for their duration and put the
 * masks back afterwards.
 */
static uint8_t g_saved_pic_master;
static uint8_t g_saved_pic_slave;

static void selftest_mask_pic(void)
{
	g_saved_pic_master = inb(PIC_MASTER_DATA);
	g_saved_pic_slave = inb(PIC_SLAVE_DATA);
	outb(PIC_MASTER_DATA, 0xFFU);
	outb(PIC_SLAVE_DATA, 0xFFU);
}

static void selftest_restore_pic(void)
{
	outb(PIC_MASTER_DATA, g_saved_pic_master);
	outb(PIC_SLAVE_DATA, g_saved_pic_slave);
}

/* Charge ticks to the running thread until it sits at exactly `level`. */
static bool selftest_tick_to_level(uint8_t level)
{
	thread_t self = current_thread();

	for (uint32_t ticks = 0U; ticks < 2U * SCHED_MLFQ_BOOST_INTERVAL_TICKS; ticks++) {
		if (self->mlfq_level == level) return true;
		sched_tick();
	}

	return false;
}

/* The equalizer boost returns the running thread to level 0. */
static bool selftest_boost_self(void)
{
	thread_t self = current_thread();

	for (uint32_t ticks = 0U; ticks < 2U * SCHED_MLFQ_BOOST_INTERVAL_TICKS; ticks++) {
		sched_tick();
		if (self->mlfq_level == SCHED_MLFQ_TOP_LEVEL) {
			sched_clear_preemption();
			return true;
		}
	}

	return false;
}

/* Yield until `done` holds, with a bound so a bug fails instead of hanging. */
#define SELFTEST_YIELD_UNTIL(condition) \
	for (uint32_t spins_ = 0U; !(condition) && spins_ < 100000U; spins_++) (void)sched_yield()

/*
 * With paging on (the integrated kernel) user code must live in mapped user
 * pages, which needs the vm area's address spaces and the loader; the ring 3
 * stages below run kernel-image code with flat segments and so only work
 * with paging off, as in a threads-only bring-up.
 */
static bool selftest_paging_enabled(void)
{
	uint32_t cr0;

	__asm__ volatile("movl %%cr0, %0" : "=r"(cr0));
	return (cr0 & 0x80000000U) != 0U;
}

/* ---- 1: shared scheduler self-tests ------------------------------------- */

static bool test_shared_scheduler(void)
{
	SELFTEST_CHECK(sched_run_queue_self_test(), "sched_run_queue_self_test");
	SELFTEST_CHECK(sched_mlfq_self_test(), "sched_mlfq_self_test");
	SELFTEST_CHECK(sched_validate() && thread_validate(), "sched_validate/thread_validate");

	kputln("i386_init_threads_selftest: run queue and MLFQ self-tests passed");
	return true;
}

/* ---- 2: machine_thread contracts ---------------------------------------- */

static uint8_t g_context_buffer[256] __attribute__((aligned(16)));

static bool test_machine_thread(void)
{
	machine_thread_t machine;

	machine_thread_init_kernel(&machine);
	SELFTEST_CHECK(!machine_thread_has_user_state(&machine), "kernel thread has no user state");

	SELFTEST_CHECK(machine_thread_init_user(&machine, 0x1000ULL, 0x2000ULL, 7ULL), "init_user");
	SELFTEST_CHECK(machine_thread_has_user_state(&machine), "init_user sets user state");
	SELFTEST_CHECK(machine_thread_user_pc(&machine) == 0x1000ULL, "user pc");
	SELFTEST_CHECK(machine_thread_user_sp(&machine) == 0x2000ULL, "user sp");
	SELFTEST_CHECK(machine.user.arg == 7U, "user argument");
	SELFTEST_CHECK(machine.user.eflags == I386_THREAD_EFLAGS_USER && (machine.user.eflags & 0x200U) != 0U, "user eflags has IF");

	SELFTEST_CHECK(machine_thread_set_user_state(&machine, 0x3000ULL, 0x4000ULL, 9ULL), "set_user_state");
	SELFTEST_CHECK(machine_thread_user_pc(&machine) == 0x3000ULL && machine.user.arg == 9U, "set_user_state stores");

	SELFTEST_CHECK(!machine_thread_init_user(&machine, 0ULL, 0x2000ULL, 0ULL), "zero entry rejected");
	SELFTEST_CHECK(!machine_thread_init_user(&machine, 0x1000ULL, 0ULL, 0ULL), "zero stack rejected");
	SELFTEST_CHECK(!machine_thread_init_user(&machine, 0x100000000ULL, 0x2000ULL, 0ULL), "64-bit entry rejected");
	SELFTEST_CHECK(!machine_thread_init_user(&machine, 0x1000ULL, 0x2000ULL, 0x100000000ULL), "64-bit argument rejected");

	uint64_t top = (uint64_t)(uintptr_t)&g_context_buffer[sizeof(g_context_buffer)];

	machine_thread_init_kernel(&machine);
	SELFTEST_CHECK(!machine_thread_prepare_context(&machine, top - 8ULL, 0x1000ULL), "unaligned stack rejected");
	SELFTEST_CHECK(machine_thread_prepare_context(&machine, top, 0x1234ULL), "prepare_context");
	SELFTEST_CHECK(machine.context.sp == (uint32_t)top - 24U, "initial kernel sp");
	SELFTEST_CHECK(machine.esp0 == (uint32_t)top, "esp0 recorded");

	const uint32_t *frame = (const uint32_t *)(uintptr_t)machine.context.sp;

	SELFTEST_CHECK(frame[4] == 0x1234U, "initial return address is the entry");
	SELFTEST_CHECK(frame[0] == 0U && frame[1] == 0U && frame[2] == 0U && frame[3] == 0U, "initial callee-saved registers zero");
	SELFTEST_CHECK(((machine.context.sp + 5U * 4U) & 0xFU) == 12U, "entry sees an ABI-aligned stack");

	kputln("i386_init_threads_selftest: machine_thread contracts passed");
	return true;
}

/* ---- 3: kernel threads, ordering, reaping -------------------------------- */

static volatile uint32_t g_pingpong_done;

static void pingpong_thread(void *parameter)
{
	uint32_t id = (uint32_t)(uintptr_t)parameter;
	thread_t self = current_thread();

	for (uint32_t round = 0U; round < SELFTEST_PINGPONG_ROUNDS; round++) {
		log_put((char)('A' + id));

		/* TSS esp0 must name this thread's kernel stack whenever it runs. */
		thread_check(
			i386_gdt_main_tss()->esp0 == (uint32_t)thread_kernel_stack_top(self),
			"TSS esp0 follows the context switch"
		);

		thread_check(sched_yield(), "sched_yield in kernel thread");
	}

	g_pingpong_done++;
}

static bool test_kernel_threads(void)
{
	thread_t threads[SELFTEST_PINGPONG_THREADS];
	uint32_t baseline = thread_count();

	log_reset();
	g_pingpong_done = 0U;

	for (uint32_t index = 0U; index < SELFTEST_PINGPONG_THREADS; index++) {
		SELFTEST_CHECK(
			kernel_thread_create(proc_task(proc_kernel()), pingpong_thread, (void *)(uintptr_t)index, &threads[index]),
			"kernel_thread_create"
		);
		SELFTEST_CHECK(sched_thread_start(threads[index]), "sched_thread_start");
	}

	SELFTEST_YIELD_UNTIL(g_pingpong_done == SELFTEST_PINGPONG_THREADS);
	SELFTEST_CHECK(g_pingpong_done == SELFTEST_PINGPONG_THREADS, "every ping-pong thread finished");

	/* One more yield lets the last exiting thread be reaped by its successor. */
	(void)sched_yield();

	SELFTEST_CHECK(g_thread_failures == 0U, "no failure inside a kernel thread");
	SELFTEST_CHECK(log_equals("ABCABCABCABC"), "round-robin order A B C x4");

	for (uint32_t index = 0U; index < SELFTEST_PINGPONG_THREADS; index++) {
		SELFTEST_CHECK(thread_is_terminated(threads[index]), "thread terminated on return");
		SELFTEST_CHECK((threads[index]->flags & TH_FLAG_REAPED) != 0U, "thread reaped");
		SELFTEST_CHECK(threads[index]->kernel_stack == 0, "kernel stack released");
		thread_deallocate(threads[index]);
	}

	SELFTEST_CHECK(thread_count() == baseline, "thread count back to baseline");
	SELFTEST_CHECK(sched_validate() && thread_validate(), "validate after ping-pong");

	kprintf("i386_init_threads_selftest: kernel threads passed (log %s)\n", (const char *)g_log);
	return true;
}

/* ---- 4: MLFQ ticks and preempt-on-return --------------------------------- */

static void log_once_thread(void *parameter)
{
	log_put((char)(uintptr_t)parameter);
}

static bool test_tick_and_preempt(void)
{
	thread_t self = current_thread();

	SELFTEST_CHECK(self->mlfq_level == SCHED_MLFQ_TOP_LEVEL, "test starts at the top MLFQ level");

	/* A full quantum of ticks demotes one level and requests preemption. */
	uint32_t quantum = sched_mlfq_level_quantum(SCHED_MLFQ_TOP_LEVEL);

	for (uint32_t tick = 0U; tick + 1U < quantum; tick++) sched_tick();

	SELFTEST_CHECK(self->mlfq_level == SCHED_MLFQ_TOP_LEVEL, "still level 0 before the quantum expires");
	SELFTEST_CHECK(!sched_preemption_pending(), "no preemption before the quantum expires");

	sched_tick();

	SELFTEST_CHECK(self->mlfq_level == 1U, "quantum expiry demotes to level 1");
	SELFTEST_CHECK(self->sched_pri == sched_mlfq_level_priority(1U), "priority follows the level");
	SELFTEST_CHECK(sched_preemption_pending(), "quantum expiry requests preemption");

	/* Nothing better to run: the exit hook must consume the request and go on. */
	x86_saved_state_t user_frame;
	x86_saved_state_t kernel_frame;

	memset(&user_frame, 0, sizeof(user_frame));
	memset(&kernel_frame, 0, sizeof(kernel_frame));
	user_frame.cs = GDT_USER_CODE_SEL;
	kernel_frame.cs = GDT_KERNEL_CODE_SEL;

	i386_trap_exit(&user_frame);
	SELFTEST_CHECK(current_thread() == self, "preempt with an empty run queue stays put");
	SELFTEST_CHECK(!sched_preemption_pending(), "pending request consumed");

	/* Now something better is runnable (level 0 beats our level 1). */
	thread_t worker;

	log_reset();
	SELFTEST_CHECK(kernel_thread_create(proc_task(proc_kernel()), log_once_thread, (void *)(uintptr_t)'W', &worker), "create worker");
	SELFTEST_CHECK(sched_thread_start(worker), "start worker");
	SELFTEST_CHECK(sched_preemption_pending(), "a higher-priority thread becoming runnable requests preemption");

	/* A kernel thread interrupted in the kernel is cooperative: no switch. */
	i386_trap_exit(&kernel_frame);
	SELFTEST_CHECK(log_equals(""), "kernel-mode frame is not preempted");
	SELFTEST_CHECK(sched_preemption_pending(), "request stays pending");

	/* An interrupted user context is preemptible: the worker runs. */
	i386_trap_exit(&user_frame);
	SELFTEST_CHECK(log_equals("W"), "user-mode frame is preempted for the worker");
	SELFTEST_CHECK(current_thread() == self, "back on the original thread");
	SELFTEST_CHECK(!sched_preemption_pending(), "pending request cleared by the switch");

	SELFTEST_YIELD_UNTIL(thread_is_terminated(worker));
	(void)sched_yield();
	SELFTEST_CHECK((worker->flags & TH_FLAG_REAPED) != 0U, "worker reaped");
	thread_deallocate(worker);

	SELFTEST_CHECK(selftest_boost_self(), "boost returns the running thread to level 0");
	SELFTEST_CHECK(self->mlfq_level == SCHED_MLFQ_TOP_LEVEL, "level 0 after the boost");

	kputln("i386_init_threads_selftest: MLFQ ticks and preempt-on-return passed");
	return true;
}

/* ---- 5: the system-call request path from a kernel frame ----------------- */

static bool test_syscall_frame(void)
{
	x86_saved_state_t frame;
	syscall_request_t request;

	memset(&frame, 0, sizeof(frame));
	frame.eax = 0xFFFFFFF0U;
	frame.ebx = 0x11111111U;
	frame.ecx = 0x22222222U;
	frame.edx = 0x33333333U;
	frame.esi = 0x44444444U;
	frame.edi = 0x55555555U;
	frame.ebp = 0x86666666U;

	i386_syscall_request_from_state(&frame, &request);

	SELFTEST_CHECK(request.number == 0xFFFFFFF0ULL, "number from eax, zero-extended");
	SELFTEST_CHECK(request.arguments[0] == 0x11111111ULL, "argument 0 from ebx");
	SELFTEST_CHECK(request.arguments[1] == 0x22222222ULL, "argument 1 from ecx");
	SELFTEST_CHECK(request.arguments[2] == 0x33333333ULL, "argument 2 from edx");
	SELFTEST_CHECK(request.arguments[3] == 0x44444444ULL, "argument 3 from esi");
	SELFTEST_CHECK(request.arguments[4] == 0x55555555ULL, "argument 4 from edi");
	SELFTEST_CHECK(request.arguments[5] == 0x86666666ULL, "argument 5 from ebp, zero-extended");

	/* Through the real gate, from ring 0. */
	static const char message[] = "i386_init_threads_selftest: write syscall from a kernel frame\n";
	uint32_t eax = SYSCALL_WRITE;
	uint32_t ecx = (uint32_t)(uintptr_t)message;
	uint32_t edx = (uint32_t)sizeof(message) - 1U;
	uint32_t ebx = 1U;

	/* A kernel pointer is only a valid "user" pointer while paging is off. */
	if (!selftest_paging_enabled()) {
		__asm__ volatile("int $0x80" : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx) : : "memory");
		SELFTEST_CHECK(eax == (uint32_t)sizeof(message) - 1U, "write returns the byte count");
	}

	eax = SYSCALL_GETPID;
	__asm__ volatile("int $0x80" : "+a"(eax) : : "memory");
	SELFTEST_CHECK(eax == PROC_PID_KERNEL, "getpid from a kernel thread is the kernel pid");

	eax = 0x7777U;
	__asm__ volatile("int $0x80" : "+a"(eax) : : "memory");
	SELFTEST_CHECK(eax == (uint32_t)(0 - (int32_t)SYSCALL_ERROR_UNKNOWN), "unknown syscall returns -1");

	kputln("i386_init_threads_selftest: system-call request path passed");
	return true;
}

/* ---- 6: ring 3 ------------------------------------------------------------ */

/* What the basic user program observed, written from ring 3 (flat memory). */
typedef struct {
	uint32_t pid;
	uint32_t tid;
	uint32_t yield_result;
	uint32_t uptime_first;
	uint32_t uptime_second;
	uint32_t version_length;
	uint32_t write_result;
	uint32_t write_bad_address;
	uint32_t unknown_syscall;
	uint32_t bad_descriptor;
	uint32_t preserved_ok;
	uint32_t ebp_after;
	uint32_t finished;
} selftest_user_report_t;

/* Registers as they were on the very first instruction of a user thread. */
typedef struct {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint32_t esi;
	uint32_t edi;
	uint32_t ebp;
	uint32_t esp;
	uint32_t eflags;
	uint32_t cs;
	uint32_t ss;
	uint32_t ds;
} selftest_user_entry_state_t;

volatile selftest_user_report_t i386_selftest_user_report;
volatile selftest_user_entry_state_t i386_selftest_user_entry_state;
char i386_selftest_user_version[64];

void i386_selftest_user_entry(void);
void i386_selftest_user_main(void);
void i386_selftest_user_entry_tick(void);
void i386_selftest_user_fault_cli(void);
void i386_selftest_user_fault_ud2(void);
void i386_selftest_user_fault_div0(void);
void i386_selftest_user_fault_segment(void);

/* Called from the tick program's assembly (ring 3, cdecl). */
void i386_selftest_user_log(uint32_t value);

/*
 * The user programs. Entry points are assembly so the very first
 * instruction can see the initial register state the kernel promises:
 * eax = argument, everything else zero, esp = the stack top it was given.
 */
__asm__(
	".text\n"
	".globl i386_selftest_user_entry\n"
	".type i386_selftest_user_entry, @function\n"
	"i386_selftest_user_entry:\n"
	"	movl %eax, i386_selftest_user_entry_state+0\n"
	"	movl %ebx, i386_selftest_user_entry_state+4\n"
	"	movl %ecx, i386_selftest_user_entry_state+8\n"
	"	movl %edx, i386_selftest_user_entry_state+12\n"
	"	movl %esi, i386_selftest_user_entry_state+16\n"
	"	movl %edi, i386_selftest_user_entry_state+20\n"
	"	movl %ebp, i386_selftest_user_entry_state+24\n"
	"	movl %esp, i386_selftest_user_entry_state+28\n"
	"	pushfl\n"
	"	popl %eax\n"
	"	movl %eax, i386_selftest_user_entry_state+32\n"
	"	movl %cs, %eax\n"
	"	movl %eax, i386_selftest_user_entry_state+36\n"
	"	movl %ss, %eax\n"
	"	movl %eax, i386_selftest_user_entry_state+40\n"
	"	movl %ds, %eax\n"
	"	movl %eax, i386_selftest_user_entry_state+44\n"
	"	call i386_selftest_user_main\n"
	"1:	jmp 1b\n"
	".size i386_selftest_user_entry, . - i386_selftest_user_entry\n"

	/* Eight software ticks, logging 'u' after each, then exit(7). */
	".globl i386_selftest_user_entry_tick\n"
	".type i386_selftest_user_entry_tick, @function\n"
	"i386_selftest_user_entry_tick:\n"
	"	movl $8, %ecx\n"
	"2:	int $22\n"
	"	pushl %ecx\n"
	"	pushl $117\n"
	"	call i386_selftest_user_log\n"
	"	addl $4, %esp\n"
	"	popl %ecx\n"
	"	decl %ecx\n"
	"	jnz 2b\n"
	"	movl $1, %eax\n"
	"	movl $7, %ebx\n"
	"	int $0x80\n"
	"3:	jmp 3b\n"
	".size i386_selftest_user_entry_tick, . - i386_selftest_user_entry_tick\n"

	/* Faulting programs: each one raises a different exception in ring 3. */
	".globl i386_selftest_user_fault_cli\n"
	"i386_selftest_user_fault_cli:\n"
	"	cli\n"
	"	jmp i386_selftest_user_fault_cli\n"

	".globl i386_selftest_user_fault_ud2\n"
	"i386_selftest_user_fault_ud2:\n"
	"	ud2\n"

	".globl i386_selftest_user_fault_div0\n"
	"i386_selftest_user_fault_div0:\n"
	"	xorl %ecx, %ecx\n"
	"	movl $1, %eax\n"
	"	xorl %edx, %edx\n"
	"	divl %ecx\n"

	".globl i386_selftest_user_fault_segment\n"
	"i386_selftest_user_fault_segment:\n"
	"	movl $0x1238, %eax\n"
	"	movw %ax, %ds\n"
);

void i386_selftest_user_log(uint32_t value)
{
	log_put((char)value);
}

static inline uint32_t user_syscall(uint32_t number, uint32_t a, uint32_t b, uint32_t c)
{
	uint32_t result;

	__asm__ volatile("int $0x80" : "=a"(result) : "a"(number), "b"(a), "c"(b), "d"(c) : "memory");
	return result;
}

static const char g_user_message[] = "i386_init_threads_selftest: hello from ring 3\n";

/* The basic user program, running in ring 3. */
void i386_selftest_user_main(void)
{
	volatile selftest_user_report_t *report = &i386_selftest_user_report;

	report->pid = user_syscall(SYSCALL_GETPID, 0U, 0U, 0U);
	report->tid = user_syscall(SYSCALL_THREAD_SELF, 0U, 0U, 0U);
	report->yield_result = user_syscall(SYSCALL_YIELD, 0U, 0U, 0U);
	report->uptime_first = user_syscall(SYSCALL_UPTIME_US, 0U, 0U, 0U);
	report->uptime_second = user_syscall(SYSCALL_UPTIME_US, 0U, 0U, 0U);
	report->version_length = user_syscall(SYSCALL_GET_VERSION, (uint32_t)(uintptr_t)i386_selftest_user_version, sizeof(i386_selftest_user_version), 0U);
	report->write_result = user_syscall(SYSCALL_WRITE, 1U, (uint32_t)(uintptr_t)g_user_message, (uint32_t)sizeof(g_user_message) - 1U);
	report->write_bad_address = user_syscall(SYSCALL_WRITE, 1U, 0xC0000000U, 4U);
	report->unknown_syscall = user_syscall(0x7777U, 0U, 0U, 0U);
	report->bad_descriptor = user_syscall(SYSCALL_CLOSE, 0xFFFFU, 0U, 0U);

	/* Every register but eax must survive a system call (ebp included). */
	uint32_t eax = SYSCALL_GETPID;
	uint32_t ebx = 0xB0B0B0B0U;
	uint32_t ecx = 0xC0DEC0DEU;
	uint32_t edx = 0xD00DD00DU;
	uint32_t esi = 0x51515151U;
	uint32_t edi = 0xD1D1D1D1U;

	/*
	 * ebp is the frame pointer, so it cannot be a memory operand's base
	 * while it holds the test value: compare it inside the asm and hand
	 * the verdict back in eax, which is free after the call.
	 */
	__asm__ volatile(
		"pushl %%ebp\n\t"
		"movl $0xEBEBEBEB, %%ebp\n\t"
		"int $0x80\n\t"
		"xorl %%eax, %%eax\n\t"
		"cmpl $0xEBEBEBEB, %%ebp\n\t"
		"sete %%al\n\t"
		"popl %%ebp"
		: "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx), "+S"(esi), "+D"(edi)
		:
		: "memory", "cc"
	);

	report->ebp_after = eax;
	report->preserved_ok =
		eax == 1U && ebx == 0xB0B0B0B0U && ecx == 0xC0DEC0DEU && edx == 0xD00DD00DU &&
		esi == 0x51515151U && edi == 0xD1D1D1D1U;
	report->finished = 1U;

	(void)user_syscall(SYSCALL_EXIT, 42U, 0U, 0U);

	for (;;) {
	}
}

static bool g_tick_hook_ran;
static uint8_t g_tick_levels[16];
static uint32_t g_tick_count;

/* Software timer: an `int $22` from ring 3 charges one scheduler tick. */
static bool selftest_tick_hook(x86_saved_state_t *state)
{
	if (state->trapno != SELFTEST_TICK_VECTOR) return false;

	g_tick_hook_ran = true;
	sched_tick();

	if (g_tick_count < sizeof(g_tick_levels)) g_tick_levels[g_tick_count] = current_thread()->mlfq_level;
	g_tick_count++;
	return true;
}

typedef struct {
	proc_t proc;
	proc_id_t pid;
	thread_t thread;
} selftest_process_t;

static bool selftest_spawn(
	const char *name,
	void (*entry)(void),
	uint32_t argument,
	selftest_process_t *process
)
{
	if (g_user_stack_next >= SELFTEST_USER_STACKS) return false;

	uint8_t *stack = g_user_stacks[g_user_stack_next++];
	uint64_t stack_top = (uint64_t)(uintptr_t)stack + SELFTEST_USER_STACK_SIZE;
	uint64_t entry_address = (uint64_t)(uintptr_t)entry;

	if (!proc_create_user(proc_kernel(), name, entry_address, stack_top, &process->proc)) return false;

	process->pid = process->proc->p_ident.pid;
	process->thread = task_first_thread_ref(proc_task(process->proc));

	if (process->thread == 0) return false;

	/* The process's own first thread starts with argument 0; give this one a value. */
	if (argument != 0U && !thread_set_user_state(process->thread, entry_address, stack_top, argument)) return false;

	if (!thread_stack_alloc(process->thread)) return false;
	if (!proc_make_runnable(process->proc) || !proc_mark_running(process->proc)) return false;

	return sched_thread_start(process->thread);
}

static bool selftest_reap(selftest_process_t *process, uint64_t *status)
{
	SELFTEST_YIELD_UNTIL(proc_state(process->proc) == PROC_STATE_ZOMBIE);

	if (proc_state(process->proc) != PROC_STATE_ZOMBIE) return false;

	/* Let the terminated thread finish unwinding and be reaped by its successor. */
	(void)sched_yield();

	thread_deallocate(process->thread);
	return proc_reap(proc_kernel(), process->pid, status);
}

static bool test_ring3_syscalls(void)
{
	selftest_process_t process;
	uint64_t status = 0ULL;

	memset((void *)&i386_selftest_user_report, 0, sizeof(i386_selftest_user_report));
	memset((void *)&i386_selftest_user_entry_state, 0, sizeof(i386_selftest_user_entry_state));

	SELFTEST_CHECK(selftest_spawn("ring3-syscalls", i386_selftest_user_entry, 0U, &process), "spawn the basic ring 3 process");

	uint32_t tid = (uint32_t)thread_tid(process.thread);

	SELFTEST_CHECK(selftest_reap(&process, &status), "the basic ring 3 process exits");
	SELFTEST_CHECK(status == 42ULL, "exit status 42 reaches the parent");

	volatile selftest_user_report_t *report = &i386_selftest_user_report;
	volatile selftest_user_entry_state_t *entry = &i386_selftest_user_entry_state;

	SELFTEST_CHECK(report->finished == 1U, "user program ran to its exit call");

	/* The first instruction of a new user thread. */
	SELFTEST_CHECK(entry->eax == 0U, "initial eax is the argument (0)");
	SELFTEST_CHECK(entry->ebx == 0U && entry->ecx == 0U && entry->edx == 0U, "ebx, ecx, edx start at zero");
	SELFTEST_CHECK(entry->esi == 0U && entry->edi == 0U && entry->ebp == 0U, "esi, edi, ebp start at zero");
	SELFTEST_CHECK(entry->esp == (uint32_t)(uintptr_t)g_user_stacks[g_user_stack_next - 1U] + SELFTEST_USER_STACK_SIZE, "esp is the given stack, no return address pushed");
	SELFTEST_CHECK((entry->eflags & 0x200U) != 0U && (entry->eflags & 0x3000U) == 0U, "eflags has IF set and IOPL 0");
	SELFTEST_CHECK((entry->cs & 3U) == 3U && (entry->cs & 0xFFFFU) == GDT_USER_CODE_SEL, "runs at CPL 3 on the user code segment");
	SELFTEST_CHECK((entry->ss & 0xFFFFU) == GDT_USER_DATA_SEL && (entry->ds & 0xFFFFU) == GDT_USER_DATA_SEL, "user data segments loaded");

	/* The system calls. */
	SELFTEST_CHECK(report->pid == process.pid && report->pid != PROC_PID_KERNEL, "getpid returns the process's pid");
	SELFTEST_CHECK(report->tid == tid, "thread_self returns the thread's tid");
	SELFTEST_CHECK(report->yield_result == 0U, "yield returns 0");
	SELFTEST_CHECK(report->uptime_first != 0U && report->uptime_second >= report->uptime_first, "uptime is nonzero and monotonic");
	SELFTEST_CHECK(report->version_length != 0U && i386_selftest_user_version[report->version_length - 1U] == '\n', "get_version fills the user buffer");
	SELFTEST_CHECK(report->write_result == (uint32_t)sizeof(g_user_message) - 1U, "write returns the byte count");
	SELFTEST_CHECK(report->write_bad_address == (uint32_t)(0 - (int32_t)SYSCALL_ERROR_BAD_ADDRESS), "kernel-half pointer gives -BAD_ADDRESS");
	SELFTEST_CHECK(report->unknown_syscall == (uint32_t)(0 - (int32_t)SYSCALL_ERROR_UNKNOWN), "unknown syscall gives -1");
	SELFTEST_CHECK(report->bad_descriptor == (uint32_t)(0 - (int32_t)SYSCALL_ERROR_BAD_FD), "bad descriptor gives -BAD_FD");
	SELFTEST_CHECK(report->preserved_ok == 1U, "system call preserves every register but eax");

	/* A thread created with an initial argument (the pthread-create shape). */
	SELFTEST_CHECK(selftest_spawn("ring3-argument", i386_selftest_user_entry, 0xC0FFEEU, &process), "spawn with an argument");
	SELFTEST_CHECK(selftest_reap(&process, &status), "the second ring 3 process exits");
	SELFTEST_CHECK(entry->eax == 0xC0FFEEU, "initial eax carries the thread argument");
	SELFTEST_CHECK(entry->ebx == 0U && entry->ebp == 0U, "other registers still zero");

	kputln("i386_init_threads_selftest: ring 3 entry and system calls passed");
	return true;
}

typedef struct {
	const char *name;
	void (*entry)(void);
	uint32_t vector;
} selftest_fault_t;

static bool test_ring3_faults(void)
{
	static const selftest_fault_t faults[] = {
		{ "ring3-cli", i386_selftest_user_fault_cli, T_GENERAL_PROTECTION },
		{ "ring3-ud2", i386_selftest_user_fault_ud2, T_INVALID_OPCODE },
		{ "ring3-div0", i386_selftest_user_fault_div0, T_DIVIDE_ERROR },
		{ "ring3-segment", i386_selftest_user_fault_segment, T_GENERAL_PROTECTION }
	};

	for (uint32_t index = 0U; index < sizeof(faults) / sizeof(faults[0]); index++) {
		selftest_process_t process;
		uint64_t status = 0ULL;
		uint32_t baseline = thread_count();

		SELFTEST_CHECK(selftest_spawn(faults[index].name, faults[index].entry, 0U, &process), "spawn a faulting process");
		SELFTEST_CHECK(selftest_reap(&process, &status), "the faulting process is terminated and reaped");
		SELFTEST_CHECK(status == (uint64_t)(I386_USER_FAULT_STATUS_BASE | faults[index].vector), "exit status records the fault vector");
		SELFTEST_CHECK(thread_count() == baseline, "the faulting thread is gone");
		SELFTEST_CHECK(current_thread() == sched_bootstrap_thread(), "the kernel carries on");
	}

	SELFTEST_CHECK(sched_validate() && thread_validate(), "validate after user faults");

	kputln("i386_init_threads_selftest: ring 3 faults terminate only the offender");
	return true;
}

static bool test_ring3_preemption(void)
{
	thread_t self = current_thread();
	selftest_process_t process;
	thread_t observer;
	uint64_t status = 0ULL;

	/*
	 * Put ourselves one level down so the user thread (level 0) is scheduled
	 * ahead of us and, once demoted by its own ticks, shares our level.
	 */
	SELFTEST_CHECK(selftest_tick_to_level(1U), "demote the test thread to level 1");
	sched_clear_preemption();

	i386_idt_set_gate(
		(uint8_t)SELFTEST_TICK_VECTOR,
		i386_isr_table[SELFTEST_TICK_VECTOR],
		GDT_KERNEL_CODE_SEL,
		IDT_GATE_INTERRUPT_USER
	);
	i386_trap_set_user_exception_hook(selftest_tick_hook);

	log_reset();
	g_tick_count = 0U;
	g_tick_hook_ran = false;

	/* The user thread starts first; the observer waits behind it in the queue. */
	SELFTEST_CHECK(selftest_spawn("ring3-ticks", i386_selftest_user_entry_tick, 0U, &process), "spawn the ticking process");
	SELFTEST_CHECK(kernel_thread_create(proc_task(proc_kernel()), log_once_thread, (void *)(uintptr_t)'V', &observer), "create the observer");
	SELFTEST_CHECK(sched_thread_start(observer), "start the observer");

	bool reaped = selftest_reap(&process, &status);

	i386_trap_set_user_exception_hook(0);
	i386_idt_set_gate(
		(uint8_t)SELFTEST_TICK_VECTOR,
		i386_isr_table[SELFTEST_TICK_VECTOR],
		GDT_KERNEL_CODE_SEL,
		IDT_GATE_INTERRUPT
	);

	SELFTEST_CHECK(reaped, "the ticking process exits");
	SELFTEST_CHECK(status == 7ULL, "ticking process exit status");
	SELFTEST_CHECK(g_tick_hook_ran && g_tick_count == 8U, "eight software ticks reached the hook");

	/*
	 * Ticks 1-3 leave the user thread running; the fourth exhausts its
	 * level-0 quantum, so the trap-exit path preempts it for the observer
	 * before it logs, and it resumes afterwards.
	 */
	SELFTEST_CHECK(g_tick_levels[2] == 0U && g_tick_levels[3] == 1U, "the fourth tick demotes the user thread to level 1");
	SELFTEST_CHECK(log_equals("uuuVuuuuu"), "user thread preempted on trap exit for the observer (uuuVuuuuu)");

	SELFTEST_YIELD_UNTIL(thread_is_terminated(observer));
	(void)sched_yield();
	thread_deallocate(observer);

	SELFTEST_CHECK(selftest_boost_self(), "restore the test thread to level 0");
	SELFTEST_CHECK(self->mlfq_level == SCHED_MLFQ_TOP_LEVEL, "test thread back at level 0");
	SELFTEST_CHECK(sched_validate() && thread_validate(), "validate after the preemption test");

	kprintf("i386_init_threads_selftest: ring 3 preemption passed (log %s)\n", (const char *)g_log);
	return true;
}

static bool test_ring3(void)
{
	if (selftest_paging_enabled()) {
		kputln("i386_init_threads_selftest: paging is on, ring 3 stages skipped (need mapped user pages)");
		return true;
	}

	return test_ring3_syscalls() && test_ring3_faults() && test_ring3_preemption();
}

/* ---- the phase hook ------------------------------------------------------- */

bool i386_init_threads_selftest(const i386_boot_info_t *boot)
{
	(void)boot;

	if (!sched_is_initialized()) {
		kputln("i386_init_threads_selftest: scheduler is not up");
		return false;
	}

	uint32_t baseline = thread_count();

	selftest_mask_pic();

	bool passed =
		test_shared_scheduler() &&
		test_machine_thread() &&
		test_kernel_threads() &&
		test_tick_and_preempt() &&
		test_syscall_frame() &&
		test_ring3();

	selftest_restore_pic();

	if (passed && (thread_count() != baseline || g_thread_failures != 0U)) {
		kputln("i386_init_threads_selftest: FAIL leaked threads or a thread-side failure");
		passed = false;
	}

	return passed;
}
