#include <inc/mmu.h>
#include <inc/x86.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/env.h>
#include <kern/syscall.h>
#include <kern/sched.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/time.h>
#include <kern/e100.h>
#include <kern/cpu.h>

/* Interrupt descriptor table.  (Must be built at run time because
 * shifted function addresses can't be represented in relocation records.)
 */
struct Gatedesc idt[256] = { { 0 } };
struct Pseudodesc idt_pd = {
	sizeof(idt) - 1, (uint32_t) idt
};


static const char *trapname(int trapno)
{
	static const char * const excnames[] = {
		"Divide error",
		"Debug",
		"Non-Maskable Interrupt",
		"Breakpoint",
		"Overflow",
		"BOUND Range Exceeded",
		"Invalid Opcode",
		"Device Not Available",
		"Double Fault",
		"Coprocessor Segment Overrun",
		"Invalid TSS",
		"Segment Not Present",
		"Stack Fault",
		"General Protection",
		"Page Fault",
		"(unknown trap)",
		"x87 FPU Floating-Point Error",
		"Alignment Check",
		"Machine-Check",
		"SIMD Floating-Point Exception"
	};

	if (trapno < sizeof(excnames)/sizeof(excnames[0]))
		return excnames[trapno];
	if (trapno == T_SYSCALL)
		return "System call";
	if (trapno >= IRQ_OFFSET && trapno < IRQ_OFFSET + 16)
		return "Hardware Interrupt";
	return "(unknown trap)";
}


extern uint32_t vectors[];  // in trapentry.S: array of 256 entry pointers
void
idt_init(void)
{
        int i = 0;
        /* Only one kernel stack, as opposed to one per process in xv6.
           The kernel is not re-entrant (cannot be interrupted), so all IDT entries are interrupt gates.
        */
        for (; i < 256; i++)
          switch (i) {
          /* Enable "int 3" for user space */
          case T_BRKPT:
          case T_SYSCALL:
            SETGATE(idt[i], 0, GD_KT, vectors[i], DPL_USER);
            break;
          default:
          SETGATE(idt[i], 0, GD_KT, vectors[i], 0);
          }

	cpu *c = cpu_cur();

	// Setup the TSS for this cpu so that we get the right stack
	// when we trap into the kernel from user mode.
	//c->tss.ts_esp0 = (uint32_t) c->kstackhi;
        c->tss.ts_esp0 = KSTACKTOP;
	c->tss.ts_ss0 = GD_KD;

	// Initialize the non-constant part of the cpu's GDT:
	// the TSS descriptor is different for each cpu.
	c->gdt[GD_TSS >> 3] = SEG16(STS_T32A, (uint32_t) (&c->tss),
					sizeof(struct Taskstate)-1, 0);
	c->gdt[GD_TSS >> 3].sd_s = 0;

	// Load the TSS (from the GDT)
	ltr(GD_TSS);
}

void
trap_init(void)
{
	// The first time we get called on the bootstrap processor,
	// initialize the IDT.  Other CPUs will share the same IDT.
	if (cpu_onboot())
		idt_init();

	// Load the IDT into this processor's IDT register.
	asm volatile("lidt idt_pd");
}

void
print_trapframe(struct Trapframe *tf)
{
	cprintf("TRAP frame at %p\n", tf);
	print_regs(&tf->tf_regs);
	cprintf("  es   0x----%04x\n", tf->tf_es);
	cprintf("  ds   0x----%04x\n", tf->tf_ds);
	cprintf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
	cprintf("  err  0x%08x\n", tf->tf_err);
	cprintf("  eip  0x%08x\n", tf->tf_eip);
	cprintf("  cs   0x----%04x\n", tf->tf_cs);
	cprintf("  flag 0x%08x\n", tf->tf_eflags);
	cprintf("  esp  0x%08x\n", tf->tf_esp);
	cprintf("  ss   0x----%04x\n", tf->tf_ss);
}

void
print_regs(struct PushRegs *regs)
{
	cprintf("  edi  0x%08x\n", regs->reg_edi);
	cprintf("  esi  0x%08x\n", regs->reg_esi);
	cprintf("  ebp  0x%08x\n", regs->reg_ebp);
	cprintf("  oesp 0x%08x\n", regs->reg_oesp);
	cprintf("  ebx  0x%08x\n", regs->reg_ebx);
	cprintf("  edx  0x%08x\n", regs->reg_edx);
	cprintf("  ecx  0x%08x\n", regs->reg_ecx);
	cprintf("  eax  0x%08x\n", regs->reg_eax);
}

static void
trap_dispatch(struct Trapframe *tf)
{
  switch (tf->tf_trapno) {
  //// Lab 3: Handle processor exceptions.
  case T_PGFLT:
    page_fault_handler(tf);
    return;
  case T_BRKPT:
    monitor(tf);
    return;
  case T_DEBUG:
    monitor_ss(tf);
    return;
  case T_SYSCALL:
    {
      struct PushRegs *regs = &tf->tf_regs;
      int ret = syscall(regs->reg_eax, regs->reg_edx, regs->reg_ecx,
                        regs->reg_ebx, regs->reg_edi, regs->reg_esi);
      regs->reg_eax = ret;
    }
    return;
    
  //// LAB 4: Handle clock interrupts.
  case IRQ_OFFSET + IRQ_TIMER:
  //// Lab 6: Add time tick increment to clock interrupts.
    time_tick();
    sched_yield();
    return;

    // Handle spurious interupts
    // The hardware sometimes raises these because of noise on the
    // IRQ line or other reasons. We don't care.
  case IRQ_OFFSET + IRQ_SPURIOUS:
      cprintf("Spurious interrupt on irq 7\n");
      print_trapframe(tf);
      return;

  //// Lab 7: Handle keyboard interrupts.
  case IRQ_OFFSET + IRQ_KBD:
    kbd_intr();
    return;
  case IRQ_OFFSET + IRQ_SERIAL:
    serial_intr();
    return;

  default:
    if (tf->tf_trapno == IRQ_OFFSET + e100_irq) {
      e100_intr();
      irq_eoi();
      return;
    }

    // Unexpected trap: The user process or the kernel has a bug.
    print_trapframe(tf);
    if (tf->tf_cs == GD_KT)
      panic("unhandled trap in kernel");
    else
      env_destroy(curenv);
  }
}
	
static int
single_step_enabled(void)
{
	int ret = 0;
	uint32_t dr6;
	const uint32_t sstep = 0x4000;

	dr6 = rdr6();
	if (dr6 & sstep) {
		dr6 &= ~sstep;
		ldr6(dr6);
		ret = 1;
	}

	return ret;
}

void
trap(struct Trapframe *tf)
{
	if ((tf->tf_cs & 3) == 3) {
		// Trapped from user mode.
		// Copy trap frame (which is currently on the stack)
		// into 'curenv->env_tf', so that running the environment
		// will restart at the trap point.
		assert(curenv);
		curenv->env_tf = *tf;
		// The trapframe on the stack should be ignored from here on.
		tf = &curenv->env_tf;
	}
	
	// Dispatch based on what type of trap occurred
	trap_dispatch(tf);

 	if (single_step_enabled())
 		return;

	// If we made it to this point, then no other environment was
	// scheduled, so we should return to the current environment
	// if doing so makes sense.
	if (curenv && curenv->env_status == ENV_RUNNABLE)
		env_run(curenv);
	else
		sched_yield();
}


void
enable_sep(void)
{
	wrmsr(0x174, GD_KT, 0);             /* SYSENTER_CS_MSR */
	wrmsr(0x175, KSTACKTOP, 0);         /* SYSENTER_ESP_MSR */
	wrmsr(0x176, (uint32_t) sysenter_handler, 0);  /* SYSENTER_EIP_MSR */
}

void
page_fault_handler(struct Trapframe *tf)
{
	uint32_t fault_va;

	// Read processor's CR2 register to find the faulting address
	fault_va = rcr2();

	// Handle kernel-mode page faults.
        if (tf->tf_cs == GD_KT)
          panic("Page fault in kernel");

	// We've already handled kernel-mode exceptions, so if we get here,
	// the page fault happened in user mode.

	// Call the environment's page fault upcall, if one exists.  Set up a
	// page fault stack frame on the user exception stack (below
	// UXSTACKTOP), then branch to curenv->env_pgfault_upcall.
        if (curenv->env_pgfault_upcall) {
          uintptr_t stacktop = UXSTACKTOP;

	// The page fault upcall might cause another page fault, in which case
	// we branch to the page fault upcall recursively, pushing another
	// page fault stack frame on top of the user exception stack.
          if (tf->tf_esp < UXSTACKTOP && tf->tf_esp >= UXSTACKTOP-PGSIZE)
	// The trap handler needs one word of scratch space at the top of the
	// trap-time stack in order to return.  In the non-recursive case, we
	// don't have to worry about this because the top of the regular user
	// stack is free.  In the recursive case, this means we have to leave
	// an extra word between the current top of the exception stack and
	// the new stack frame because the exception stack _is_ the trap-time
	// stack.
            stacktop = tf->tf_esp - sizeof(uintptr_t);

          stacktop -= sizeof(struct UTrapframe);
          struct UTrapframe *u = (struct UTrapframe*) stacktop;
          user_mem_assert(curenv, u, sizeof (struct UTrapframe), PTE_W);
          u->utf_fault_va = fault_va;
          u->utf_err = tf->tf_err;
          u->utf_regs = tf->tf_regs;
          u->utf_eip = tf->tf_eip;
          u->utf_eflags = tf->tf_eflags;
          u->utf_esp = tf->tf_esp;

          tf->tf_esp = stacktop;
          tf->tf_eip = (uintptr_t)curenv->env_pgfault_upcall;
          env_run(curenv);
        }

	// If there's no page fault upcall, the environment didn't allocate a
	// page for its exception stack or can't write to it, or the exception
	// stack overflows, then destroy the environment that caused the fault.
	cprintf("[%08x] user fault va %08x ip %08x\n",
		curenv->env_id, fault_va, tf->tf_eip);
	print_trapframe(tf);

	env_destroy(curenv);
}
