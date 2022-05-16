// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  // 使用一个数组，维护所有物理页面的引用计数
  int refcounts[PHY_COUNT];
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// 释放一个指向pa的引用，当引用数为0时，才执行释放操作
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");
  int count = decrease_ref((uint64)pa);
  if (count > 0) {
    return;
  }

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r) {
    memset((char*)r, 5, PGSIZE); // fill with junk
    kmem.refcounts[(uint64)r / PGSIZE] = 1;
  }
  return (void*)r;
}

// 自增物理地址pa的引用
void
increase_ref(uint64 pa) {
  // printf("increase a ref\n");
  acquire(&kmem.lock);
  kmem.refcounts[pa / PGSIZE] ++;
  release(&kmem.lock);
}

// 自减物理地址pa的引用
int decrease_ref(uint64 pa) {
  // printf("decrease a ref\n");
  acquire(&kmem.lock);
  kmem.refcounts[pa / PGSIZE] --;
  int res = kmem.refcounts[pa / PGSIZE];
  release(&kmem.lock);
  return res;
}

