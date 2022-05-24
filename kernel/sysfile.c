//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}


// 找到一块内存区域，用于执行mmap映射
// 此方案会复用刚被munmap的区域，来解决内存碎片问题
uint64
find_unallocated_mmap_area(uint64 len) {
  struct proc * p = myproc();
  struct vma * v = p->mmaps;
  for (int i = 0; i < N_VMA; i ++) {
    uint64 end_addr = (v[i].addr == 0 ? TRAPFRAME : v[i].addr);
    // 判断[end_addr - len, end_addr]和其余区间是否重叠
    uint64 start_addr = end_addr - len;
    if (start_addr < p->sz) {
      // 到达了堆空间，不可用
      continue;
    }
    int overlap = 0;
    for (int j = 0; j < N_VMA; j ++) {
      uint64 max_start = start_addr > v[j].addr ? start_addr : v[j].addr;
      uint64 min_end = end_addr < v[j].addr + v[j].len ? end_addr : v[j].addr + v[j].len;
      if (max_start < min_end) {
        overlap = 1;
        break;
      }
    }
    if (!overlap) {
      return start_addr;
    }
  }
  return 0;
}

// 将vma结构放入进程的vma数组中
int
push_vma(struct vma v) {
  struct proc * p = myproc();
  for (int i = 0; i < N_VMA; i ++) {
    if (p->mmaps[i].addr == 0) {
      p->mmaps[i] = v;
      return 0;
    }
  }
  return -1;
}


// 这里返回的是映射内存的起始地址
uint64
mmap(uint64 addr, uint64 len, int perm, int flags, int fd, uint64 offset) {
  struct proc* p = myproc();
  if (p->ofile[fd]->writable == 0 && (perm & PROT_WRITE) != 0 && (flags & MAP_SHARED) != 0) {
    // 如果满足 文件只读、mmap可写且共享，那么应该返回错误
    printf("[Kernel] mmap: incorrect perm.\n");
    return -1;
  }

  // 首先找到合适的内存地址
  uint64 start_addr;
  if ((start_addr = find_unallocated_mmap_area(len)) == 0) {
    // 申请不到空闲内存，直接返回
    printf("[Kernel] mmap: fail to allocate memory area.\n");
    return -1;
  }

  // 构造vma
  struct vma v;
  v.addr = start_addr;
  v.len = len;
  v.flag = flags;
  v.perm = perm;
  v.offset = offset;
  v.file_pointer = p->ofile[fd];
  filedup(v.file_pointer);
  // 将新的vma放入结构体中
  if (push_vma(v) == -1) {
    // 放入失败
    fileclose(v.file_pointer);
    printf("[Kernel] mmap: fail to push memory area.\n");
    return -1;
  }
  printf("mmap succ\n");
  return start_addr;
}

uint64
sys_mmap(void) {
  uint64 addr;
  uint64 len;
  int perm, flags;
  int fd;
  uint64 offset;
  if (argaddr(0, &addr) < 0) {
    return -1;
  }
  if (argaddr(1, &len) < 0 || argint(2, &perm) < 0 || argint(3, &flags) < 0 || argint(4, &fd) < 0 || argaddr(5, &offset) < 0) {
    return -1;
  }
  // mmap
  return mmap(addr, len, perm, flags, fd, offset);
}

// 找到addr所在的vma区域
struct vma *
find_vma_area(uint64 addr) {
  struct proc * p = myproc();
  // 在区间内，遍历所有vma，看va处于哪个区间内
  for (int i = 0; i < N_VMA; i ++) {
    struct vma* v = &p->mmaps[i];
    if (v->addr <= addr && addr < v->addr + v->len) {
      return v;
    }
  }
  return 0;
}

uint64
munmap(uint64 addr, uint64 len) {
  struct vma * v;
  if ((v = find_vma_area(addr)) == 0) {
    printf("[Kernel] munmmap: fail to find vma for addr.\n");
    return -1;
  }
  
  uint64 end_addr = addr + len;
  if (addr != v->addr && end_addr != v->addr + v->len) {
    // 不是在边界上释放，会出现洞，返回错误
    printf("[Kernel] munmmap: not implement munmap.\n");
    return -1;
  }

  if (end_addr > v->addr + v->len) {
    // 要释放的区域超出了vma
    printf("[Kernel] munmmap: munmap area exceed end addr.\n");
    return -1;
  }

  struct proc * p = myproc();

  // 如果是Shared，还需要判断是否需要将内容写入文件
  if ((v->flag & MAP_SHARED) != 0) {
    filewrite(v->file_pointer, addr, len);
  }
  // 释放[start, end)之间的内存,更新vma信息
  uvmunmap_munmap(p->pagetable, addr, len / PGSIZE, 1);
  v->addr += len;
  v->len -= len;
  v->offset += len;
  
  // 如果vma的所有内存都释放完毕了，则还需要将vma结构释放掉
  if (0 == v->len) {
    fileclose(v->file_pointer);
    memset(v, 0, sizeof(struct vma));
  }

  return 0;
}

uint64
sys_munmap(void) {
  // munmap(addr, length)
  uint64 addr;
  uint64 len;
  if (argaddr(0, &addr) < 0) {
    return -1;
  }
  if (argaddr(1, &len) < 0) {
    return -1;
  }
  // 执行unmap操作
  return munmap(addr, len);
}