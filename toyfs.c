// toyfs.c
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#define USER_NS_REQUIRED() LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)

#define MAXLEN 8
#define INODE_CNT 2048
#define BLOCK_CNT 2048
#define BLOCKSIZE 512

struct toyfs_block {
  char data[BLOCKSIZE];
};

struct toyfs_block blocks[BLOCK_CNT];

struct toyfs_super_block {
  uint8_t block_map[BLOCK_CNT];
  // uint8_t inode_map[BLOCK_CNT];
} *tf_sb =
    (struct toyfs_super_block *)&blocks[0];  // sizeof(super_block) = 2048

struct toyfs_dentry {
  char filename[MAXLEN];
  uint8_t inode_idx;  // inode number
};

struct toyfs_inode {
  uint8_t busy;
  mode_t mode;
  union {
    uint16_t file_size;
    uint16_t dir_children;
  };
  uint16_t blk_idx_start;
  uint16_t blk_idx_end;
} *inodes = (struct toyfs_inode *)&blocks[1];  // sizeof(toyfs_inode) = 8, 1
                                               // block = 512 = 64 toyfs_inode
// reserve 1 block for super_block, 32 blocks for inodes

// a simple allocator
static int allocate_blocks(struct toyfs_inode *inode) {
  if (inode->blk_idx_end >= BLOCK_CNT) {
    return -1;
  }
  if (inode->blk_idx_end != 0) {
    if (tf_sb->block_map[inode->blk_idx_end / 8] &
        (1 << (inode->blk_idx_end % 8))) {
      return -1;
    } else {
      tf_sb->block_map[inode->blk_idx_end / 8] |=
          (1 << (inode->blk_idx_end % 8));
      inode->blk_idx_end++;
      return 0;
    }
  }

  for (int i = 0; i < BLOCK_CNT; i++) {
    if (tf_sb->block_map[i / 8] & (1 << (i % 8))) {
      continue;
    } else {
      tf_sb->block_map[i / 8] |= (1 << (i % 8));
      inode->blk_idx_start = i;
      inode->blk_idx_end = i + 1;
      return 0;
    }
  }
  return -1;
}

static int free_blocks(struct toyfs_inode *inode) {
  int i;
  for (i = inode->blk_idx_start; i < inode->blk_idx_end; i++) {
    tf_sb->block_map[i / 8] &= ~(1 << (i % 8));
  }
  return 0;
}

static int get_tf_inode(void) {
  for (int i = 0; i < INODE_CNT; i++) {
    if (!inodes[i].busy) {
      inodes[i].busy = 1;
      return i;
    }
  }
  return -1;
}

static struct inode_operations toyfs_inode_ops;

static int toyfs_readdir(struct file *file, struct dir_context *ctx) {
  struct toyfs_inode *tf_inode;
  struct toyfs_dentry *tf_entry;
  int i;

  if (ctx->pos) return 0;

  // tf_inode = (struct toyfs_inode *)file->f_path.dentry->d_inode->i_private;
  tf_inode = &inodes[file->f_path.dentry->d_inode->i_ino];

  if (!S_ISDIR(tf_inode->mode)) {
    return -ENOTDIR;
  }

  tf_entry = (struct toyfs_dentry *)&blocks[tf_inode->blk_idx_start];
  for (i = 0; i < tf_inode->dir_children; i++) {
    if (!dir_emit(ctx, tf_entry[i].filename, MAXLEN, tf_entry[i].inode_idx,
                  DT_UNKNOWN)) {
      return 0;
    }
    ctx->pos += sizeof(struct toyfs_dentry);
  }

  return 0;
}

ssize_t toyfs_read(struct file *file, char __user *buf, size_t len,
                   loff_t *ppos) {
  struct toyfs_inode *tf_inode;
  char *buffer;

  // tf_inode = (struct toyfs_inode *)file->f_path.dentry->d_inode->i_private;
  tf_inode = &inodes[file->f_path.dentry->d_inode->i_ino];

  if (*ppos >= tf_inode->file_size) return 0;

  buffer = (char *)&blocks[tf_inode->blk_idx_start];
  buffer += *ppos;
  len = min((size_t)(tf_inode->file_size - *ppos), len);

  if (copy_to_user(buf, buffer, len)) {
    return -EFAULT;
  }
  *ppos += len;

  return len;
}

ssize_t toyfs_write(struct file *file, const char __user *buf, size_t len,
                    loff_t *ppos) {
  struct toyfs_inode *tf_inode;
  char *buffer;

  // tf_inode = (struct toyfs_inode *)file->f_path.dentry->d_inode->i_private;
  tf_inode = &inodes[file->f_path.dentry->d_inode->i_ino];

  while (*ppos + len >
         BLOCKSIZE * (tf_inode->blk_idx_end - tf_inode->blk_idx_start)) {
    if (allocate_blocks(tf_inode) < 0) {
      return -ENOSPC;
    }
  }

  buffer = (char *)&blocks[tf_inode->blk_idx_start];
  buffer += *ppos;

  if (copy_from_user(buffer, buf, len)) {
    return -EFAULT;
  }
  *ppos += len;
  tf_inode->file_size = *ppos;

  return len;
}

const struct file_operations toyfs_file_operations = {
    .read = toyfs_read,
    .write = toyfs_write,
};

const struct file_operations toyfs_dir_operations = {
    .owner = THIS_MODULE,
    .iterate_shared = toyfs_readdir,
};

static int toyfs_do_create(struct inode *dir, struct dentry *dentry,
                           umode_t mode) {
  struct inode *inode;
  struct super_block *sb;
  struct toyfs_dentry *entry;
  struct toyfs_inode *tf_inode, *p_tf_inode;
  int idx_inode;

  sb = dir->i_sb;

  if (!S_ISDIR(mode) && !S_ISREG(mode)) {
    return -EINVAL;
  }
  if (strlen(dentry->d_name.name) > MAXLEN) {
    return -ENAMETOOLONG;
  }

  inode = new_inode(sb);
  if (!inode) {
    return -ENOMEM;
  }

  inode->i_sb = sb;
  inode->i_op = &toyfs_inode_ops;
  inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

  idx_inode = get_tf_inode();
  if (idx_inode < 0) {
    return -ENOSPC;
  }
  tf_inode = &inodes[idx_inode];

  if (allocate_blocks(tf_inode) < 0) {
    return -ENOSPC;
  }

  inode->i_ino = idx_inode;
  tf_inode->mode = mode;

  if (S_ISDIR(mode)) {
    tf_inode->dir_children = 0;
    inode->i_fop = &toyfs_dir_operations;
  } else if (S_ISREG(mode)) {
    tf_inode->file_size = 0;
    inode->i_fop = &toyfs_file_operations;
  }

  inode->i_private = tf_inode;
  p_tf_inode = (struct toyfs_inode *)dir->i_private;

  if ((p_tf_inode->dir_children + 1) * sizeof(struct toyfs_dentry) >
      BLOCKSIZE * (p_tf_inode->blk_idx_end - p_tf_inode->blk_idx_start)) {
    if (allocate_blocks(p_tf_inode) < 0) {
      return -ENOSPC;
    }
  }

  entry = (struct toyfs_dentry *)&blocks[p_tf_inode->blk_idx_start];
  entry += p_tf_inode->dir_children;
  p_tf_inode->dir_children++;

  entry->inode_idx = idx_inode;
  strcpy(entry->filename, dentry->d_name.name);

  // link inode to VFS list
#if USER_NS_REQUIRED()
  inode_init_owner(&init_user_ns, inode, dir, mode);
#else
  inode_init_owner(inode, dir, mode);
#endif

  d_add(dentry, inode);

  return 0;
}

#if USER_NS_REQUIRED()
static int toyfs_mkdir(struct user_namespace *ns, struct inode *dir,
                       struct dentry *dentry, umode_t mode)
#else
static int toyfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
#endif
{
  return toyfs_do_create(dir, dentry, S_IFDIR | mode);
}

#if USER_NS_REQUIRED()
static int toyfs_create(struct user_namespace *ns, struct inode *dir,
                        struct dentry *dentry, umode_t mode, bool excl)
#else
static int toyfs_create(struct inode *dir, struct dentry *dentry, umode_t mode,
                        bool excl)
#endif
{
  return toyfs_do_create(dir, dentry, mode);
}

static struct inode *toyfs_iget(struct super_block *sb, int idx) {
  struct inode *inode;
  struct toyfs_inode *tf_inode;

  inode = new_inode(sb);
  inode->i_ino = idx;
  inode->i_sb = sb;
  inode->i_op = &toyfs_inode_ops;

  tf_inode = &inodes[idx];

  if (S_ISDIR(tf_inode->mode))
    inode->i_fop = &toyfs_dir_operations;
  else if (S_ISREG(tf_inode->mode))
    inode->i_fop = &toyfs_file_operations;

  inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
  inode->i_private = tf_inode;

  return inode;
}

struct dentry *toyfs_lookup(struct inode *parent_inode,
                            struct dentry *child_dentry, unsigned int flags) {
  struct super_block *sb = parent_inode->i_sb;
  struct toyfs_inode *tf_inode;
  struct toyfs_dentry *entry;

  // tf_inode = (struct toyfs_inode *)parent_inode->i_private;
  tf_inode = &inodes[parent_inode->i_ino];

  entry = (struct toyfs_dentry *)&blocks[tf_inode->blk_idx_start];
  for (int i = 0; i < tf_inode->dir_children; i++) {
    if (!strcmp(entry[i].filename, child_dentry->d_name.name)) {
      struct inode *inode = toyfs_iget(sb, entry[i].inode_idx);
      struct toyfs_inode *inner = (struct toyfs_inode *)inode->i_private;
#if USER_NS_REQUIRED()
      inode_init_owner(&init_user_ns, inode, parent_inode, inner->mode);
#else
      inode_init_owner(inode, parent_inode, inner->mode);
#endif
      d_add(child_dentry, inode);
      return NULL;
    }
  }

  return NULL;
}

int toyfs_rmdir(struct inode *dir, struct dentry *dentry) {
  struct inode *inode = dentry->d_inode;
  struct toyfs_inode *tf_inode = &inodes[inode->i_ino];

  tf_inode->busy = 0;
  free_blocks(tf_inode);
  return simple_rmdir(dir, dentry);
}

int toyfs_unlink(struct inode *dir, struct dentry *dentry) {
  struct inode *inode = dentry->d_inode;
  struct toyfs_inode *tf_inode = &inodes[inode->i_ino];
  struct toyfs_inode *p_tf_inode = &inodes[dir->i_ino];
  struct toyfs_dentry *entry;

  entry = (struct toyfs_dentry *)&blocks[p_tf_inode->blk_idx_start];
  for (int i = 0; i < p_tf_inode->dir_children; i++) {
    if (!strcmp(entry[i].filename, dentry->d_name.name)) {
      for (int j = i; j < p_tf_inode->dir_children - 1; j++) {
        memcpy(&entry[j], &entry[j + 1], sizeof(struct toyfs_dentry));
      }
      p_tf_inode->dir_children--;
      break;
    }
  }
  free_blocks(tf_inode);
  tf_inode->busy = 0;
  return simple_unlink(dir, dentry);
}

static struct inode_operations toyfs_inode_ops = {
    .create = toyfs_create,
    .lookup = toyfs_lookup,
    .mkdir = toyfs_mkdir,
    .rmdir = toyfs_rmdir,
    .unlink = toyfs_unlink,
};

int toyfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct inode *root_inode;
  int mode = S_IFDIR;

  root_inode = new_inode(sb);
  root_inode->i_ino = 0;
#if USER_NS_REQUIRED()
  inode_init_owner(&init_user_ns, root_inode, NULL, mode);
#else
  inode_init_owner(root_inode, NULL, mode);
#endif
  root_inode->i_sb = sb;
  root_inode->i_op = &toyfs_inode_ops;
  root_inode->i_fop = &toyfs_dir_operations;
  root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime =
      current_time(root_inode);

  // super_block & inodes
  tf_sb->block_map[0] = 0xff;
  tf_sb->block_map[1] = 0xff;
  tf_sb->block_map[2] = 0xff;
  tf_sb->block_map[3] = 0xff;
  tf_sb->block_map[4] = 0x01;

  inodes[0].mode = mode;
  inodes[0].dir_children = 0;
  inodes[0].busy = 1;
  root_inode->i_private = &inodes[0];

  if (allocate_blocks(&inodes[0]) < 0) {
    printk(KERN_ERR "toyfs: failed to allocate root inode\n");
    return -1;
  }

  sb->s_root = d_make_root(root_inode);

  return 0;
}

static struct dentry *toyfs_mount(struct file_system_type *fs_type, int flags,
                                  const char *dev_name, void *data) {
  return mount_nodev(fs_type, flags, data, toyfs_fill_super);
}

static void toyfs_kill_superblock(struct super_block *sb) {
  kill_anon_super(sb);
}

struct file_system_type toyfs_fs_type = {
    .owner = THIS_MODULE,
    .name = "toyfs",
    .mount = toyfs_mount,
    .kill_sb = toyfs_kill_superblock,
};

static int toyfs_init(void) {
  int ret;

  memset(blocks, 0, sizeof(blocks));
  memset(inodes, 0, sizeof(inodes));
  ret = register_filesystem(&toyfs_fs_type);
  if (ret)
    printk("register toyfs failed\n");
  else
    printk("register toyfs success\n");

  return ret;
}

static void toyfs_exit(void) {
  unregister_filesystem(&toyfs_fs_type);
  printk("unregister toyfs success\n");
}

module_init(toyfs_init);
module_exit(toyfs_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("a simple file system");
