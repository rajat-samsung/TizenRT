/****************************************************************************
 *
 * Copyright 2016 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/
/****************************************************************************
 * arch/arm/src/common/up_vfork.c
 *
 *   Copyright (C) 2013 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <tinyara/config.h>

#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <tinyara/sched.h>
#include <tinyara/arch.h>
#include <arch/irq.h>

#ifdef CONFIG_DEBUG_MM_HEAPINFO
#include <tinyara/mm/mm.h>
#endif

#include "up_vfork.h"
#include "up_internal.h"
#include "sched/sched.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_vfork
 *
 * Description:
 *   The vfork() function has the same effect as fork(), except that the
 *   behavior is undefined if the process created by vfork() either modifies
 *   any data other than a variable of type pid_t used to store the return
 *   value from vfork(), or returns from the function in which vfork() was
 *   called, or calls any other function before successfully calling _exit()
 *   or one of the exec family of functions.
 *
 *   The overall sequence is:
 *
 *   1) User code calls vfork().  vfork() collects context information and
 *      transfers control up up_vfork().
 *   2) up_vfork()and calls task_vforksetup().
 *   3) task_vforksetup() allocates and configures the child task's TCB.  This
 *      consists of:
 *      - Allocation of the child task's TCB.
 *      - Initialization of file descriptors and streams
 *      - Configuration of environment variables
 *      - Setup the input parameters for the task.
 *      - Initialization of the TCB (including call to up_initial_state()
 *   4) up_vfork() provides any additional operating context. up_vfork must:
 *      - Allocate and initialize the stack
 *      - Initialize special values in any CPU registers that were not
 *        already configured by up_initial_state()
 *   5) up_vfork() then calls task_vforkstart()
 *   6) task_vforkstart() then executes the child thread.
 *
 * task_vforkabort() may be called if an error occurs between steps 3 and 6.
 *
 * Input Parameters:
 *   context - Caller context information saved by vfork()
 *
 * Return:
 *   Upon successful completion, vfork() returns 0 to the child process and
 *   returns the process ID of the child process to the parent process.
 *   Otherwise, -1 is returned to the parent, no child process is created,
 *   and errno is set to indicate the error.
 *
 ****************************************************************************/

pid_t up_vfork(const struct vfork_s *context)
{
	struct tcb_s *parent = this_task();
	struct task_tcb_s *child;
	size_t stacksize;
	uint32_t newsp;
	uint32_t newfp;
	uint32_t stackutil;
	int ret;

	svdbg("vfork context [%p]:\n", context);
	svdbg("  r4:%08x r5:%08x r6:%08x r7:%08x\n", context->r4, context->r5, context->r6, context->r7);
	svdbg("  r8:%08x r9:%08x r10:%08x\n", context->r8, context->r9, context->r10);
	svdbg("  fp:%08x sp:%08x lr:%08x\n", context->fp, context->sp, context->lr);

	/* Allocate and initialize a TCB for the child task. */

	child = task_vforksetup((start_t)(context->lr & ~1));
	if (!child) {
		sdbg("ERROR: task_vforksetup failed\n");
		return (pid_t)ERROR;
	}

	svdbg("TCBs: Parent=%p Child=%p\n", parent, child);

	/* Get the size of the parent task's stack.  Due to alignment operations,
	 * the adjusted stack size may be smaller than the stack size originally
	 * requested.
	 */

	stacksize = parent->adj_stack_size + STACK_ALIGNMENT - 1;

	/* Allocate the stack for the TCB */

	ret = up_create_stack((FAR struct tcb_s *)child, stacksize, parent->flags & TCB_FLAG_TTYPE_MASK);
	if (ret != OK) {
		sdbg("ERROR: up_create_stack failed: %d\n", ret);
		task_vforkabort(child, -ret);
		return (pid_t)ERROR;
	}
#ifdef CONFIG_DEBUG_MM_HEAPINFO
	/* Exclude a stack node from heap usages of current thread.
	 * This will be shown separately as stack usages.
	 */
	heapinfo_exclude_stacksize(child->cmn.stack_alloc_ptr);
	/* Update the pid information to set a stack node */
	heapinfo_set_stack_node(child->cmn.stack_alloc_ptr, child->cmn.pid);
#endif

	/* How much of the parent's stack was utilized?  The ARM uses
	 * a push-down stack so that the current stack pointer should
	 * be lower than the initial, adjusted stack pointer.  The
	 * stack usage should be the difference between those two.
	 */

	DEBUGASSERT((uint32_t)parent->adj_stack_ptr > context->sp);
	stackutil = (uint32_t)parent->adj_stack_ptr - context->sp;

	svdbg("Parent: stacksize:%d stackutil:%d\n", stacksize, stackutil);

	/* Make some feeble effort to preserve the stack contents.  This is
	 * feeble because the stack surely contains invalid pointers and other
	 * content that will not work in the child context.  However, if the
	 * user follows all of the caveats of vfork() usage, even this feeble
	 * effort is overkill.
	 */

	newsp = (uint32_t)child->cmn.adj_stack_ptr - stackutil;
	memcpy((void *)newsp, (const void *)context->sp, stackutil);

	/* Was there a frame pointer in place before? */

	if (context->fp <= (uint32_t)parent->adj_stack_ptr && context->fp >= (uint32_t)parent->adj_stack_ptr - stacksize) {
		uint32_t frameutil = (uint32_t)parent->adj_stack_ptr - context->fp;
		newfp = (uint32_t)child->cmn.adj_stack_ptr - frameutil;
	} else {
		newfp = context->fp;
	}

	svdbg("Parent: stack base:%08x SP:%08x FP:%08x\n", parent->adj_stack_ptr, context->sp, context->fp);
	svdbg("Child:  stack base:%08x SP:%08x FP:%08x\n", child->cmn.adj_stack_ptr, newsp, newfp);

	/* Update the stack pointer, frame pointer, and volatile registers.  When
	 * the child TCB was initialized, all of the values were set to zero.
	 * up_initial_state() altered a few values, but the return value in R0
	 * should be cleared to zero, providing the indication to the newly started
	 * child thread.
	 */

	child->cmn.xcp.regs[REG_R4] = context->r4;	/* Volatile register r4 */
	child->cmn.xcp.regs[REG_R5] = context->r5;	/* Volatile register r5 */
	child->cmn.xcp.regs[REG_R6] = context->r6;	/* Volatile register r6 */
	child->cmn.xcp.regs[REG_R7] = context->r7;	/* Volatile register r7 */
	child->cmn.xcp.regs[REG_R8] = context->r8;	/* Volatile register r8 */
	child->cmn.xcp.regs[REG_R9] = context->r9;	/* Volatile register r9 */
	child->cmn.xcp.regs[REG_R10] = context->r10;	/* Volatile register r10 */
	child->cmn.xcp.regs[REG_FP] = newfp;	/* Frame pointer */
	child->cmn.xcp.regs[REG_SP] = newsp;	/* Stack pointer */

#ifdef CONFIG_LIB_SYSCALL
	/* If we got here via a syscall, then we are going to have to setup some
	 * syscall return information as well.
	 */

	if (parent->xcp.nsyscalls > 0) {
		int index;
		for (index = 0; index < parent->xcp.nsyscalls; index++) {
			child->cmn.xcp.syscall[index].sysreturn = parent->xcp.syscall[index].sysreturn;

			/* REVISIT:  This logic is *not* common. */

#if (defined(CONFIG_ARCH_CORTEXA5) || defined(CONFIG_ARCH_CORTEXA8)) && \
	 defined(CONFIG_BUILD_KERNEL)

			child->cmn.xcp.syscall[index].cpsr = parent->xcp.syscall[index].cpsr;

#elif defined(CONFIG_ARCH_CORTEXR4) || defined(CONFIG_ARCH_CORTEXR4F)
#ifdef CONFIG_BUILD_PROTECTED

			child->cmn.xcp.syscall[index].cpsr = parent->xcp.syscall[index].cpsr;

#endif
#elif defined(CONFIG_ARCH_CORTEXM3) || defined(CONFIG_ARCH_CORTEXM4) || \
	  defined(CONFIG_ARCH_CORTEXM0) || defined(CONFIG_ARCH_CORTEXM7) || \
	  defined(CONFIG_ARCH_CORTEXM33) || defined(CONFIG_ARCH_CORTEXM55)

			child->cmn.xcp.syscall[index].excreturn = parent->xcp.syscall[index].excreturn;
#else
#error Missing logic
#endif
		}

		child->cmn.xcp.nsyscalls = parent->xcp.nsyscalls;
	}
#endif

	/* And, finally, start the child task.  On a failure, task_vforkstart()
	 * will discard the TCB by calling task_vforkabort().
	 */

	return task_vforkstart(child);
}
