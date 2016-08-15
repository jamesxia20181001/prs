/*
 *  Portable Runtime System (PRS)
 *  Copyright (C) 2016  Alexandre Tremblay
 *  
 *  This file is part of PRS.
 *  
 *  PRS is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as
 *  published by the Free Software Foundation, either version 3 of the
 *  License, or (at your option) any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *  
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *  
 *  portableruntimesystem@gmail.com
 */

/**
 * \file
 * \brief
 *  This file contains the stack allocator definitions.
 *
 *  The stack allocator reserves virtual memory for a task's stack. Since the stack grows backwards, the pointer
 *  returned by \ref prs_stack_create refers to the end of the memory area reserved for the stack. Memory pages at
 *  the end of the area are committed. When the stack overflows into pages that were not committed, an exception is
 *  generated by the operating system, which is then handled by the exception handler which can choose to grow the
 *  stack using \ref prs_stack_grow. Growing the stack consists in committing more pages of memory.
 *
 *  The maximum stack size is defined by the \ref PRS_MAX_STACK_SIZE macro.
 */

#include <prs/alloc/stack.h>
#include <prs/pal/arch.h>
#include <prs/pal/bitops.h>
#include <prs/pal/mem.h>
#include <prs/pal/os.h>
#include <prs/config.h>
#include <prs/error.h>
#include <prs/rtc.h>

/*
 * On Windows, the exception signal handler is run on the same stack on which the exception occurred. Because of that,
 * we have to use a single Windows-specific guard page that will trigger the exception when it is accessed.
 * The caveat of this method is that the vectored exception handler uses a _lot_ of stack space to set up itself
 * (more than a page - 4KB). Also, if we want to do some syscalls on top of that, we definitely end up using a lot of
 * memory. In order to avoid crashing the application when the stack overflow occurs, we set up 'extra' pages without
 * the guard flag so that the exception handler can proceed normally when the guard page is hit. Here is an example:
 *
 * [stack bottom|rw pages|rw guard page|rw 'extra' pages|no access pages|...|stack top]
 *
 * On Linux, we do not need the guard or extra pages as the signal handler for a segmentation fault can run on an
 * alternative stack. So it would look like this:
 *
 * [stack bottom|rw pages|no access pages|...|stack top]
 *
 * Note: On Linux, we run mprotect() in the signal handler to grow the stack, even though that function is not
 * explicitly said to be async-safe.
 *
 */
#if PRS_PAL_OS == PRS_PAL_OS_WINDOWS
#define PRS_STACK_EXTRA_PAGES           3
#else
#define PRS_STACK_EXTRA_PAGES           0
#endif

/**
 * \brief
 *  Creates a stack.
 * \param size
 *  Initial resquested size of the stack.
 * \param available_size
 *  Actual stack size that was committed to memory.
 * \return
 *  Pointer to the end of the stack.
 * \note
 *  The \p available_size parameter returns the requested size aligned to a multiple of the system's page size.
 */
void* prs_stack_create(prs_size_t size, prs_size_t* available_size)
{
    PRS_PRECONDITION(size > 0);

    const prs_size_t page_size = prs_pal_os_get_page_size();
    const prs_size_t aligned_size = prs_bitops_align_size(size, page_size);

    PRS_RTC_IF (aligned_size > PRS_MAX_STACK_SIZE) {
        return 0;
    }

    PRS_ASSERT(aligned_size == prs_bitops_align_size(aligned_size, page_size));

    void* stack_bottom = prs_pal_mem_map(PRS_MAX_STACK_SIZE, 0);
    PRS_ERROR_IF (!stack_bottom) {
        return 0;
    }
    const prs_size_t protected_size = PRS_MAX_STACK_SIZE - aligned_size;
    void* stack_limit = (void*)((prs_uintptr_t)stack_bottom + protected_size);

#if PRS_PAL_OS == PRS_PAL_OS_WINDOWS
    const prs_size_t extra_size = page_size * (1 + PRS_STACK_EXTRA_PAGES);

    PRS_RTC_IF (aligned_size > PRS_MAX_STACK_SIZE - extra_size) {
        return 0;
    }

    void* stack_extra = (void*)((prs_uintptr_t)stack_limit - extra_size);
    void* stack_guard = (void*)((prs_uintptr_t)stack_limit - page_size);
    prs_pal_mem_commit(stack_extra, page_size * PRS_STACK_EXTRA_PAGES, PRS_PAL_MEM_FLAG_READ | PRS_PAL_MEM_FLAG_WRITE);
    prs_pal_mem_commit(stack_guard, page_size, PRS_PAL_MEM_FLAG_GUARD | PRS_PAL_MEM_FLAG_READ | PRS_PAL_MEM_FLAG_WRITE);
#endif /* PRS_PAL_OS_WINDOWS */

    prs_pal_mem_commit(stack_limit, aligned_size, PRS_PAL_MEM_FLAG_READ | PRS_PAL_MEM_FLAG_WRITE);

    void* stack = (void*)((prs_uintptr_t)stack_bottom + PRS_MAX_STACK_SIZE);
    *available_size = aligned_size;
    return stack;
}

/**
 * \brief
 *  Destroys a stack.
 * \param stack
 *  Stack to destroy. Must be a value returned by \ref prs_stack_create.
 */
void prs_stack_destroy(void* stack)
{
    void* stack_bottom = (void*)((prs_uintptr_t)stack - PRS_MAX_STACK_SIZE);
    prs_pal_mem_unmap(stack_bottom, PRS_MAX_STACK_SIZE);
}

/**
 * \brief
 *  Grows a stack.
 * \param stack
 *  Stack to grow. Must be a value returned by \ref prs_stack_create.
 * \param old_size
 *  Previous stack size. Must be a value obtained from \prs_stack_create (\p available_size) or \prs_stack_grow
 *  (\p new_size).
 * \param failed_ptr
 *  The address that causes an overflow in the stack. The stack will be grown so that this address is accessible.
 * \param new_size
 *  New size of the stack, aligned to the next page boundary.
 * \return
 *  \ref PRS_TRUE if the stack could be grown.
 *  \ref PRS_FALSE if the stack could not be grown.
 */
prs_bool_t prs_stack_grow(void* stack, prs_size_t old_size, void* failed_ptr, prs_size_t* new_size)
{
    PRS_PRECONDITION(stack);

    const prs_size_t page_size = prs_pal_os_get_page_size();
#if PRS_PAL_OS == PRS_PAL_OS_WINDOWS
    const prs_size_t grow_size = (1 + PRS_STACK_EXTRA_PAGES) * page_size;
    const prs_size_t aligned_new_size = old_size + grow_size;
#else
    const prs_size_t required_size = (prs_uintptr_t)stack - (prs_uintptr_t)failed_ptr;
    const prs_size_t grow_size = prs_bitops_align_size(required_size, page_size);
    PRS_ASSERT(required_size > old_size);
    const prs_size_t aligned_new_size = prs_bitops_align_size(required_size, page_size);
#endif

    if (aligned_new_size > PRS_MAX_STACK_SIZE) {
        return PRS_FALSE;
    }

    void* stack_bottom = (void*)((prs_uintptr_t)stack - PRS_MAX_STACK_SIZE);
    const prs_size_t protected_size = PRS_MAX_STACK_SIZE - aligned_new_size;
    void* stack_limit = (void*)((prs_uintptr_t)stack_bottom + protected_size);

#if PRS_PAL_OS == PRS_PAL_OS_WINDOWS
    if (aligned_new_size > PRS_MAX_STACK_SIZE - grow_size) {
        return PRS_FALSE;
    }

    void* stack_extra = (void*)((prs_uintptr_t)stack_limit - grow_size);
    void* stack_guard = (void*)((prs_uintptr_t)stack_limit - page_size);
    prs_pal_mem_commit(stack_extra, page_size * PRS_STACK_EXTRA_PAGES, PRS_PAL_MEM_FLAG_READ | PRS_PAL_MEM_FLAG_WRITE);
    prs_pal_mem_commit(stack_guard, page_size, PRS_PAL_MEM_FLAG_GUARD | PRS_PAL_MEM_FLAG_READ | PRS_PAL_MEM_FLAG_WRITE);
#endif /* PRS_PAL_OS_WINDOWS */

    prs_pal_mem_commit(stack_limit, grow_size, PRS_PAL_MEM_FLAG_READ | PRS_PAL_MEM_FLAG_WRITE);

    *new_size = aligned_new_size;

    return PRS_TRUE;
}

/**
 * \brief
 *  Returns if the provided address is within the specified stack's range.
 * \param stack
 *  Stack to check. Must be a value returned by \ref prs_stack_create.
 * \param address
 *  Address to verify.
 */
prs_bool_t prs_stack_address_in_range(void* stack, void* address)
{
    const prs_uintptr_t ptr = (prs_uintptr_t)address;
    const prs_uintptr_t start = (prs_uintptr_t)stack - PRS_MAX_STACK_SIZE;
    const prs_uintptr_t end = (prs_uintptr_t)stack;
    return (ptr >= start && ptr < end);
}