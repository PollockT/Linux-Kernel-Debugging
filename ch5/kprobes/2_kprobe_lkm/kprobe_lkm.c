/*
 * ch5/kprobes/kprobe_lkm/kprobe_lkm.c
 ***************************************************************
 * This program is part of the source code released for the book
 *  "Linux Kernel Debugging"
 *  (c) Author: Kaiwan N Billimoria
 *  Publisher:  Packt
 *  GitHub repository:
 *  https://github.com/PacktPublishing/Linux-Kernel-Debugging
 *
 * From: Ch 5: Debug via Instrumentation - printk and friends
 ****************************************************************
 * Brief Description:
 *
 * For details, please refer the book, Ch 5.
 */
#define pr_fmt(fmt) "%s:%s(): " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include "../../../convenient.h"

MODULE_AUTHOR("<insert your name here>");
MODULE_DESCRIPTION("LKD book:ch5/kprobes/kprobe_lkm: simple Kprobes demo module");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION("0.1");

static spinlock_t lock;
static struct kprobe kpb;
static u64 tm_start, tm_end;
static char *fname;

#define MAX_FUNCNAME_LEN  64
static char kprobe_func[MAX_FUNCNAME_LEN];
module_param_string(kprobe_func, kprobe_func, sizeof(kprobe_func), 0);
MODULE_PARM_DESC(kprobe_func, "function name to attach a kprobe to");

static int verbose;
module_param(verbose, int, 0644);
MODULE_PARM_DESC(verbose, "Set to 1 to get verbose printk's (defaults to 0).");

/*
 * This probe runs just prior to the function "kprobe_func()" is invoked.
 */
static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	PRINT_CTX();
	/*
	 * Here, we're assuming you've setup a kprobe into the do_sys_open().
	 * We want the filename; to get it, we *must* copy it in from it's userspace
	 * buffer, the pointer to which is in the (R)SI CPU register (on x86).
	 * Using strncpy_from_user() here is considered a bug! as we're in an atomic
	 * context in this kprobe pre-handler...
	 * [ ... ]
	 * [ 2552.898142] BUG: sleeping function called from invalid context at lib/strncpy_from_user.c:117
	 * [ 2552.904085] in_atomic(): 1, irqs_disabled(): 0, non_block: 0, pid: 390, name: systemd-journal
	 * [ ... ]
	 * [ 2542.112886] Call Trace:
	 * [ 2542.112892]  dump_stack+0xbd/0xfa
	 * [ 2542.112897]  ___might_sleep.cold+0x63/0x74
	 * [ 2542.112902]  __might_sleep+0x73/0xe0
	 * [ 2542.112908]  __might_fault+0x52/0xd0
	 * [ 2542.112912]  strncpy_from_user+0x2b/0x280
	 * [ 2542.112919]  ? handler_pre+0x1dd/0x2e0 [kprobe_lkm]
	 * [ 2542.118917] [390] kprobe_lkm:handler_pre(): 003)  systemd-journal :390   |  ...1   \* handler_pre() *\
	 * [ 2542.124541]  handler_pre+0x97/0x2e0 [kprobe_lkm]
	 * [ ... ]
	 * (shows up ONLY on our debug kernel!)
	 * Not really much choice here, we use it ...   :-/
	 */
#if 1
	if (!strncpy_from_user(fname, (const char __user *)regs->si, PATH_MAX + 1))
#else
	/* Attempting to use the 'usual' copy_from_user() here simply causes a hard
	 * hang... avoid it */
	if (!copy_from_user(fname, (const char __user *)regs->si,
			    strnlen_user((const char __user *)regs->si, PATH_MAX + 1)))
#endif
		return -EFAULT;

	/* For this demo, lets filter on the process context being 'vi' only; else
	 * we'll get far too much log info to reasonably process...
	 */
	if (strncmp(current->comm, "vi", 2))
		return 0;

	pr_info("FILE being opened: ptr [R]SI: %px   name:%s\n", (void *)regs->si, fname);

	spin_lock(&lock);
	tm_start = ktime_get_real_ns();
	spin_unlock(&lock);

	return 0;
}

/*
 * This probe runs immediately after the function "kprobe_func()" completes.
 */
static void handler_post(struct kprobe *p, struct pt_regs *regs, unsigned long flags)
{
	spin_lock(&lock);
	tm_end = ktime_get_real_ns();

	if (verbose)
		PRINT_CTX();

	SHOW_DELTA(tm_end, tm_start);
	spin_unlock(&lock);
}

/*
 * fault_handler: this is called if an exception is generated for any
 * instruction within the pre- or post-handler, or when Kprobes
 * single-steps the probed instruction.
 */
static int handler_fault(struct kprobe *p, struct pt_regs *regs, int trapnr)
{
	pr_info("fault_handler: p->addr = 0x%p, trap #%dn", p->addr, trapnr);
	/* Return 0 because we don't handle the fault. */
	return 0;
}

NOKPROBE_SYMBOL(handler_fault);

static int __init kprobe_lkm_init(void)
{
	/* Verify that the function to kprobe has been passed as a parameter to
	 * this module
	 */
	if (kprobe_func[0] == '\0') {
		pr_warn("expect a valid kprobe_func=<func_name> module parameter");
		return -EINVAL;
	}
	/********* Possible SECURITY concern:
     * We just assume the pointer passed is valid and okay.
	 * Minimally, ensure that the passed function is NOT marked with any of:
	 * __kprobes or nokprobe_inline annotation nor marked via the NOKPROBE_SYMBOL
	 * macro
	 */
	fname = kzalloc(PATH_MAX + 1, GFP_ATOMIC);
	if (unlikely(!fname))
		return -ENOMEM;

	/* Register the kprobe handler */
	kpb.pre_handler = handler_pre;
	kpb.post_handler = handler_post;
	kpb.fault_handler = handler_fault;
	kpb.symbol_name = kprobe_func;
	if (register_kprobe(&kpb)) {
		pr_alert("register_kprobe failed!\n\
Check: is function '%s' invalid, static, inline; or blacklisted: attribute-marked '__kprobes'\n\
or nokprobe_inline, or is marked with the NOKPROBE_SYMBOL macro?\n", kprobe_func);
		return -EINVAL;
	}
	pr_info("registering kernel probe @ '%s'\n", kprobe_func);
	spin_lock_init(&lock);

	return 0;		/* success */
}

static void __exit kprobe_lkm_exit(void)
{
	kfree(fname);
	unregister_kprobe(&kpb);
	pr_info("bye, unregistering kernel probe @ '%s'\n", kprobe_func);
}

module_init(kprobe_lkm_init);
module_exit(kprobe_lkm_exit);