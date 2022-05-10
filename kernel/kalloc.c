// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  struct cpu *c;
  for (int i = 0; i < NCPU; i ++) {
    c = &cpus[i];
    initlock(&c->lock, "kmem");
  }

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
void
kfree(void *pa)
{
  struct run *r;
  struct cpu *c;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  c = mycpu();

  acquire(&c->lock);
  r->next = c->freelist;
  c->freelist = r;
  release(&c->lock);
}

// 将链表均分为两段，长度为奇数的话前一段更多一些
// 第一段以r开头，第二段以返回值开头
struct run *
split(struct run * r){
  if (!r) {
    return 0;
  }
  struct run * slow = r, *fast = r;
  int i = 0;
  while (fast->next != 0 && fast->next->next != 0) {
    fast = fast->next->next;
    slow = slow->next;
    i ++;
  }
  struct run * head2 = slow->next;
  slow->next = 0;
  return head2;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  struct cpu *c;
  struct cpu *c1;
  c = mycpu();
  acquire(&c->lock);

  // 获取当前cpu的空闲内存列表
  r = c->freelist;

  // 如果当前进程没有空闲内存，就尝试从别的进程偷
  if (!r) {
    // 当前进程已经没有空闲内存了，尝试从其他进程的内存中偷一些过来
    for (int i = 0; i < NCPU; i ++) {
      if (c == &cpus[i]) {
        continue;
      }
      // 判断第i个CPU是否还有空闲内存
      c1 = &cpus[i];
      acquire(&c1->lock);
      // 判断该CPU中是否还有空闲内存
      if (c1->freelist) {
        struct run * head = c1->freelist;
        r = head;
        c1->freelist = split(head);
        c->freelist = head->next;
        // r = head;
        // c1->freelist = r->next;
      }
      release(&c1->lock);
      if (r) {
        break;
      }
    }
  }else {
    c->freelist = r->next;
  }
  
  release(&c->lock);
  if (r) {
    // 将申请到内存填充junk
    memset((char*)r, 5, PGSIZE); // fill with junk
  }
  
  return (void *)r;
}
