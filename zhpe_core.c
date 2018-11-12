/*
 * Copyright (C) 2017-2018 Hewlett Packard Enterprise Development LP.
 * All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/uaccess.h>
#include <linux/pci.h>
#include <linux/amd-iommu.h>
#include <linux/interrupt.h>
#include <linux/cdev.h>

#if LINUX_VERSION_CODE >=  KERNEL_VERSION(4, 11, 0)
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#endif
#include <zhpe.h>
#include <zhpe_driver.h>

#ifndef NDEBUG
module_param_named(debug, zhpe_debug_flags, uint, 0644);
MODULE_PARM_DESC(debug, "debug output bitmask");
#endif /* !NDEBUG */

const char zhpe_driver_name[] = DRIVER_NAME;

static atomic64_t mem_total = ATOMIC64_INIT(0);

static atomic_t slice_id = ATOMIC_INIT(0);

static union zpages            *global_shared_zpage;
struct zhpe_global_shared_data *global_shared_data;

static struct pci_device_id zhpe_id_table[] = {
    { PCI_VDEVICE(HP_3PAR, 0x028f), }, /* Function 0 */
    { PCI_VDEVICE(HP_3PAR, 0x0290), }, /* Function 1 */
    { 0 },
};

MODULE_DEVICE_TABLE(pci, zhpe_id_table);


/* Revisit Carbon: Workaround for Carbon simulator not having AVX instructions
 * and ymm registers, but also not requiring 16/32-byte accesses
 */
uint zhpe_no_avx = 1;
module_param_named(no_avx, zhpe_no_avx, uint, S_IRUGO);
MODULE_PARM_DESC(no_avx, "Workaround for lack of AVX instructions/registers");

uint zhpe_no_rkeys = 1;
module_param_named(no_rkeys, zhpe_no_rkeys, uint, S_IRUGO);
MODULE_PARM_DESC(no_rkeys, "Disable Gen-Z R-keys");

static int __init zhpe_init(void);
static void zhpe_exit(void);

module_init(zhpe_init);
module_exit(zhpe_exit);

MODULE_LICENSE("GPL");

struct bridge    zhpe_bridge = { 0 };
struct sw_page_grid     sw_pg[PAGE_GRID_ENTRIES];

static DECLARE_WAIT_QUEUE_HEAD(poll_wqh);

uint no_iommu = 0;
module_param(no_iommu, uint, 0444);
MODULE_PARM_DESC(no_iommu, "System does not have an IOMMU (default=0)");

/* "libfabric" is the default for now. */
static char *backend = "libfabric";
module_param(backend, charp, 0444);
MODULE_PARM_DESC(backend, "backend transport: zhpe, libfabric (default)");

#define TRACKER_MAX     (256)

/* Revisit Carbon: Gen-Z Global CID should come from bridge Core
 * Structure, but for now, it's a module parameter
 */
uint genz_gcid = 0x0000001;  /* Revisit Carbon: carbon node 1 */
module_param(genz_gcid, uint, S_IRUGO);
MODULE_PARM_DESC(genz_gcid, "Gen-Z bridge global CID");

uint genz_loopback = 1;
module_param(genz_loopback, uint, S_IRUGO);
MODULE_PARM_DESC(genz_loopback, "Gen-Z loopback mode (default=1)");

static char *req_page_grid = "4K*20K,2M*12000,1G*2K,8G*1906,4K:1024,2M:1000,1G:5000,128T:3072";
module_param(req_page_grid, charp, 0444);
MODULE_PARM_DESC(req_page_grid, "requester page grid allocations - page_sz{*:}page_cnt[, ...]");

static char *rsp_page_grid = "128T:64,1G:1024,2M:25K,4K:30K";
module_param(rsp_page_grid, charp, 0444);
MODULE_PARM_DESC(rsp_page_grid, "responder page grid allocations - page_sz:page_cnt[, ...]");

uint zhpe_debug_flags;

static bool _expected_saw(const char *callf, uint line,
                          const char *label, uintptr_t expected, uintptr_t saw)
{
    if (expected == saw)
        return true;

    printk(KERN_ERR "%s,%u:%s:%s:expected 0x%lx saw 0x%lx\n",
           callf, line, __FUNCTION__, label, expected, saw);

    return false;
}

#define expected_saw(...) \
    _expected_saw(__FUNCTION__, __LINE__, __VA_ARGS__)

void _do_kfree(const char *callf, uint line, void *ptr)
{
    size_t              size;

    if (!ptr)
        return;

    ptr -= sizeof(void *);
    size = *(uintptr_t *)ptr;
    atomic64_sub(size, &mem_total);
    debug(DEBUG_MEM, "%s:%s,%u:%s:ptr 0x%p size %lu\n",
          zhpe_driver_name, callf, line, __FUNCTION__, ptr, size);
    kfree(ptr);
}

void *_do_kmalloc(const char *callf, uint line,
                  size_t size, gfp_t flags, bool zero)
{
    void                *ret, *ptr;

    /* kmalloc alignment is sizeof(void *) */
    ptr = kmalloc(size + sizeof(void *), flags);
    if (!ptr) {
        if (flags != GFP_ATOMIC)
            printk(KERN_ERR "%s:%s,%d:%s:failed to allocate %lu bytes\n",
                   zhpe_driver_name, callf, line, __FUNCTION__, size);
        return NULL;
    }
    ret = ptr + sizeof(void *);
    if (zero)
        memset(ret, 0, size);
    debug(DEBUG_MEM, "%s:%s,%u:%s:ptr 0x%p ret 0x%p size %lu\n",
          zhpe_driver_name, callf, line, __FUNCTION__, ptr, ret, size);
    atomic64_add(size, &mem_total);
    *(uintptr_t *)ptr = size;

    return ret;
}

void _do_free_pages(const char *callf, uint line, void *ptr, int order)
{
    size_t              size;
    struct page         *page;

    if (!ptr)
        return;

    size = 1UL << (order + PAGE_SHIFT);
    atomic64_sub(size, &mem_total);
    page = virt_to_page(ptr);
    (void)page;
    debug(DEBUG_MEM, "%s:%s,%u:%s:ptr/page/pfn 0x%p/0x%p/0x%lx size %lu\n",
          zhpe_driver_name, callf, line, __FUNCTION__,
          ptr, page, page_to_pfn(page), size);
    free_pages((ulong)ptr, order);
}

void *_do__get_free_pages(const char *callf, uint line,
                          int order, gfp_t flags, bool zero)
{
    void                *ret;
    size_t              size = 1UL << (order + PAGE_SHIFT);
    struct page         *page;

    ret = (void *)__get_free_pages(flags, order);
    if (!ret) {
        if (flags != GFP_ATOMIC)
            printk(KERN_ERR "%s:%s,%u:%s:failed to allocate %lu bytes\n",
                   zhpe_driver_name, callf, line, __FUNCTION__, size);
        return NULL;
    }
    if (zero)
        memset(ret, 0, size);
    atomic64_add(size, &mem_total);
    page = virt_to_page(ret);
    (void)page;
    debug(DEBUG_MEM, "%s:%s,%u:%s:ret/page/pfn 0x%p/0x%p/0x%lx size %lu\n",
          zhpe_driver_name, callf, line, __FUNCTION__,
          ret, page, page_to_pfn(page), size);

    return ret;
}

static inline void _put_io_entry(const char *callf, uint line,
                                 struct io_entry *entry)
{
    int                 count;

    if (entry) {
        count = atomic_dec_return(&entry->count);
        debug(DEBUG_COUNT, "%s:%s,%u:%s:entry 0x%p count %d\n",
              zhpe_driver_name, callf, line, __FUNCTION__, entry, count);
        if (!count && entry->free)
            entry->free(callf, line, entry);
    }
}

#define put_io_entry(...) \
    _put_io_entry(__FUNCTION__, __LINE__, __VA_ARGS__)

static inline struct io_entry *_get_io_entry(const char *callf, uint line,
                                             struct io_entry *entry)
{
    int                 count;

    if (!entry)
        return NULL;

    count = atomic_inc_return(&entry->count);
    /* Override unused variable warning. */
    (void)count;
    debug(DEBUG_COUNT, "%s:%s,%u:%s:entry 0x%p count %d\n",
          zhpe_driver_name, callf, line, __FUNCTION__, entry, count);

    return entry;
}

#define get_io_entry(...) \
    _get_io_entry(__FUNCTION__, __LINE__, __VA_ARGS__)

static void _free_io_lists(const char *callf, uint line,
                           struct file_data *fdata)
{
    struct io_entry     *next;
    struct io_entry     *entry;
    int i = 0;

    debug(DEBUG_RELEASE, "%s:%s,%u:%s:fdata 0x%p\n",
          zhpe_driver_name, callf, line, __FUNCTION__, fdata);

    list_for_each_entry_safe(entry, next, &fdata->rd_list, list) {
        debug(DEBUG_RELEASE, "%s:%s,%u:i %d entry 0x%p idx 0x%04x\n",
              zhpe_driver_name, __FUNCTION__, __LINE__, i, entry,
              entry->op.hdr.index);
        list_del_init(&entry->list);
        put_io_entry(entry);
        i++;
    }
}

#define free_io_lists(...) \
    _free_io_lists(__FUNCTION__, __LINE__, __VA_ARGS__)

static void file_data_free(const char *callf, uint line, void *ptr)
{
    _do_kfree(callf, line, ptr);
}

static inline void _put_file_data(const char *callf, uint line,
                                  struct file_data *fdata)
{
    int                 count;

    if (fdata) {
        count = atomic_dec_return(&fdata->count);
        debug(DEBUG_COUNT, "%s:%s,%u:%s:fdata 0x%p count %d\n",
              zhpe_driver_name, callf, line, __FUNCTION__, fdata, count);
        if (!count && fdata->free)
            fdata->free(callf, line, fdata);
    }
}

#define put_file_data(...) \
    _put_file_data(__FUNCTION__, __LINE__, __VA_ARGS__)

static inline struct file_data *_get_file_data(const char *callf, uint line,
                                               struct file_data *fdata)
{
    int                 count;

    if (!fdata)
        return NULL;

    count = atomic_inc_return(&fdata->count);
    /* Override unused variable warning. */
    (void)count;
    debug(DEBUG_COUNT, "%s:%s,%u:%s:fdata 0x%p count %d\n",
          zhpe_driver_name, callf, line, __FUNCTION__, fdata, count);

    return fdata;
}

#define get_file_data(...) \
    _get_file_data(__FUNCTION__, __LINE__, __VA_ARGS__)

void queue_zpages_free(union zpages *zpages)
{
    size_t              npages;
    size_t              i;
    struct page         *page;

    npages = zpages->queue.size >> PAGE_SHIFT;
    for (i = 0; i < npages; i++) {
        page = virt_to_page(zpages->queue.pages[i]);
        if (page_count(page) != 1 || page_mapcount(page) != 0)
            printk(KERN_WARNING
                   "%s:%s,%u:i %lu ptr/page/pfn 0x%p/0x%p/0x%lx c %d/%d\n",
                   zhpe_driver_name, __FUNCTION__, __LINE__,
                   i, zpages->queue.pages[i],
                   page, page_to_pfn(page), page_count(page),
                   page_mapcount(page));
        do_free_pages(zpages->queue.pages[i], 0);
    }
}

void _zpages_free(const char *callf, uint line, union zpages *zpages)
{
    if (!zpages)
        return;

    debug(DEBUG_MEM, "%s:%s,%u:%s:zpages 0x%p\n",
          zhpe_driver_name, callf, line, __FUNCTION__, zpages);

    /* Revisit: most of these need zap_vma_ptes(vma, addr, size); */
    switch (zpages->hdr.page_type) {
    case QUEUE_PAGE:
    case LOCAL_SHARED_PAGE:
        queue_zpages_free(zpages);
        break;
    case GLOBAL_SHARED_PAGE:
    case HSR_PAGE:
    case RMR_PAGE:
        /* Nothing to do */
        break;
    case DMA_PAGE:
        dma_free_coherent(zpages->dma.dev, zpages->dma.size,
                          zpages->dma.cpu_addr, zpages->dma.dma_addr);
        break;
    }

    do_kfree(zpages);
}

/*
 * hsr_zpages_alloc - allocate a zpages structure for a single page of
 * HSR registers. This is used to map the QCM application data for queues.
 * The space for the HSRs is already allocated and mapped by the pci probe
 * function.
 * 	base_addr - pointer to the start of the QCM app first 64 bytes
 */
union zpages *_hsr_zpage_alloc(
	const char  *callf,
	uint        line,
	phys_addr_t base_addr)
{
    union zpages       *ret = NULL;

    debug(DEBUG_MEM, "%s:%s,%u:%s:page_type HSR_PAGE\n",
          zhpe_driver_name, callf, line, __FUNCTION__);

    /* kmalloc space for the return and an array of pages in the zpage struct */
    ret = do_kmalloc(sizeof(*ret), GFP_KERNEL, true);
    if (!ret)
        goto done;

    ret->hsr.page_type = HSR_PAGE;
    ret->hsr.size = PAGE_SIZE; /* always 1 page of HSRs */
    ret->hsr.base_addr = base_addr;

 done:
    debug(DEBUG_MEM, "%s:%s,%u:%s:ret 0x%p\n",
          zhpe_driver_name, callf, line, __FUNCTION__, ret);

    return ret;
}

/*
 * dma_zpages_alloc - allocate a zpages structure that can be used for the
 * contiguous physical address space for queues. It uses dma_zalloc_coherent()
 * to allocate the space.
 *	sl - slice structure for the pci_dev->device.
	size - size in bytes to be allocated for the dma
 */
union zpages *_dma_zpages_alloc(
	const char *callf, uint line,
	struct slice * sl,
	size_t size)
{
    union zpages       *ret = NULL;
    int                 order = 0;
    size_t              npages;

    debug(DEBUG_MEM, "%s:%s,%u:%s:page_type DMA_PAGE\n",
          zhpe_driver_name, callf, line, __FUNCTION__);

    order = get_order(size);
    npages = 1UL << order;

    /* kmalloc space for the return structure. */
    ret = do_kmalloc(sizeof(*ret), GFP_KERNEL, true);
    if (!ret)
        goto done;

    ret->dma.cpu_addr = dma_zalloc_coherent(&sl->pdev->dev, npages * PAGE_SIZE,
			&ret->dma.dma_addr, GFP_KERNEL);
    debug(DEBUG_MEM, "dma_zalloc_coherent(size = %u, pa returned = 0x%llu, va returned = 0x%p\n", (unsigned int)(npages * PAGE_SIZE), ret->dma.dma_addr, ret->dma.cpu_addr);
    /* RAM memory will always be WB unless you set the memory type. */
    ret->dma.page_type = DMA_PAGE;
    ret->dma.size = size;
    ret->dma.dev = &sl->pdev->dev;
    if (!ret->dma.cpu_addr) {
        do_kfree(ret);
        ret = NULL;
    }

 done:
    debug(DEBUG_MEM, "%s:%s,%u:%s:ret 0x%p\n",
          zhpe_driver_name, callf, line, __FUNCTION__, ret);

    return ret;
}

/*
 * shared_zpage_alloc - allocate a single page for the shared data. It is
 * allocated at init and only free'ed at exit.
 */

union zpages *shared_zpage_alloc(
	size_t size, int type)
{
    union zpages       *ret = NULL;

    /* Use the queue type alloc to get a zpage */
    ret = queue_zpages_alloc(size, false);
    if (!ret)
        return ret;
    /* Mark this page as special SHARED_PAGE type. */
    ret->queue.page_type = type;

    return ret;
}

/*
 * queue_zpages_alloc - allocate a zpages structure that can be used for
 * simple kmalloced queues for early testing without hardware.
 */
union zpages *_queue_zpages_alloc(
	const char *callf, uint line, 
	size_t size,
	bool contig)
{
    union zpages       *ret = NULL;
    int                 order = 0;
    size_t              npages;
    size_t              i;

    debug(DEBUG_MEM, "%s:%s,%u:%s:page_type QUEUE_PAGE size %lu contig %d\n",
          zhpe_driver_name, callf, line, __FUNCTION__, size, contig);

    if  (contig) {
        order = get_order(size);
        npages = 1UL << order;
        size = npages << PAGE_SHIFT;
    } else {
        size = PAGE_ALIGN(size);
        npages = size >> PAGE_SHIFT;
    }

    /* kmalloc space for the return and an array of pages in the zpage struct */
    ret = do_kmalloc(sizeof(*ret) + npages * sizeof(ret->queue.pages[0]),
                     GFP_KERNEL, true);
    if (!ret || !npages)
        goto done;

    ret->queue.size = size;
    ret->queue.page_type = QUEUE_PAGE;
    if (contig) {
	ret->queue.pages[0] = _do__get_free_pages(callf, line,
                                           order, GFP_KERNEL | __GFP_ZERO,
                                           true);
	i = 1;
	if (ret->queue.pages[0]) {
		split_page(virt_to_page(ret->queue.pages[0]), order);
		for (; i < npages; i++)
			ret->queue.pages[i] = ret->queue.pages[i - 1] + PAGE_SIZE;
	}
    } else {
	for (i = 0; i < npages; i++) {
		ret->queue.pages[i] = _do__get_free_pages(callf, line,
                                               0, GFP_KERNEL | __GFP_ZERO,
                                               true);
		if (!ret->queue.pages[i])
			break;
	}
    }
    if (!ret->queue.pages[i-1]) {
        for (i = 0; i < npages; i++)
            do_free_pages(ret->queue.pages[i], 0);
        do_kfree(ret);
        ret = NULL;
    }

 done:
    debug(DEBUG_MEM, "%s:%s,%u:%s:ret 0x%p\n",
          zhpe_driver_name, callf, line, __FUNCTION__, ret);

    return ret;
}

/*
 * rmr_zpages_alloc - allocate a zpages structure for a cpu-visible RMR_IMPORT.
 * This is used to map the requester ZMMU PTE.
 * 	rmr - pointer to the corresponding rmr structure
 */
union zpages *_rmr_zpages_alloc(const char *callf, uint line,
                                struct zhpe_rmr *rmr)
{
    union zpages       *ret = NULL;

    debug(DEBUG_MEM, "%s:%s,%u:%s:page_type RMR_PAGE\n",
          zhpe_driver_name, callf, line, __FUNCTION__);

    /* kmalloc space for the return struct */
    ret = do_kmalloc(sizeof(*ret), GFP_KERNEL, true);
    if (!ret)
        goto done;

    ret->rmrz.page_type = RMR_PAGE;
    ret->rmrz.size = rmr->pte_info.length_adjusted;
    ret->rmrz.rmr = rmr;

 done:
    debug(DEBUG_MEM, "%s:%s,%u:%s:ret 0x%p\n",
          zhpe_driver_name, callf, line, __FUNCTION__, ret);

    return ret;
}

void _zmap_free(const char *callf, uint line, struct zmap *zmap)
{
    if (!zmap)
        return;

    debug(DEBUG_MEM, "%s:%s,%u:%s:zmap 0x%p offset 0x%lx\n",
          zhpe_driver_name, callf, line, __FUNCTION__,
          zmap, zmap->offset);

    if (zmap->zpages)
        zpages_free(zmap->zpages);
    do_kfree(zmap);
}

struct zmap *_zmap_alloc(
	const char *callf,
	uint line,
	struct file_data *fdata,
	union zpages *zpages)
{
    struct zmap         *ret;
    struct zmap         *cur;
    ulong               coff;
    size_t              size;

    debug(DEBUG_MEM, "%s:%s,%u:%s:zpages 0x%p\n",
          zhpe_driver_name, callf, line, __FUNCTION__, zpages);
    ret = _do_kmalloc(callf, line, sizeof(*ret), GFP_KERNEL, true);
    if (!ret) {
        ret = ERR_PTR(-ENOMEM);
        goto done;
    }

    INIT_LIST_HEAD(&ret->list);
    ret->zpages = zpages;
    /* Set bad owner to keep entry from being used until ready. */
    ret->owner = ZMAP_BAD_OWNER;
    /*
     * Look for a hole in betwen entries; allow space for unmapped pages
     * between entries.
     */
    size = zpages->hdr.size + PAGE_SIZE;
    coff = 0;
    spin_lock(&fdata->zmap_lock);
    list_for_each_entry(cur, &fdata->zmap_list, list) {
        if (cur->offset - coff >= size)
            break;
        coff = cur->offset + cur->zpages->hdr.size;
    }
    /*
     * cur will either point to a real entry before which we want to insert
     * ret or &cur->list == head and we want to add ourselves at the tail.
     *
     * Can we wrap around in real life? Probably not.
     */
    if (coff < coff + size) {
        ret->offset = coff;
        if (coff)
            ret->offset += PAGE_SIZE;
        list_add_tail(&ret->list, &cur->list);
    }
    spin_unlock(&fdata->zmap_lock);
    if (list_empty(&ret->list)) {
        _zmap_free(callf, line, ret);
        printk(KERN_ERR "%s:%s,%u:Out of file space.\n",
               zhpe_driver_name, __FUNCTION__, __LINE__);
        ret = ERR_PTR(-ENOSPC);
        goto done;
    }

 done:
    return ret;
}

bool _free_zmap_list(const char *callf, uint line,
                            struct file_data *fdata)
{
    bool                ret = true;
    struct zmap         *zmap;
    struct zmap         *next;

    debug(DEBUG_RELEASE, "%s:%s,%u:%s:fdata 0x%p\n",
          zhpe_driver_name, callf, line, __FUNCTION__, fdata);

    spin_lock(&fdata->zmap_lock);
    list_for_each_entry_safe(zmap, next, &fdata->zmap_list, list) {
        /* global_shared_zmap zpages are not free'ed until exit. */
        if (zmap == fdata->global_shared_zmap) {
            list_del_init(&zmap->list);
            do_kfree(zmap);
        }
        else if (!fdata || zmap->owner == fdata || zmap == fdata->local_shared_zmap) {
            list_del_init(&zmap->list);
            zmap_free(zmap);
        }
    }
    spin_unlock(&fdata->zmap_lock);

    return ret;
}

static inline void queue_io_entry_locked(struct file_data *fdata,
                                         struct list_head *head,
                                         struct io_entry *entry)
{
    bool                wake = list_empty(head);

    list_add_tail(&entry->list, head);
    spin_unlock(&fdata->io_lock);
    wake_up(&fdata->io_wqh);
    if (wake)
        wake_up_all(&poll_wqh);
}

static inline int queue_io_entry(struct file_data *fdata,
                                 struct list_head *head,
                                 struct io_entry *entry)
{
    int                 ret = 0;

    spin_lock(&fdata->io_lock);
    if (fdata->state & STATE_CLOSED) {
        ret = -EIO;
        spin_unlock(&fdata->io_lock);
    } else
        queue_io_entry_locked(fdata, head, entry);

    return ret;
}

static void io_free(const char *callf, uint line, void *ptr)
{
    struct io_entry     *entry = ptr;

    _put_file_data(callf, line, entry->fdata);
    _do_kfree(callf, line, entry);
}

static inline struct io_entry *_io_alloc(
    const char *callf, uint line, size_t size, bool nonblock,
    struct file_data *fdata,
    void (*free)(const char *callf, uint line, void *ptr))
{
    struct io_entry     *ret = NULL;

    if (size < sizeof(ret->op))
        size = sizeof(ret->op);
    size += sizeof(*ret);
    ret = do_kmalloc(size, (nonblock ? GFP_ATOMIC : GFP_KERNEL), false);
    if (!ret)
        goto done;

    ret->free = free;
    atomic_set(&ret->count, 1);
    ret->nonblock = nonblock;
    ret->fdata = get_file_data(fdata);
    INIT_LIST_HEAD(&ret->list);

 done:

    return ret;
}

#define io_alloc(...) \
    _io_alloc(__FUNCTION__, __LINE__, __VA_ARGS__)


int queue_io_rsp(struct io_entry *entry, size_t data_len, int status)
{
    int                 ret = 0;
    struct file_data    *fdata = entry->fdata;
    struct zhpe_common_hdr *op_hdr = &entry->op.hdr;

    op_hdr->version = ZHPE_OP_VERSION;
    op_hdr->opcode = entry->hdr.opcode | ZHPE_OP_RESPONSE;
    op_hdr->index = entry->hdr.index;
    op_hdr->status = status;
    if (!data_len)
        data_len = sizeof(*op_hdr);
    entry->data_len = data_len;

    if (fdata)
        ret = queue_io_entry(fdata, &fdata->rd_list, entry);

    return ret;
}

static int parse_page_grid_one(char *str, uint64_t max_page_count,
                               bool allow_cpu_visible,
                               struct sw_page_grid *pg)
{
    char     *orig = str;
    uint64_t page_size, page_count;

    page_size = memparse(str, &str);
    if (str == orig || !(*str == ':' || *str == '*'))
        goto err;
    if (!is_power_of_2(page_size))
        goto err;
    page_size = ilog2(page_size);
    if (page_size < PAGE_GRID_MIN_PAGESIZE ||
        page_size > PAGE_GRID_MAX_PAGESIZE)
        goto err;
    pg->page_grid.page_size = page_size;

    if (*str == '*' && !allow_cpu_visible)
        goto err;
    pg->cpu_visible = (*str++ == '*') ? true : false;

    orig = str;
    page_count = memparse(str, &str);
    if (str == orig || *str != '\0')
        goto err;
    if (page_count > max_page_count)
        goto err;
    pg->page_grid.page_count = page_count;

    return 0;

 err:
    return -EINVAL;
}

static uint parse_page_grid_opt(char *str, uint64_t max_page_count,
                                bool allow_cpu_visible,
                                struct sw_page_grid pg[])
{
    uint cnt = 0;
    int ret;
    char *str_copy, *s, *k;
    bool bit;
    DECLARE_BITMAP(non_visible_ps_bitmap, PAGE_GRID_MAX_PAGESIZE+1) = { 0 };
    DECLARE_BITMAP(cpu_visible_ps_bitmap, PAGE_GRID_MAX_PAGESIZE+1) = { 0 };

    /* make a writable copy of str */
    str_copy = do_kmalloc(strlen(str) + 1, GFP_KERNEL, false);
    strcpy(str_copy, str);

    for (s = str_copy; s; s = k) {
        k = strchr(s, ',');
        debug(DEBUG_ZMMU, "%s:%s,%u: str=%p,s=%p,k=%p\n",
              zhpe_driver_name, __FUNCTION__, __LINE__,
              str_copy, s, k);

        if (k)
            *k++ = 0;
        debug(DEBUG_ZMMU, "%s:%s,%u: calling parse_page_grid_one(s=%s,max_page_count=%llu, &pg[cnt]=%p)\n",
              zhpe_driver_name, __FUNCTION__, __LINE__,
              s, max_page_count, &pg[cnt]);
        ret = parse_page_grid_one(s, max_page_count, allow_cpu_visible,
                                  &pg[cnt]);
        debug(DEBUG_ZMMU, "%s:%s,%u:ret=%d, page_size=%u, page_count=%u, "
              "cpu_visible=%d\n",
              zhpe_driver_name, __FUNCTION__, __LINE__, ret,
              pg[cnt].page_grid.page_size, pg[cnt].page_grid.page_count,
              pg[cnt].cpu_visible);
        if (pg[cnt].cpu_visible) {
            bit = test_and_set_bit(pg[cnt].page_grid.page_size,
                                   cpu_visible_ps_bitmap);
        } else {
            bit = test_and_set_bit(pg[cnt].page_grid.page_size,
                                   non_visible_ps_bitmap);
        }
        if (!bit && ret == 0)
            cnt++;
        else
            printk(KERN_WARNING "%s:%s:invalid page_grid parameter - %s\n",
                   zhpe_driver_name, __FUNCTION__, s);
        if (cnt == PAGE_GRID_ENTRIES)
            break;
    }

    do_kfree(str_copy);
    return cnt;
}

static int zhpe_user_req_INIT(struct io_entry *entry)
{
    union zhpe_rsp      *rsp = &entry->op.rsp;
    struct file_data    *fdata = entry->fdata;
    struct uuid_tracker *uu;
    uint32_t            ro_rkey, rw_rkey;
    int                 status = 0;
    ulong               flags;
    char                str[UUID_STRING_LEN+1];

    rsp->init.global_shared_offset = fdata->global_shared_zmap->offset;
    rsp->init.global_shared_size =
            fdata->global_shared_zmap->zpages->hdr.size;
    rsp->init.local_shared_offset = fdata->local_shared_zmap->offset;
    rsp->init.local_shared_size =
            fdata->local_shared_zmap->zpages->hdr.size;

    zhpe_generate_uuid(fdata->bridge, &rsp->init.uuid);
    uu = zhpe_uuid_tracker_alloc_and_insert(&rsp->init.uuid, UUID_TYPE_LOCAL,
		0, fdata, GFP_KERNEL, &status);
    if (!uu)
        goto out;

    status = zhpe_rkey_alloc(&ro_rkey, &rw_rkey);
    if (status < 0) {
        zhpe_uuid_remove(uu);
        goto out;
    }

    spin_lock(&fdata->io_lock);
    if (fdata->state & STATE_INIT) {  /* another INIT */
        status = -EBADRQC;
        spin_unlock(&fdata->io_lock);
        zhpe_rkey_free(ro_rkey, rw_rkey);
        zhpe_uuid_remove(uu);
        goto out;
    }
    fdata->state |= STATE_INIT;
    fdata->ro_rkey = ro_rkey;
    fdata->rw_rkey = rw_rkey;
    spin_unlock(&fdata->io_lock);

    spin_lock_irqsave(&fdata->uuid_lock, flags);
    fdata->local_uuid = uu;
    spin_unlock_irqrestore(&fdata->uuid_lock, flags);

 out:
    debug(DEBUG_IO, "%s:%s,%u:ret = %d uuid = %s, ro_rkey=0x%08x, rw_rkey=0x%08x\n",
          zhpe_driver_name, __FUNCTION__, __LINE__, status,
          zhpe_uuid_str(&rsp->init.uuid, str, sizeof(str)), ro_rkey, rw_rkey);
    return queue_io_rsp(entry, sizeof(rsp->init), status);
}

static int zhpe_user_req_QALLOC(struct io_entry *entry)
{
    int                 ret = -EINVAL;
    union zhpe_req      *req = &entry->op.req;
    union zhpe_rsp      *rsp = &entry->op.rsp;
    union zpages        *zpages[3] = { NULL, NULL, NULL };
    struct zmap         *zmaps[3] = { NULL, NULL, NULL };
    size_t              sizes[3];
    uint32_t            qlen;
    size_t              qsize;
    size_t              i;

    CHECK_INIT_STATE(entry, ret, done);
    qlen = req->qalloc.qlen;
    if (qlen < 1 || qlen > global_shared_data->default_attr.max_hw_qlen)
        goto done;
    /* To support qlen entries, we need a real size of qlen + 1 rounded
     * up to the next power of 2.
     */
    rsp->qalloc.info.qlen = 1U << fls(qlen);
    /* Compute sizes; assume a page for registers. */
    rsp->qalloc.info.rsize = PAGE_SIZE;
    sizes[0] = PAGE_SIZE;
    qsize = req->qalloc.qlen * ZHPE_HW_ENTRY_LEN;
    qsize = PAGE_ALIGN(qsize);
    rsp->qalloc.info.qsize = qsize;
    sizes[1] = qsize;
    sizes[2] = qsize;
    /* Allocate zpages and zmaps. */
    ret = -ENOMEM;
    for (i = 0; i < ARRAY_SIZE(sizes); i++) {
        zpages[i] = queue_zpages_alloc(sizes[i], false);
        if (!zpages[i])
            goto done;
        zmaps[i] = zmap_alloc(entry->fdata, zpages[i]);
        if (IS_ERR(zmaps[i])) {
            ret = PTR_ERR(zmaps[i]);
            zmaps[i] = NULL;
            goto done;
        }
    }
    rsp->qalloc.info.reg_off = zmaps[0]->offset;
    rsp->qalloc.info.wq_off = zmaps[1]->offset;
    rsp->qalloc.info.cq_off = zmaps[2]->offset;
    /* Set owner field to valid value; can't fail after this. */
    for (i = 0; i < ARRAY_SIZE(sizes); i++)
        zmaps[i]->owner = entry->fdata;
    /* Make sure owner is seen before we advertise the queue anywhere. */
    smp_wmb();
    ret = 0;

 done:
    if (ret < 0) {
        for (i = 0; i < ARRAY_SIZE(sizes); i++) {
            if (zmaps[i])
                zmap_free(zmaps[i]);
            else if (zpages[i])
                zpages_free(zpages[i]);
        }
    }

    return queue_io_rsp(entry, sizeof(rsp->qalloc), ret);
}

static int zhpe_user_req_QFREE(struct io_entry *entry)
{
    int                 ret = 0;
    struct file_data    *fdata = entry->fdata;
    union zhpe_req      *req = &entry->op.req;
    union zhpe_rsp      *rsp = &entry->op.rsp;
    int                 count = 3;
    struct zmap         *zmap;
    struct zmap         *next;

    CHECK_INIT_STATE(entry, ret, done);
    spin_lock(&fdata->zmap_lock);
    list_for_each_entry_safe(zmap, next, &fdata->zmap_list, list) {
        if (zmap->offset == req->qfree.info.reg_off ||
            zmap->offset == req->qfree.info.wq_off ||
            zmap->offset == req->qfree.info.cq_off) {
            if (zmap->owner != fdata) {
                if (ret >= 0)
                    ret = -EACCES;
            } else {
                list_del_init(&zmap->list);
                zmap_free(zmap);
            }
            if (--count == 0)
                break;
        }
    }
    spin_unlock(&fdata->zmap_lock);
    if (ret >= 0 && count)
        ret = -ENOENT;

 done:
    return queue_io_rsp(entry, sizeof(rsp->qfree), ret);
}

/* This function called by IOMMU driver on PPR failure */
static int iommu_invalid_ppr_cb(struct pci_dev *pdev, int pasid,
            unsigned long address, u16 flags)

{
    printk(KERN_WARNING "%s:%s IOMMU PRR failure device = %s, pasid = %d address = 0x%lx flags = %ux\n",
          zhpe_driver_name, __FUNCTION__, pci_name(pdev), pasid,
          address, flags);

    return AMD_IOMMU_INV_PRI_RSP_INVALID;
}

static int zhpe_bind_iommu(struct file_data *fdata)
{
    int s, ret = 0;
    struct pci_dev *pdev;

    if (!no_iommu) {
        spin_lock(&fdata->io_lock);
        for (s=0; s<SLICES; s++) {
            if (!SLICE_VALID(&(fdata->bridge->slice[s])))
                continue;
            pdev = fdata->bridge->slice[s].pdev;
            ret = amd_iommu_bind_pasid(pdev, fdata->pasid, current);
            if (ret < 0) {
                debug(DEBUG_IO, "amd_iommu_bind_pasid failed for slice %d with return %d\n", s, ret);
            }
            amd_iommu_set_invalid_ppr_cb(pdev, iommu_invalid_ppr_cb);
        }
        spin_unlock(&fdata->io_lock);
    }
    return (ret);
}

static void zhpe_unbind_iommu(struct file_data *fdata)
{
    int s;
    struct pci_dev *pdev;

    spin_lock(&fdata->io_lock);
    if (!no_iommu) {
	for (s=0; s<SLICES; s++) {
            if (!SLICE_VALID(&(fdata->bridge->slice[s])))
                continue;
            pdev = fdata->bridge->slice[s].pdev;
            amd_iommu_unbind_pasid(pdev, fdata->pasid);
            amd_iommu_set_invalid_ppr_cb(pdev, NULL);
        }
    }
    spin_unlock(&fdata->io_lock);
    return;
}

static int zhpe_release(struct inode *inode, struct file *file)
{
    struct file_data    *fdata = file->private_data;
    ulong               flags;

    spin_lock(&fdata->io_lock);
    fdata->state &= ~STATE_INIT;
    fdata->state |= STATE_CLOSED;
    spin_unlock(&fdata->io_lock);
    zhpe_release_owned_xdm_queues(fdata);
    zhpe_release_owned_rdm_queues(fdata);
    free_zmap_list(fdata);
    free_io_lists(fdata);
    zhpe_rmr_free_all(fdata);
    zhpe_notify_remote_uuids(fdata);
    zhpe_umem_free_all(fdata);
    spin_lock_irqsave(&fdata->uuid_lock, flags);
    zhpe_free_remote_uuids(fdata);
    (void)zhpe_free_local_uuid(fdata, true); /* also frees associated R-keys */
    spin_unlock_irqrestore(&fdata->uuid_lock, flags);
    zhpe_unbind_iommu(fdata);
    zhpe_pasid_free(fdata->pasid);
    spin_lock(&fdata->bridge->fdata_lock);
    list_del(&fdata->fdata_list);
    spin_unlock(&fdata->bridge->fdata_lock);
    put_file_data(fdata);

    debug(DEBUG_IO, "%s:%s,%u:ret = %d pid = %d\n",
          zhpe_driver_name, __FUNCTION__, __LINE__, 0, task_pid_vnr(current));

    return 0;
}

static ssize_t zhpe_read(struct file *file, char __user *buf, size_t len,
                         loff_t *ppos)
{
    ssize_t             ret = 0;
    struct file_data    *fdata = file->private_data;
    struct io_entry     *entry;

    if (!len)
        goto done;

    /*
     * Weird semantics: read must be big enough to read entire packet
     * at once; if not, return -EINVAL;
     */
    for (;;) {
        entry = NULL;
        spin_lock(&fdata->io_lock);
        if (!list_empty(&fdata->rd_list)) {
            entry = list_first_entry(&fdata->rd_list, struct io_entry, list);
            if (len >= entry->data_len) {
                list_del_init(&entry->list);
                len = entry->data_len;
            } else {
                debug(DEBUG_IO, "zhpe_read: len %ld entry->data_len %ld\n", len, entry->data_len);
                ret = -EINVAL;
            }
        }
        spin_unlock(&fdata->io_lock);
        if (ret < 0)
            goto done;
        if (entry)
            break;
        if (file->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            goto done;
        }
        ret = wait_event_interruptible(fdata->io_wqh,
                                       !list_empty(&fdata->rd_list));
        if (ret < 0)
            goto done;
    }
    ret = copy_to_user(buf, entry->data, len);
    put_io_entry(entry);

 done:
    debug_cond(DEBUG_IO, (ret /* != -EAGAIN*/),
               "%s:%s,%u:ret = %ld len = %ld pid = %d\n",
               zhpe_driver_name, __FUNCTION__, __LINE__, ret, len,
               task_pid_vnr(current));

    return (ret < 0 ? ret : len);
}

static ssize_t zhpe_write(struct file *file, const char __user *buf,
                          size_t len, loff_t *ppos)
{
    ssize_t             ret = 0;
    struct file_data    *fdata = file->private_data;
    bool                nonblock = !!(file->f_flags & O_NONBLOCK);
    struct io_entry     *entry = NULL;
    struct zhpe_common_hdr *op_hdr;
    size_t              op_len;

    if (!len)
        goto done;

    /*
     * Weird semantics: requires write be a packet containing a single
     * request.
     */
    if (len < sizeof(*op_hdr)) {
        ret = -EINVAL;
        printk(KERN_ERR "%s:%s,%u:Unexpected short write %lu\n",
               zhpe_driver_name, __FUNCTION__, __LINE__, len);
        goto done;
    }

    entry = io_alloc(0, nonblock, fdata, io_free);
    if (!entry) {
        ret = (nonblock ? -EAGAIN : -ENOMEM);
        goto done;
    }
    op_hdr = &entry->op.hdr;

    op_len = sizeof(union zhpe_req);
    if (op_len > len)
        op_len = len;
    ret = copy_from_user(op_hdr, buf, op_len);
    if (ret < 0)
        goto done;
    entry->hdr = *op_hdr;

    ret = -EINVAL;
    if (!expected_saw("version", ZHPE_OP_VERSION, op_hdr->version))
        goto done;

#define USER_REQ_HANDLER(_op)                           \
    case ZHPE_OP_ ## _op:                               \
        debug(DEBUG_IO, "%s:%s:ZHPE_OP_" # _op,         \
              zhpe_driver_name, __FUNCTION__);               \
        op_len = sizeof(struct zhpe_req_ ## _op);       \
        if (len != op_len)                              \
            goto done;                                  \
        ret = zhpe_user_req_ ## _op(entry);             \
        break;

    switch (op_hdr->opcode) {

    USER_REQ_HANDLER(INIT);
    USER_REQ_HANDLER(MR_REG);
    USER_REQ_HANDLER(MR_FREE);
    USER_REQ_HANDLER(QALLOC);
    USER_REQ_HANDLER(QFREE);
    USER_REQ_HANDLER(RMR_IMPORT);
    USER_REQ_HANDLER(RMR_FREE);
    USER_REQ_HANDLER(ZMMU_REG);
    USER_REQ_HANDLER(ZMMU_FREE);
    USER_REQ_HANDLER(UUID_IMPORT);
    USER_REQ_HANDLER(UUID_FREE);
    USER_REQ_HANDLER(XQALLOC);
    USER_REQ_HANDLER(XQFREE);
    USER_REQ_HANDLER(RQALLOC);
    USER_REQ_HANDLER(RQFREE);

    default:
        printk(KERN_ERR "%s:%s,%u:Unexpected opcode 0x%02x\n",
               zhpe_driver_name, __FUNCTION__, __LINE__, op_hdr->opcode);
        ret = -EIO;
        break;
    }

#undef USER_REQ_HANDLER

    /*
     * If handler accepts op, it is no longer our responsibility to free
     * the entry.
     */
    if (ret >= 0)
        entry = NULL;

 done:
    put_io_entry(entry);

    debug_cond(DEBUG_IO, (ret != -EAGAIN),
               "%s:%s,%u:ret = %ld len = %ld pid = %d\n",
               zhpe_driver_name, __FUNCTION__, __LINE__, ret, len,
               task_pid_vnr(current));

    return (ret < 0 ? ret : len);
}

static uint zhpe_poll(struct file *file, struct poll_table_struct *wait)
{
    uint                ret = 0;
    struct file_data    *fdata = file->private_data;

    poll_wait(file, &poll_wqh, wait);
    ret |= (list_empty(&fdata->rd_list) ? 0 : POLLIN | POLLRDNORM);

    return ret;
}

/* Revisit: if vma_set_page_prot was exported by mm/mmap.c, we'd just use
 * it, but it's not, so we do it ourselves here.
 */

#define vma_set_page_prot zhpe_vma_set_page_prot
#define vm_pgprot_modify zhpe_pgprot_modify
#define vma_wants_writenotify zhpe_vma_wants_writenotify

/* Revisit: copy actual vma_wants_writenotify? */
static inline int zhpe_vma_wants_writenotify(struct vm_area_struct *vma,
                                             pgprot_t vm_page_prot)
{
    return 0;
}

/* identical to vm_pgprot_modify, except for function name */
static pgprot_t zhpe_pgprot_modify(pgprot_t oldprot, unsigned long vm_flags)
{
        return pgprot_modify(oldprot, vm_get_page_prot(vm_flags));
}

/* identical to vma_set_page_prot, except for function name */
void zhpe_vma_set_page_prot(struct vm_area_struct *vma)
{
        unsigned long vm_flags = vma->vm_flags;
        pgprot_t vm_page_prot;

        vm_page_prot = vm_pgprot_modify(vma->vm_page_prot, vm_flags);
        if (vma_wants_writenotify(vma, vm_page_prot)) {
                vm_flags &= ~VM_SHARED;
                vm_page_prot = vm_pgprot_modify(vm_page_prot, vm_flags);
        }
        /* remove_protection_ptes reads vma->vm_page_prot without mmap_sem */
        WRITE_ONCE(vma->vm_page_prot, vm_page_prot);
}


static int zhpe_mmap(struct file *file, struct vm_area_struct *vma)
{
    int                 ret = -ENOENT;
    struct file_data    *fdata = file->private_data;
    struct zmap         *zmap;
    union zpages        *zpages;
    struct zhpe_rmr     *rmr;
    ulong               vaddr, offset, length, i, pgoff;
    uint32_t            cache_flags;

    vma->vm_flags |= VM_MIXEDMAP | VM_DONTCOPY;
    vma->vm_private_data = NULL;

    offset = vma->vm_pgoff << PAGE_SHIFT;
    length = vma->vm_end - vma->vm_start;
    debug(DEBUG_MMAP, "%s:%s,%u:vm_start=0x%lx, vm_end=0x%lx, offset=0x%lx\n",
          zhpe_driver_name, __FUNCTION__, __LINE__,
          vma->vm_start, vma->vm_end, offset);
    spin_lock(&fdata->zmap_lock);
    list_for_each_entry(zmap, &fdata->zmap_list, list) {
        if (offset == zmap->offset &&
            length == zmap->zpages->hdr.size) {
            if (!zmap->owner || zmap->owner == fdata)
                ret = 0;
            break;
        }
    }
    spin_unlock(&fdata->zmap_lock);
    if (ret < 0) {
        debug(DEBUG_MMAP, "%s:%s,%u:ret < 0 - zmap not found in zmap_list\n",
                zhpe_driver_name, __FUNCTION__, __LINE__);
        goto done;
    }
    if (!(vma->vm_flags & VM_SHARED)) {
        printk(KERN_ERR "%s:%s,%u:vm_flags !VM_SHARED\n",
                zhpe_driver_name, __FUNCTION__, __LINE__);
        goto done;
    }
    if (vma->vm_flags & VM_EXEC) {
        printk(KERN_ERR "%s:%s,%u:vm_flags VM_EXEC\n",
                zhpe_driver_name, __FUNCTION__, __LINE__);
        goto done;
    }
    vma->vm_flags &= ~VM_MAYEXEC;
    if (zmap == fdata->local_shared_zmap) {
        if (vma->vm_flags & VM_WRITE) {
            printk(KERN_ERR "%s:%s,%u:vm_flags VM_WRITE\n",
                    zhpe_driver_name, __FUNCTION__, __LINE__);
        }
        vma->vm_flags &= ~VM_MAYWRITE;
    }

    zpages = zmap->zpages;
    vma->vm_private_data = zmap;

    switch (zpages->hdr.page_type) {
    case LOCAL_SHARED_PAGE:
    case GLOBAL_SHARED_PAGE:
    case QUEUE_PAGE:
        for (vaddr = vma->vm_start, i = 0; vaddr < vma->vm_end;
             vaddr += PAGE_SIZE, i++) {
            ret = vm_insert_page(vma, vaddr,
                                 virt_to_page(zpages->queue.pages[i]));
            if (ret < 0) {
                printk(KERN_ERR "%s:%s,%u:vm_insert_page() returned %d\n",
                       zhpe_driver_name, __FUNCTION__, __LINE__, ret);
                goto done;
            }
        }
        ret = 0;
        break;
    case DMA_PAGE:
        /* temporarily zero vm_pgoff so dma_mmap_coherent does what we want */
        pgoff = vma->vm_pgoff;
        vma->vm_pgoff = 0;
        ret = dma_mmap_coherent(zpages->dma.dev, vma,
                                zpages->dma.cpu_addr,
                                zpages->dma.dma_addr,
                                length);
        vma->vm_pgoff = pgoff;
        if (ret < 0) {
            printk(KERN_ERR "%s:%s,%u:dma_mmap_coherent() returned %d\n",
                   zhpe_driver_name, __FUNCTION__, __LINE__, ret);
            goto done; /* BUG to break */
        }
        break;
    case HSR_PAGE:
        vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
        ret = io_remap_pfn_range(vma, vma->vm_start,
                                 zpages->hsr.base_addr >> PAGE_SHIFT,
                                 length,
                                 vma->vm_page_prot);
        if (ret) {
            printk(KERN_ERR "%s:%s,%u:HSR io_remap_pfn_range failed\n",
                   zhpe_driver_name, __FUNCTION__, __LINE__);
            goto done;
        }
        break;
    case RMR_PAGE:
        rmr = zpages->rmrz.rmr;
        cache_flags = rmr->pte_info.access & ZHPE_MR_REQ_CPU_CACHE;
        switch (cache_flags) {
            /* ZHPE_MR_REQ_CPU_WB is the default, so nothing to do */
        case ZHPE_MR_REQ_CPU_WC:
            vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
            break;
        case ZHPE_MR_REQ_CPU_WT:
            vma->vm_page_prot = pgprot_writethrough(vma->vm_page_prot);
            break;
        case ZHPE_MR_REQ_CPU_UC:
            vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
            break;
        }
        if (!rmr->writable) {
            vma->vm_flags &= ~(VM_WRITE | VM_MAYWRITE);
            vma_set_page_prot(vma);
        }
        debug(DEBUG_MMAP, "%s:%s,%u:RMR mmap_pfn=0x%lx, vm_page_prot=0x%lx\n",
              zhpe_driver_name, __FUNCTION__, __LINE__,
              zpages->rmrz.rmr->mmap_pfn,
              pgprot_val(vma->vm_page_prot));
        ret = io_remap_pfn_range(vma, vma->vm_start,
                                 zpages->rmrz.rmr->mmap_pfn,
                                 length, vma->vm_page_prot);
        if (ret) {
            printk(KERN_ERR "%s:%s,%u:RMR io_remap_pfn_range returned %d\n",
                   zhpe_driver_name, __FUNCTION__, __LINE__, ret);
            goto done;
        }
        break;
    }

 done:
    if (ret < 0) {
        if (vma->vm_private_data) {
            vma->vm_private_data = NULL;
        }
        printk(KERN_ERR "%s:%s,%u:ret = %d:start 0x%lx end 0x%lx off 0x%lx\n",
               zhpe_driver_name, __FUNCTION__, __LINE__, ret,
               vma->vm_start, vma->vm_end, vma->vm_pgoff);
    }

    return ret;
}

static int zhpe_open(struct inode *inode, struct file *file);

static const struct file_operations zhpe_fops = {
    .owner              =       THIS_MODULE,
    .open               =       zhpe_open,
    .release            =       zhpe_release,
    .read               =       zhpe_read,
    .write              =       zhpe_write,
    .poll               =       zhpe_poll,
    .mmap               =       zhpe_mmap,
    /* Revisit: implement get_unmapped_area to enforce pg_ps alignment */
    .llseek             =       no_llseek,
};

static int alloc_map_shared_data(struct file_data *fdata)
{
    int                 ret = 0;
    int                 i;
    struct zhpe_local_shared_data *local_shared_data;

    fdata->local_shared_zpage =
        shared_zpage_alloc(sizeof(*local_shared_data), LOCAL_SHARED_PAGE);
    if (!fdata->local_shared_zpage) {
        debug(DEBUG_IO, "%s:%s:queue_zpages_alloc failed.\n",
               zhpe_driver_name, __FUNCTION__);
        ret = -ENOMEM;
        goto done;
    }
    /* Add shared data to the zmap_list */
    fdata->local_shared_zmap = zmap_alloc(fdata, fdata->local_shared_zpage);
    if (IS_ERR(fdata->local_shared_zmap)) {
        debug(DEBUG_IO, "%s:%s,%u: zmap_alloc failed\n",
            zhpe_driver_name, __FUNCTION__, __LINE__);
        ret = PTR_ERR(fdata->local_shared_zmap);
        fdata->local_shared_zmap = NULL;
        goto err_zpage_free;
    }
    /* Initialize the counters to 0 */
    local_shared_data = (struct zhpe_local_shared_data *)
			fdata->local_shared_zpage->queue.pages[0];
    local_shared_data->magic = ZHPE_MAGIC;
    local_shared_data->version = ZHPE_LOCAL_SHARED_VERSION;
    for (i = 0; i < MAX_IRQ_VECTORS; i++)
	local_shared_data->handled_counter[i] = 0;

    fdata->local_shared_zmap->owner = NULL;
    smp_wmb();

    /* Map the global shared page for this process's address space. */
    fdata->global_shared_zmap = zmap_alloc(fdata, global_shared_zpage);
    if (IS_ERR(fdata->global_shared_zmap)) {
        debug(DEBUG_IO, "%s:%s,%u: zmap_alloc failed\n",
                zhpe_driver_name, __FUNCTION__, __LINE__);
        ret = PTR_ERR(fdata->global_shared_zmap);
        fdata->global_shared_zmap = NULL;
            goto err_zpage_free;
    }

    fdata->global_shared_zmap->owner = NULL;
    smp_wmb();

    goto done;

err_zpage_free:
    if (fdata->local_shared_zpage) {
        queue_zpages_free(fdata->local_shared_zpage);
        do_kfree(fdata->local_shared_zpage);
    }
done:
    return ret;
}

struct file_data *pid_to_fdata(struct bridge *br, pid_t pid)
{
    struct file_data *cur, *ret = NULL;

    spin_lock(&br->fdata_lock);
    list_for_each_entry(cur, &br->fdata_list, fdata_list) {
        if (cur->pid == pid) {
            ret = cur;
            break;
        }
    }
    spin_unlock(&br->fdata_lock);
    return ret;
}

static int zhpe_open(struct inode *inode, struct file *file)
{
    int                 ret = -ENOMEM;
    struct file_data    *fdata = NULL;
    size_t              size;

    size = sizeof(*fdata);
    fdata = do_kmalloc(size, GFP_KERNEL, true);
    if (!fdata)
        goto done;

    fdata->pid = task_pid_nr(current); /* Associate this fdata with pid */
    fdata->free = file_data_free;
    atomic_set(&fdata->count, 1);
    spin_lock_init(&fdata->io_lock);
    init_waitqueue_head(&fdata->io_wqh);
    INIT_LIST_HEAD(&fdata->rd_list);
    fdata->bridge = &zhpe_bridge;  /* Revisit MultiBridge: support multiple bridges */
    spin_lock_init(&fdata->uuid_lock);
    fdata->local_uuid = NULL;
    fdata->fd_remote_uuid_tree = RB_ROOT;
    spin_lock_init(&fdata->mr_lock);
    fdata->mr_tree = RB_ROOT;
    fdata->fd_rmr_tree = RB_ROOT;
    INIT_LIST_HEAD(&fdata->zmap_list);
    spin_lock_init(&fdata->zmap_lock);
    spin_lock_init(&fdata->xdm_queue_lock);
    /* xdm_queues tracks what queues are owned by this file_data */
    /* Revisit Perf: what is the tradeoff of size of bitmap vs. rbtree? */
    bitmap_zero(fdata->xdm_queues, QUEUES_PER_SLICE*SLICES);
    spin_lock_init(&fdata->rdm_queue_lock);
    bitmap_zero(fdata->rdm_queues, QUEUES_PER_SLICE*SLICES);
    /* we only allow one open per pid */
    if (pid_to_fdata(fdata->bridge, fdata->pid)) {
        ret = -EBUSY;
        goto done;
    }
    /* Allocate and map the local shared data page. Map the global page. */
    ret = alloc_map_shared_data(fdata);
    if (ret != 0) {
        debug(DEBUG_IO, "alloc_map_shared_data:failed with ret = %d\n", ret);
        goto done;
    }
    ret = zhpe_pasid_alloc(&fdata->pasid);
    if (ret < 0)
        goto free_shared_data;
    /* Bind the task to the PASID on the device, if there is an IOMMU. */
    ret = zhpe_bind_iommu(fdata);
    if (ret < 0)
        goto free_pasid;

    /* Add this fdata to the bridge's fdata_list */
    spin_lock(&fdata->bridge->fdata_lock);
    list_add(&fdata->fdata_list, &fdata->bridge->fdata_list);
    spin_unlock(&fdata->bridge->fdata_lock);

    ret = 0;
    goto done;

 free_pasid:
    zhpe_pasid_free(fdata->pasid);

 free_shared_data:
    zmap_free(fdata->local_shared_zmap);
    zmap_free(fdata->global_shared_zmap);

 done:
    if (ret < 0 && fdata) {
        put_file_data(fdata);
        fdata = NULL;
    }
    file->private_data = fdata;

    debug(DEBUG_IO, "%s:%s,%u:ret = %d, pid = %d, pasid = %u\n",
          zhpe_driver_name, __FUNCTION__, __LINE__, ret,
          task_pid_vnr(current), (fdata) ? fdata->pasid : 0);

    return ret;
}

#define ZHPE_ZMMU_XDM_RDM_HSR_BAR 0

struct slice *slice_id_to_slice(struct file_data *fdata, int slice)
{
    struct slice *sl;
    int i;

    for (i = 0; i < SLICES; i++) {
        if (fdata->bridge->slice[i].id == slice)
            return sl;
    }
    return NULL;
}

static int zhpe_probe(struct pci_dev *pdev,
                      const struct pci_device_id *pdev_id)
{
    int ret;
    int l_slice_id;
    void __iomem *base_addr;
    struct bridge *br = &zhpe_bridge;
    struct slice *sl;
    phys_addr_t phys_base;

    /* No setup for function 0 */
    if (PCI_FUNC(pdev->devfn) == 0) {
        return 0;
    }

    /* Zero based slice ID */
    l_slice_id = atomic_inc_return(&slice_id) - 1;
    sl = &br->slice[l_slice_id];

    debug(DEBUG_PCI, "%s:%s device = %s, slice = %u\n",
          zhpe_driver_name, __FUNCTION__, pci_name(pdev), l_slice_id);

    ret = pci_enable_device(pdev);
    if (ret) {
        debug(DEBUG_PCI, "%s:%s:pci_enable_device probe error %d for device %s\n",
               zhpe_driver_name, __FUNCTION__, ret, pci_name(pdev));
        goto err_out;
    }

    if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64))) {
        ret = -ENOSPC;
        dev_warn(&pdev->dev, "%s: No 64-bit DMA available\n",
                 zhpe_driver_name);
        goto err_pci_disable_device;
    }

    ret = pci_request_regions(pdev, DRIVER_NAME);
    if (ret < 0) {
        debug(DEBUG_PCI, "%s:%s:pci_request_regions error %d for device %s\n",
               zhpe_driver_name, __FUNCTION__, ret, pci_name(pdev));
        goto err_pci_disable_device;
    }

    base_addr = pci_iomap(pdev, ZHPE_ZMMU_XDM_RDM_HSR_BAR, sizeof(struct func1_bar0));
    if (!base_addr) {
        debug(DEBUG_PCI, "%s:%s:cannot iomap bar %u registers of size %lu (requested size = %lu)\n",
               zhpe_driver_name, __FUNCTION__, ZHPE_ZMMU_XDM_RDM_HSR_BAR,
               (unsigned long) pci_resource_len(pdev, ZHPE_ZMMU_XDM_RDM_HSR_BAR),
               sizeof(struct func1_bar0));
        ret = -EINVAL;
        goto err_pci_release_regions;
    }
    phys_base = pci_resource_start(pdev, 0);

    debug(DEBUG_PCI, "%s:%s bar = %u, start = 0x%lx, actual len = %lu, requested len = %lu, base_addr = 0x%lx\n",
           zhpe_driver_name, __FUNCTION__, 0,
           (unsigned long) phys_base,
           (unsigned long) pci_resource_len(pdev, 0),
           sizeof(struct func1_bar0),
           (unsigned long) base_addr);

    sl->bar = base_addr;
    sl->phys_base = phys_base;
    sl->id = l_slice_id;
    sl->pdev = pdev;
    sl->valid = true;

    zhpe_zmmu_clear_slice(sl);
    zhpe_zmmu_setup_slice(sl);

    zhpe_xqueue_init(sl);
    if (zhpe_clear_xdm_qcm(sl->bar->xdm)) {
	debug(DEBUG_PCI, "zhpe_clear_xdm_qcm failed\n");
	ret = -1;
	goto err_pci_release_regions;
    }

    zhpe_rqueue_init(sl);
    if (zhpe_clear_rdm_qcm(sl->bar->rdm)) {
	debug(DEBUG_PCI, "zhpe_clear_rdm_qcm failed\n");
	ret = -1;
	goto err_pci_release_regions;
    }

    pci_set_drvdata(pdev, sl);

    /* Initialize this pci_dev with the AMD iommu */
    if (!no_iommu) {
        ret = amd_iommu_init_device(pdev, ZHPE_NUM_PASIDS);
        if (ret < 0) {
            debug(DEBUG_PCI, "amd_iommu_init_device failed with error %d\n", ret);
            goto err_pci_release_regions;
        }
    }

    ret = zhpe_register_interrupts(pdev, sl);
    if (ret) {
        debug(DEBUG_PCI, "zhpe_register_interrupts failed with ret=%d\n", ret);
        goto err_iommu_free;
    }

    if (sl->id == 0) {
        /* allocate driver-driver msg queues on slice 0 only */
        ret = zhpe_msg_qalloc(br);
        if (ret) {
            debug(DEBUG_PCI, "zhpe_msg_qalloc failed with error %d\n", ret);
            goto err_free_interrupts;
        }
    }
    pci_set_master(pdev);
    return 0;

 err_free_interrupts:
    zhpe_free_interrupts(pdev);

 err_iommu_free:
    if (!no_iommu) {
        amd_iommu_free_device(pdev);
    }

err_pci_release_regions:
    pci_release_regions(pdev);

err_pci_disable_device:
    pci_disable_device(pdev);

err_out:
    sl->valid = false;

    return ret;
}

static void zhpe_remove(struct pci_dev *pdev)
{
    struct slice *sl;

    /* No teardown for function 0 */
    if (PCI_FUNC(pdev->devfn) == 0) {
        return;
    }

    sl = (struct slice *) pci_get_drvdata(pdev);

    debug(DEBUG_PCI, "%s:%s device = %s, slice = %u\n",
          zhpe_driver_name, __FUNCTION__, pci_name(pdev), sl->id);

    zhpe_free_interrupts(pdev);
    if (sl->id == 0) {
        zhpe_msg_qfree(BRIDGE_FROM_SLICE(sl));
    }
    pci_clear_master(pdev);

    /* If we are using the IOMMU, free the device */
    if (!no_iommu) {
        amd_iommu_free_device(pdev);
    }

    zhpe_zmmu_clear_slice(sl);
    pci_iounmap(pdev, sl->bar);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
}

static struct miscdevice miscdev = {
    .name               = zhpe_driver_name,
    .fops               = &zhpe_fops,
    .minor              = MISC_DYNAMIC_MINOR,
    .mode               = 0666,
};

static struct pci_driver zhpe_pci_driver = {
    .name      = DRIVER_NAME,
    .id_table  = zhpe_id_table,
    .probe     = zhpe_probe,
    .remove    = zhpe_remove,
};


static int __init zhpe_init(void)
{
    int                 ret = -ENODEV;
    int                 i;
    struct zhpe_attr default_attr = {
        .max_tx_queues      = 1024,
        .max_rx_queues      = 1024,
        .max_hw_qlen        = 65535,
        .max_sw_qlen        = 65535,
        .max_dma_len        = (1U << 31),
    };
    uint                sl, pg, cnt, pg_index;

    if (!strcmp(backend, "zhpe"))
        default_attr.backend = ZHPE_BACKEND_ZHPE;
    else if (!strcmp(backend, "libfabric"))
        default_attr.backend = ZHPE_BACKEND_LIBFABRIC;
    else {
        printk(KERN_WARNING "%s:%s:unrecognized backend = %s\n",
               zhpe_driver_name, __FUNCTION__, backend);
        goto err_out;
    }
    if (default_attr.backend != ZHPE_BACKEND_LIBFABRIC) {
        printk(KERN_WARNING
               "%s:%s:only LIBFABRIC backend supported at this time.\n",
               zhpe_driver_name, __FUNCTION__);
        goto err_out;
    }

    ret = -EINVAL;
    if (!(zhpe_no_avx || boot_cpu_has(X86_FEATURE_AVX))) {
        printk(KERN_WARNING "%s:%s:missing required AVX CPU feature.\n",
               zhpe_driver_name, __FUNCTION__);
        goto err_out;
    }
    ret = -ENOMEM;
    global_shared_zpage = shared_zpage_alloc(sizeof(*global_shared_data), GLOBAL_SHARED_PAGE);
    if (!global_shared_zpage) {
        printk(KERN_WARNING "%s:%s:queue_zpages_alloc failed.\n",
               zhpe_driver_name, __FUNCTION__);
        goto err_out;
    }
    global_shared_data = global_shared_zpage->queue.pages[0];
    global_shared_data->magic = ZHPE_MAGIC;
    global_shared_data->version = ZHPE_GLOBAL_SHARED_VERSION;
    global_shared_data->debug_flags = zhpe_debug_flags;
    global_shared_data->default_attr = default_attr;
    for (i = 0; i < MAX_IRQ_VECTORS; i++)
	global_shared_data->triggered_counter[i] = 0;

    zhpe_bridge.gcid = genz_gcid;
    spin_lock_init(&zhpe_bridge.zmmu_lock);
    for (sl = 0; sl < SLICES; sl++) {
        spin_lock_init(&zhpe_bridge.slice[sl].zmmu_lock);
    }
    spin_lock_init(&zhpe_bridge.fdata_lock);
    INIT_LIST_HEAD(&zhpe_bridge.fdata_list);

    debug(DEBUG_ZMMU, "%s:%s,%u: calling zhpe_zmmu_clear_all\n",
          zhpe_driver_name, __FUNCTION__, __LINE__);
    zhpe_zmmu_clear_all(&zhpe_bridge, false);
    debug(DEBUG_ZMMU, "%s:%s,%u: calling zhpe_pasid_init\n",
          zhpe_driver_name, __FUNCTION__, __LINE__);
    zhpe_pasid_init();
    debug(DEBUG_RKEYS, "%s:%s,%u: calling zhpe_rkey_init\n",
          zhpe_driver_name, __FUNCTION__, __LINE__);
    zhpe_rkey_init();

    debug(DEBUG_ZMMU, "%s:%s,%u: req calling parse_page_grid_opt(%s, %u, %p)\n",
          zhpe_driver_name, __FUNCTION__, __LINE__,
          req_page_grid, REQ_ZMMU_ENTRIES, sw_pg);
    cnt = parse_page_grid_opt(req_page_grid, REQ_ZMMU_ENTRIES, true, sw_pg);
    for (pg = 0; pg < cnt; pg++) {
        pg_index = zhpe_zmmu_req_page_grid_alloc(&zhpe_bridge, &sw_pg[pg]);
    }

    debug(DEBUG_ZMMU, "%s:%s,%u: rsp calling parse_page_grid_opt(%s, %u, %p)\n",
          zhpe_driver_name, __FUNCTION__, __LINE__,
          rsp_page_grid, RSP_ZMMU_ENTRIES, sw_pg);
    cnt = parse_page_grid_opt(rsp_page_grid, RSP_ZMMU_ENTRIES, false, sw_pg);
    for (pg = 0; pg < cnt; pg++) {
        pg_index = zhpe_zmmu_rsp_page_grid_alloc(&zhpe_bridge, &sw_pg[pg]);
    }

    /* Create 128 polling devices for interrupt notification to user space */
    if (zhpe_setup_poll_devs() != 0)
        goto err_zpage_free;

    /* Initiate call to zhpe_probe() for each zhpe PCI function */
    ret = pci_register_driver(&zhpe_pci_driver);
    if (ret < 0) {
        printk(KERN_WARNING "%s:%s:pci_register_driver ret = %d\n",
               zhpe_driver_name, __FUNCTION__, ret);
        goto err_cleanup_poll_devs;
    }

    /* Create device. */
    debug(DEBUG_IO, "%s:%s,%u: creating device\n",
          zhpe_driver_name, __FUNCTION__, __LINE__);
    ret = misc_register(&miscdev);
    if (ret < 0) {
        printk(KERN_WARNING "%s:%s:misc_register() returned %d\n",
               zhpe_driver_name, __FUNCTION__, ret);
        goto err_pci_unregister_driver;
    }

    zhpe_poll_init_waitqueues(&zhpe_bridge);

    return 0;

err_pci_unregister_driver:
    /* Initiate call to zhpe_remove() for each zhpe PCI function */
    pci_unregister_driver(&zhpe_pci_driver);

err_cleanup_poll_devs:
    zhpe_cleanup_poll_devs();

err_zpage_free:
    if (global_shared_zpage) {
        queue_zpages_free(global_shared_zpage);
        do_kfree(global_shared_zpage);
    }

err_out:
    return ret;
}

static void zhpe_exit(void)
{
    if (miscdev.minor != MISC_DYNAMIC_MINOR)
        misc_deregister(&miscdev);

    zhpe_cleanup_poll_devs();

    /* free shared data page. */
    if (global_shared_zpage) {
        queue_zpages_free(global_shared_zpage);
        do_kfree(global_shared_zpage);
    }

    /* Initiate call to zhpe_remove() for each zhpe PCI function */
    pci_unregister_driver(&zhpe_pci_driver);

    zhpe_zmmu_clear_all(&zhpe_bridge, true);
    zhpe_rkey_exit();
    zhpe_pasid_exit();
    zhpe_uuid_exit();

    printk(KERN_INFO "%s:%s mem_total %lld\n",
           zhpe_driver_name, __FUNCTION__, (llong)atomic64_read(&mem_total));
}
