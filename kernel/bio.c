// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"
#include "proc.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  // 哈希桶，为dev, blockno申请或释放buffer时需要对其响应哈希桶加锁

  // 保护桶内的dev, block不会被转化
  struct spinlock buckets[BUFFERS_BUCKETS];
  // 保护桶内的buf不会被重用
  struct spinlock reuse_locks[BUFFERS_BUCKETS];
  struct buf      buffers[BUFFERS_BUCKETS];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;


// 获取dev, blockno对应的桶下标
uint get_index(uint dev, uint blockno) {
  return (dev + blockno) % BUFFERS_BUCKETS;
}

void insert(struct buf *head, struct buf * buf) {
  struct buf * next = head->next;

  buf->next = head->next;
  next->prev = buf;
  head->next = buf;
  buf->prev = head;
}

void
binit(void)
{
  struct buf *b;
  // 首先给每个桶添加NBUF / BUFFERS_BUCKETS个元素
  b = bcache.buf;
  initlock(&bcache.lock, "bcache");
  for (int i = 0; i < BUFFERS_BUCKETS; i ++) {
    struct buf * head = &bcache.buffers[i];
    head->next = head;
    head->prev = head;
    initlock(&bcache.buckets[i], "bcache222");
    for (int j = 0; j < NBUF / BUFFERS_BUCKETS; j ++) {
      // printf("b: %p\n", b);
      insert(head, b);
      initsleeplock(&b->lock, "buffer");
      b ++;
    }
  }

  // 之后给前NBUF % BUFFERS_BUCKETS个桶添加1个元素
  for (int i = 0; i < NBUF % BUFFERS_BUCKETS; i ++) {
    struct buf * head = &bcache.buffers[i];
    insert(head, b);
    initsleeplock(&b->lock, "buffer");
    b ++;
  }
}

struct buf*
get_least_recently_used_buffer_with_no_ref(struct buf * head) {

  struct buf * b;
  for (b = head->prev; b != head; b = b->prev) {
    // printf("%p\n", b);
    if (b->refcnt == 0) {
      return b;
    }
  }
  return 0;
}

// 在head中寻找dev, blockno对应的buffer
struct buf *
get_buffer_for_block(struct buf* head, uint dev, uint blockno) {

  struct buf * b;

  for (b = head->next; b != head; b = b->next) {
    if(b->dev == dev && b->blockno == blockno){
      return b;
    }
  }
  return 0;
}

void evit(struct buf * b) {
  struct buf * pre = b->prev;
  struct buf * nex = b->next;
  pre->next = nex;
  nex->prev = pre;
  b->next = b->prev = 0;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  uint index = get_index(dev, blockno);

  // 对dev, blockno对应桶加锁，避免别的进程对其中的buffer申请或释放
  acquire(&(bcache.buckets[index]));
  if ((b = get_buffer_for_block(&bcache.buffers[index], dev, blockno)) != 0) {
    // 获取到了
    b->refcnt ++;
    release(&(bcache.buckets[index]));
    acquiresleep(&b->lock);
    return b;
  }
  // 没有获取到

  // 找到全局LRU, 同时需要持有LRU对应桶的锁
  struct buf * least_recently_used_buf = 0;
  int least_index = -1;
  for (int i = 0; i < BUFFERS_BUCKETS; i ++) {
    acquire(&bcache.reuse_locks[i]);
    b = get_least_recently_used_buffer_with_no_ref(&bcache.buffers[i]);
    if (b == 0) {
      release(&bcache.reuse_locks[i]);
      continue;
    }
    if (least_index == -1 || b->ticks < bcache.buffers[least_index].ticks) {
      if (least_index != -1) {
        release(&bcache.reuse_locks[least_index]);
      }
      least_index = i;
      least_recently_used_buf = b;
    }else {
      release(&bcache.reuse_locks[i]);
    }
  }
  b = least_recently_used_buf;

  if (b == 0) {
    panic("bget: no buffers");
  }

  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
  evit(b);
  release(&bcache.reuse_locks[least_index]);

  // 将其插入现在的桶
  insert(&bcache.buffers[index], b);
  release(&(bcache.buckets[index]));
  acquiresleep(&b->lock);
  return b;

}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  uint index = get_index(b->dev, b->blockno);
  acquire(&bcache.buckets[index]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.buffers[index].next;
    b->prev = &bcache.buffers[index];
    bcache.buffers[index].next->prev = b;
    bcache.buffers[index].next = b;
    b->ticks = ticks;
  }
  release(&bcache.buckets[index]);
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


