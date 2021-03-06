#include <include/errno.h>
#include <include/limits.h>
#include <memory/vmm.h>
#include <proc/task.h>
#include <utils/string.h>

#include "vfs.h"

struct vfs_dentry *alloc_dentry(struct vfs_dentry *parent, char *name)
{
	struct vfs_dentry *d = kcalloc(1, sizeof(struct vfs_dentry));
	d->d_name = strdup(name);
	d->d_parent = parent;
	INIT_LIST_HEAD(&d->d_subdirs);

	if (parent)
		d->d_sb = parent->d_sb;

	return d;
}

int path_walk(struct nameidata *nd, const char *path, int32_t flags, mode_t mode)
{
	nd->dentry = current_process->fs->d_root;
	nd->mnt = current_process->fs->mnt_root;

	char part_name[256] = {0};
	for (int i = 1, length = strlen(path); i < length; ++i)
	{
		memset(part_name, 0, sizeof(part_name));
		for (int j = 0; path[i] != '/' && i < length; ++i, ++j)
			part_name[j] = path[i];

		struct vfs_dentry *iter = NULL;
		struct vfs_dentry *d_child = NULL;
		list_for_each_entry(iter, &nd->dentry->d_subdirs, d_sibling)
		{
			if (!strcmp(part_name, iter->d_name))
			{
				d_child = iter;
				break;
			}
		}

		if (d_child)
		{
			nd->dentry = d_child;
			if (i == length && flags & O_CREAT && flags & O_EXCL)
				return -EEXIST;
		}
		else
		{
			struct vfs_inode *inode = NULL;
			if (nd->dentry->d_inode->i_op->lookup)
				inode = nd->dentry->d_inode->i_op->lookup(nd->dentry->d_inode, part_name);

			if (inode == NULL)
			{
				if (i == length && flags & O_CREAT)
					inode = nd->dentry->d_inode->i_op->create(nd->dentry->d_inode, part_name, i == length ? mode : S_IFDIR);
				else
					return -EACCES;
			}
			else if (i == length && flags & O_CREAT && flags & O_EXCL)
				return -EEXIST;

			d_child = alloc_dentry(nd->dentry, part_name);
			d_child->d_inode = inode;
			list_add_tail(&d_child->d_sibling, &nd->dentry->d_subdirs);
			nd->dentry = d_child;
		}

		struct vfs_mount *mnt = lookup_mnt(nd->dentry);
		if (mnt)
			nd->mnt = mnt;
	};

	return 0;
}

struct vfs_file *get_empty_filp()
{
	struct vfs_file *file = kcalloc(1, sizeof(struct vfs_file));
	file->f_maxcount = INT_MAX;
	atomic_set(&file->f_count, 1);
	return file;
}

int32_t vfs_open(const char *path, int32_t flags)
{
	int ret;
	int fd = find_unused_fd_slot();

	struct nameidata nd;
	ret = path_walk(&nd, path, flags, S_IFREG);
	if (ret < 0)
		return ret;

	struct vfs_file *file = get_empty_filp();
	file->f_dentry = nd.dentry;
	file->f_vfsmnt = nd.mnt;
	file->f_flags = flags;
	file->f_mode = OPEN_FMODE(flags);
	file->f_op = nd.dentry->d_inode->i_fop;

	if (file->f_mode & FMODE_READ)
		file->f_mode |= FMODE_CAN_READ;
	if (file->f_mode & FMODE_WRITE)
		file->f_mode |= FMODE_CAN_WRITE;

	if (file->f_op && file->f_op->open)
	{
		ret = file->f_op->open(nd.dentry->d_inode, file);
		if (ret < 0)
		{
			kfree(file);
			return ret;
		}
	}

	current_process->files->fd[fd] = file;
	return fd;
}

int32_t vfs_close(int32_t fd)
{
	int ret = 0;
	struct files_struct *files = current_process->files;
	acquire_semaphore(&files->lock);

	struct vfs_file *f = files->fd[fd];
	atomic_dec(&f->f_count);
	if (!atomic_read(&f->f_count) && f->f_op->release)
		ret = f->f_op->release(f->f_dentry->d_inode, f);
	files->fd[fd] = NULL;

	release_semaphore(&files->lock);
	return ret;
}

static void generic_fillattr(struct vfs_inode *inode, struct kstat *stat)
{
	stat->st_dev = inode->i_sb->s_dev;
	stat->st_ino = inode->i_ino;
	stat->st_mode = inode->i_mode;
	stat->st_nlink = inode->i_nlink;
	stat->st_uid = inode->i_uid;
	stat->st_gid = inode->i_gid;
	stat->st_rdev = inode->i_rdev;
	stat->st_atim = inode->i_atime;
	stat->st_mtim = inode->i_mtime;
	stat->st_ctim = inode->i_ctime;
	stat->st_size = inode->i_size;
	stat->st_blocks = inode->i_blocks;
	stat->st_blksize = inode->i_blksize;
}

static int do_getattr(struct vfs_mount *mnt, struct vfs_dentry *dentry, struct kstat *stat)
{
	struct vfs_inode *inode = dentry->d_inode;
	if (inode->i_op->getattr)
		return inode->i_op->getattr(mnt, dentry, stat);

	generic_fillattr(inode, stat);
	if (!stat->st_blksize)
	{
		struct vfs_superblock *s = inode->i_sb;
		unsigned blocks;
		blocks = (stat->st_size + s->s_blocksize - 1) >> s->s_blocksize_bits;
		stat->st_blocks = (s->s_blocksize / 512) * blocks;
		stat->st_blksize = s->s_blocksize;
	}
	return 0;
}

int vfs_stat(const char *path, struct kstat *stat)
{
	struct nameidata nd;
	int ret = path_walk(&nd, path, 0, S_IFREG);
	if (ret < 0)
		return ret;

	return do_getattr(nd.mnt, nd.dentry, stat);
}

int vfs_fstat(int32_t fd, struct kstat *stat)
{
	struct vfs_file *f = current_process->files->fd[fd];
	return do_getattr(f->f_vfsmnt, f->f_dentry, stat);
}

int vfs_mknod(const char *path, int mode, dev_t dev)
{
	char *dir, *name;
	strlsplat(path, strliof(path, "/"), &dir, &name);

	struct nameidata nd;
	int ret = path_walk(&nd, dir, 0, S_IFDIR);
	if (ret < 0)
		return ret;

	struct vfs_dentry *d_child = alloc_dentry(nd.dentry, name);
	ret = nd.dentry->d_inode->i_op->mknod(nd.dentry->d_inode, d_child, mode, dev);
	if (ret < 0)
		return ret;

	list_add_tail(&d_child->d_sibling, &nd.dentry->d_subdirs);

	return ret;
}

static int simple_setattr(struct vfs_dentry *d, struct iattr *attrs)
{
	struct vfs_inode *inode = d->d_inode;

	if (attrs->ia_valid & ATTR_SIZE)
		inode->i_size = attrs->ia_size;
	return 0;
}

static int do_truncate(struct vfs_dentry *dentry, int32_t length)
{
	struct vfs_inode *inode = dentry->d_inode;
	struct iattr *attrs = kcalloc(1, sizeof(struct iattr));
	attrs->ia_valid = ATTR_SIZE;
	attrs->ia_size = length;

	if (inode->i_op->setattr)
		return inode->i_op->setattr(dentry, attrs);
	else
		return simple_setattr(dentry, attrs);
}

int vfs_truncate(const char *path, int32_t length)
{
	struct nameidata nd;

	int ret = path_walk(&nd, path, 0, S_IFREG);
	if (ret < 0)
		return ret;

	return do_truncate(nd.dentry, length);
}

int vfs_ftruncate(int32_t fd, int32_t length)
{
	struct vfs_file *f = current_process->files->fd[fd];
	return do_truncate(f->f_dentry, length);
}

int generic_memory_readdir(struct vfs_file *file, struct dirent *dirent, unsigned int count)
{
	struct vfs_dentry *dentry = file->f_dentry;
	int entries_size = 0;

	struct dirent *idrent = dirent;
	struct vfs_dentry *iter;
	list_for_each_entry(iter, &dentry->d_subdirs, d_sibling)
	{
		int len = strlen(iter->d_name);
		int total_len = sizeof(struct dirent) + len + 1;

		if (entries_size + total_len > count)
			break;

		memcpy(idrent->d_name, iter->d_name, len);
		idrent->d_reclen = sizeof(struct dirent) + len + 1;
		idrent->d_ino = iter->d_inode->i_ino;

		entries_size += idrent->d_reclen;
		idrent = (struct dirent *)((char *)idrent + idrent->d_reclen);
	}
	return entries_size;
}

void vfs_build_path_backward(struct vfs_dentry *dentry, char *path)
{
	if (!dentry->d_parent)
		return;

	vfs_build_path_backward(dentry->d_parent, path);
	strcat(path, "/");
	strcat(path, dentry->d_name);
}
