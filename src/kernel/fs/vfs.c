#include <include/errno.h>
#include <kernel/utils/string.h>
#include <kernel/memory/malloc.h>
#include <kernel/proc/task.h>
#include "vfs.h"
#include "dev.h"
#include "ext2/ext2.h"

static vfs_file_system_type *file_systems;
static vfs_mount *vfsmntlist;

extern process *current_process;

vfs_file_system_type **find_filesystem(const char *name)
{
  vfs_file_system_type **p;
  for (p = &file_systems; *p; p = &(*p)->next)
  {
    if (strcmp((*p)->name, name) == 0)
      break;
  }
  return p;
}

int register_filesystem(vfs_file_system_type *fs)
{
  vfs_file_system_type **p = find_filesystem(fs->name);

  if (*p)
    return -EBUSY;
  else
    *p = fs;

  return 0;
}

int unregister_filesystem(vfs_file_system_type *fs)
{
  vfs_file_system_type **p;
  for (p = &file_systems; *p; p = &(*p)->next)
    if (strcmp((*p)->name, fs->name) == 0)
    {
      *p = (*p)->next;
      return 0;
    }
  return -EINVAL;
}

int find_unused_fd_slot()
{
  for (int i = 0; i < 256; ++i)
    if (!current_process->files->fd[i])
      return i;

  return -EINVAL;
}

vfs_inode *init_inode()
{
  vfs_inode *i = malloc(sizeof(vfs_inode));
  i->i_blocks = 0;
  i->i_size = 0;
  sema_init(&i->i_sem, 1);

  return i;
}

void init_special_inode(vfs_inode *inode, umode_t mode, dev_t dev)
{
  inode->i_mode = mode;
  if (S_ISCHR(mode))
  {
    inode->i_fop = &def_chr_fops;
    inode->i_rdev = dev;
  }
}

vfs_mount *lookup_mnt(vfs_dentry *d)
{
  for (vfs_mount *mnt = vfsmntlist; mnt; mnt = mnt->next)
    if (mnt->mnt_mountpoint == d)
      return mnt;

  return NULL;
}

void init_rootfs(vfs_file_system_type *fs_type, char *dev_name)
{
  vfs_mount *mnt = malloc(sizeof(vfs_mount));
  vfs_superblock *sb = fs_type->mount(fs_type, dev_name, "/");
  mnt->mnt_sb = sb;
  mnt->mnt_mountpoint = mnt->mnt_root = sb->s_root;
  mnt->mnt_devname = dev_name;

  vfsmntlist = mnt;

  current_process->fs->d_root = mnt->mnt_root;
  current_process->fs->mnt_root = mnt;
}

// NOTE: MQ 2019-07-24
// we use device mounted name as identifier fhttps://en.wikibooks.org/wiki/Guide_to_Unix/Explanations/Filesystems_and_Swap#Disk_Partitioning
void vfs_init(vfs_file_system_type *fs, char *dev_name)
{
  init_ext2_fs();
  init_rootfs(fs, dev_name);

  chrdev_init();
}