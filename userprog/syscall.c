#include "userprog/syscall.h"

#include <hash.h>
#include <stdio.h>
#include <syscall-nr.h>

#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/loader.h"
#include "threads/thread.h"
#include "userprog/gdt.h"
#include "userprog/process.h"

#define READDIR_MAX_LEN 14

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

void sys_halt(void);
void sys_exit(int status);
pid_t sys_fork(const char *thread_name, struct intr_frame *_if);
int sys_exec(const char *cmd_line);
int sys_wait(pid_t pid);
bool sys_create(const char *file, unsigned initial_size);
bool sys_remove(const char *file);
int sys_open(const char *file);
int sys_filesize(int fd);
int sys_read(int fd, void *buffer, unsigned size);
int sys_write(int fd, const void *buffer, unsigned size);
void sys_seek(int fd, unsigned position);
unsigned sys_tell(int fd);
void sys_close(int fd);
int sys_dup2(int oldfd, int newfd);
bool sys_chdir(const char *dir);
bool sys_mkdir(const char *dir);
bool sys_readdir(int fd, char *name);
bool sys_isdir(int fd);
int sys_inumber(int fd);
int sys_symlink(const char *target, const char *linkpath);
void validate_address(void *addr);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void) {
  write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG)
                                                               << 32);
  write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

  /* The interrupt service rountine should not serve any interrupts
   * until the syscall_entry swaps the userland stack to the kernel
   * mode stack. Therefore, we masked the FLAG_FL. */
  write_msr(MSR_SYSCALL_MASK,
            FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f) {
  int syscall_number = f->R.rax;
  uint64_t arg1 = f->R.rdi;
  uint64_t arg2 = f->R.rsi;
  uint64_t arg3 = f->R.rdx;
  uint64_t arg4 = f->R.r10;
  uint64_t arg5 = f->R.r8;
  uint64_t arg6 = f->R.r9;

  switch (syscall_number) {
    case SYS_HALT:
      sys_halt();
      break;
    case SYS_EXIT:
      sys_exit((int)arg1);
      break;
    case SYS_FORK:
      f->R.rax = (uint64_t)(sys_fork((const char *)arg1, f));
      break;
    case SYS_EXEC:
      f->R.rax = (uint64_t)(sys_exec((const char *)arg1));
      break;
    case SYS_WAIT:
      f->R.rax = (uint64_t)(sys_wait((pid_t)arg1));
      break;
    case SYS_CREATE:
      f->R.rax = (uint64_t)sys_create((const char *)arg1, (unsigned)arg2);
      break;
    case SYS_REMOVE:
      f->R.rax = (uint64_t)sys_remove((const char *)arg1);
      break;
    case SYS_OPEN:
      f->R.rax = (uint64_t)sys_open((const char *)arg1);
      break;
    case SYS_FILESIZE:
      f->R.rax = (uint64_t)sys_filesize((int)arg1);
      break;
    case SYS_READ:
      f->R.rax = (uint64_t)sys_read((int)arg1, (void *)arg2, (unsigned)arg3);
      break;
    case SYS_WRITE:
      f->R.rax =
          (uint64_t)sys_write((int)arg1, (const void *)arg2, (unsigned)arg3);
      break;
    case SYS_SEEK:
      sys_seek((int)arg1, (unsigned)arg2);
      break;
    case SYS_TELL:
      f->R.rax = (uint64_t)sys_tell((int)arg1);
      break;
    case SYS_CLOSE:
      sys_close((int)arg1);
      break;
    case SYS_DUP2:
      f->R.rax = (uint64_t)sys_dup2((int)arg1, (int)arg2);
      break;
    case SYS_CHDIR:
      f->R.rax = (uint64_t)sys_chdir((const char *)arg1);
      break;
    case SYS_MKDIR:
      f->R.rax = (uint64_t)sys_mkdir((const char *)arg1);
      break;
    case SYS_READDIR:
      f->R.rax = (uint64_t)sys_readdir((int)arg1, (char *)arg2);
      break;
    case SYS_ISDIR:
      f->R.rax = (uint64_t)sys_isdir((int)arg1);
      break;
    case SYS_INUMBER:
      f->R.rax = (uint64_t)sys_inumber((int)arg1);
      break;
    case SYS_SYMLINK:
      f->R.rax = (uint64_t)sys_symlink((const char *)arg1, (const char *)arg2);
      break;
    default:
      printf("Not implemented system call\n");
      sys_exit(-1);
      break;
  }
}

void sys_halt(void) { power_off(); }

void sys_exit(int status) {
  struct thread *cur = thread_current();
  cur->proc_desc->exit_status = status;
  thread_exit();
}

pid_t sys_fork(const char *thread_name, struct intr_frame *_if) {
  validate_address((void *)thread_name);
  return process_fork(thread_name, _if);
}

int sys_exec(const char *cmd_line) {
  validate_address((void *)cmd_line);
  if (process_exec((void *)cmd_line) == -1) {
    sys_exit(-1);
  }
  NOT_REACHED();
}

int sys_wait(pid_t pid) { return process_wait((tid_t)pid); }

bool sys_create(const char *file, unsigned initial_size) {
  validate_address((void *)file);
  filesys_lock_acquire();
  bool result = filesys_create(file, initial_size);
  filesys_lock_release();
  return result;
}

bool sys_remove(const char *file) {
  validate_address((void *)file);
  filesys_lock_acquire();
  bool result = filesys_remove(file);
  filesys_lock_release();
  return result;
}

int sys_open(const char *file) {
  validate_address((void *)file);
  filesys_lock_acquire();
  struct file *f = filesys_open(file);
  if (f == NULL) {
    filesys_lock_release();
    return -1;
  }

  struct thread *cur = thread_current();
  int new_fd = cur->proc_desc->next_fd;
  struct file_desc *fd = file_desc_create(new_fd, f);
  if (fd != NULL &&
      file_desc_table_insert(&cur->proc_desc->file_desc_table, fd)) {
    filesys_lock_release();
    cur->proc_desc->next_fd++;
    return new_fd;
  }

  if (fd != NULL) {
    file_desc_destroy(fd);
  }
  filesys_lock_release();
  return -1;
}

int sys_filesize(int fd) {
  struct thread *cur = thread_current();

  filesys_lock_acquire();
  struct file *f =
      file_desc_table_find_file(&cur->proc_desc->file_desc_table, fd);
  if (f == NULL || f == STDIN_FD || f == STDOUT_FD) {
    filesys_lock_release();
    return 0;
  }
  int result = file_length(f);
  filesys_lock_release();

  return result;
}

int sys_read(int fd, void *buffer, unsigned size) {
  validate_address((void *)buffer);
  validate_address((void *)(buffer + size));
  struct thread *cur = thread_current();

  filesys_lock_acquire();
  struct process_desc *pd = cur->proc_desc;
  struct file *f =
      file_desc_table_find_file(&cur->proc_desc->file_desc_table, fd);
  int result = 0;

  if (f == STDIN_FD) {
    if (pd->stdin_count < 1) {
      NOT_REACHED();
    }
    uint8_t *buf = buffer;
    for (unsigned i = 0; i < size; i++) {
      *buf = input_getc();
      buf++;
      result++;
      if ((char)(*buf) == '\0') {
        break;
      }
    }
  } else if (f == NULL || f == STDOUT_FD) {
    result = -1;
  } else {
    result = file_read(f, buffer, size);
  }

  filesys_lock_release();
  return result;
}

int sys_write(int fd, const void *buffer, unsigned size) {
  validate_address((void *)buffer);
  validate_address((void *)(buffer + size));
  struct thread *cur = thread_current();

  filesys_lock_acquire();
  struct process_desc *pd = cur->proc_desc;
  struct file *f =
      file_desc_table_find_file(&cur->proc_desc->file_desc_table, fd);
  int result = 0;

  if (f == STDOUT_FD) {
    if (pd->stdout_count < 1) {
      NOT_REACHED();
    }
    const uint8_t *buf = buffer;
    for (unsigned i = 0; i < size; i++) {
      putchar(*buf);
      buf++;
      result++;
      if ((char)(*buf) == '\0') {
        break;
      }
    }
  } else if (f == NULL || f == STDIN_FD) {
    result = -1;
  } else if (inode_file_type(file_get_inode(f)) != FILETYPE_FILE) {
    result = -1;
  } else {
    result = file_write(f, buffer, size);
  }
  filesys_lock_release();
  return result;
}

void sys_seek(int fd, unsigned position) {
  struct thread *cur = thread_current();

  filesys_lock_acquire();
  struct file *f =
      file_desc_table_find_file(&cur->proc_desc->file_desc_table, fd);
  if (f == NULL || f == STDIN_FD || f == STDOUT_FD) {
    filesys_lock_release();
    return;
  }

  file_seek(f, position);
  filesys_lock_release();
}

unsigned sys_tell(int fd) {
  struct thread *cur = thread_current();

  filesys_lock_acquire();
  struct file *f =
      file_desc_table_find_file(&cur->proc_desc->file_desc_table, fd);
  if (f == NULL || f == STDIN_FD || f == STDOUT_FD) {
    filesys_lock_release();
    return 0;
  }
  unsigned result = file_tell(f);
  filesys_lock_release();

  return result;
}

void sys_close(int fd) {
  struct thread *cur = thread_current();

  filesys_lock_acquire();
  struct process_desc *pd = cur->proc_desc;
  struct file *f =
      file_desc_table_find_file(&cur->proc_desc->file_desc_table, fd);
  if (f == NULL) {
    filesys_lock_release();
    return;
  } else if (f == STDIN_FD) {
    pd->stdin_count--;
  } else if (f == STDOUT_FD) {
    pd->stdout_count--;
  }

  file_desc_table_delete(&cur->proc_desc->file_desc_table, fd);
  filesys_lock_release();
}

int sys_dup2(int oldfd, int newfd) {
  struct thread *cur = thread_current();
  struct process_desc *pd = cur->proc_desc;

  filesys_lock_acquire();
  struct file *f_old = file_desc_table_find_file(&pd->file_desc_table, oldfd);
  struct file *f_new = file_desc_table_find_file(&pd->file_desc_table, newfd);
  if (f_old == NULL) {
    filesys_lock_release();
    return -1;
  }

  if (oldfd == newfd) {
    filesys_lock_release();
    return newfd;
  }

  struct file_desc *new_desc = file_desc_create(newfd, f_old);
  if (new_desc == NULL) {
    filesys_lock_release();
    return -1;
  }

  if (f_old == STDIN_FD) {
    pd->stdin_count++;
  } else if (f_old == STDOUT_FD) {
    pd->stdout_count++;
  } else {
    file_increase_dup_count(f_old);
  }

  if (f_new == STDIN_FD) {
    pd->stdin_count--;
  } else if (f_new == STDOUT_FD) {
    pd->stdout_count--;
  }

  if (f_new != NULL) {
    file_desc_table_delete(&pd->file_desc_table, newfd);
  }

  file_desc_table_insert(&pd->file_desc_table, new_desc);
  if (newfd >= pd->next_fd) {
    pd->next_fd = newfd + 1;
  }

  filesys_lock_release();
  return newfd;
}

bool sys_chdir(const char *dir) {
  validate_address(dir);
  filesys_lock_acquire();
  bool success = filesys_change_dir(dir);
  filesys_lock_release();
  return success;
}

bool sys_mkdir(const char *dir) {
  validate_address(dir);
  filesys_lock_acquire();
  bool success = filesys_create_dir(dir);
  filesys_lock_release();
  return success;
}

bool sys_readdir(int fd, char *name) {
  bool success = false;
  if (name == NULL) {
    return success;
  }
  struct thread *cur = thread_current();
  struct process_desc *pd = cur->proc_desc;

  filesys_lock_acquire();
  struct file *f = file_desc_table_find_file(&pd->file_desc_table, fd);
  if (f == NULL || f == STDIN_FD || f == STDOUT_FD) {
    goto done;
  }

  if (inode_file_type(file_get_inode(f)) != FILETYPE_DIR) {
    goto done;
  }

  struct dir *dir = dir_open(file_get_inode(f));
  if (dir == NULL) {
    goto done;
  }
  char *tmp = (char *)calloc(sizeof(char), READDIR_MAX_LEN + 1);
  if (tmp == NULL) {
    goto done;
  }

  success = dir_readdir(dir, tmp);
  if (success) {
    strlcpy(name, tmp, READDIR_MAX_LEN + 1);
    name[READDIR_MAX_LEN + 1] = '\0';
  }

  free(tmp);
  // dir_close(dir);
  dir_close_wo_inode(dir);

done:
  filesys_lock_release();
  return success;
}

bool sys_isdir(int fd) {
  struct thread *cur = thread_current();
  struct process_desc *pd = cur->proc_desc;
  bool is_dir = false;

  filesys_lock_acquire();
  struct file *f = file_desc_table_find_file(&pd->file_desc_table, fd);
  if (f == NULL || f == STDIN_FD || f == STDOUT_FD) {
    filesys_lock_release();
    return is_dir;
  }

  if (inode_file_type(file_get_inode(f)) == FILETYPE_DIR) {
    is_dir = true;
  }
  filesys_lock_release();

  return is_dir;
}

int sys_inumber(int fd) {
  struct thread *cur = thread_current();
  struct process_desc *pd = cur->proc_desc;
  int inumber = -1;

  filesys_lock_acquire();
  struct file *f = file_desc_table_find_file(&pd->file_desc_table, fd);

  if (f == NULL || f == STDIN_FD || f == STDOUT_FD) {
    filesys_lock_release();
    return inumber;
  }

  inumber = (int)inode_get_inumber(file_get_inode(f));
  filesys_lock_release();
  return inumber;
}

// return zero for success, -1 for failed
int sys_symlink(const char *target, const char *linkpath) {
  validate_address(target);
  validate_address(linkpath);
  filesys_lock_acquire();
  int result = filesys_create_symlink(target, linkpath);
  filesys_lock_release();
  return result;
}

void validate_address(void *addr) {
  if (addr == NULL || !is_user_vaddr(addr)) {
    sys_exit(-1);
  }
}
