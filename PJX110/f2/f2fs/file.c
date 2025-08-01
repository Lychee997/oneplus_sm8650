// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/file.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 */
#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/stat.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/falloc.h>
#include <linux/types.h>
#include <linux/compat.h>
#include <linux/uaccess.h>
#include <linux/mount.h>
#include <linux/pagevec.h>
#include <linux/uio.h>
#include <linux/uuid.h>
#include <linux/file.h>
#include <linux/nls.h>
#include <linux/sched/signal.h>
#include <linux/fileattr.h>
#include <linux/fadvise.h>
#include <linux/iomap.h>
#include <linux/random.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "xattr.h"
#include "acl.h"
#include "gc.h"
#include "iostat.h"
#include <trace/events/f2fs.h>
#include <uapi/linux/f2fs.h>

#ifdef CONFIG_F2FS_APPBOOST
#include <linux/version.h>
#include <linux/delay.h>
#include "../crypto/fscrypt_private.h"
#endif


#ifdef CONFIG_F2FS_FS_DEDUP
#define DEDUP_COMPARE_PAGES	10

#define DEDUP_META_UN_MODIFY_FL		0x1
#define DEDUP_DATA_UN_MODIFY_FL		0x2
#define DEDUP_SET_MODIFY_CHECK		0x4
#define DEDUP_GET_MODIFY_CHECK		0x8
#define DEDUP_CLEAR_MODIFY_CHECK	0x10
#define DEDUP_CLONE_META		0x20
#define DEDUP_CLONE_DATA		0x40
#define DEDUP_SYNC_DATA			0x80
#define DEDUP_FOR_SNAPSHOT		0x100
#define DEDUP_LOOP_MOD			10000
#define DEDUP_MIN_SIZE			65536
#define OUTER_INODE			1
#define INNER_INODE			2
#define NORMAL_INODE			3

struct page_list {
	struct list_head list;
	struct page *page;
};
static struct kmem_cache *page_info_slab;

bool may_compress = false;
bool may_set_compr_fl = false;

#define LOG_PAGE_INTO_LIST(head, page)	do {			\
	struct page_list *tmp;					\
	tmp = f2fs_kmem_cache_alloc_nofail(page_info_slab, GFP_NOFS);	\
	if (tmp) {						\
		tmp->page = page;				\
		INIT_LIST_HEAD(&tmp->list);			\
		list_add_tail(&tmp->list, &head);		\
	}							\
} while (0)

#define FREE_FIRST_PAGE_IN_LIST(head)	do {			\
	struct page_list *tmp;					\
	tmp = list_first_entry(&head, struct page_list, list);	\
	f2fs_put_page(tmp->page, 0);				\
	list_del(&tmp->list);					\
	kmem_cache_free(page_info_slab, tmp);			\
} while (0)

int create_page_info_slab(void)
{
	page_info_slab = f2fs_kmem_cache_create("f2fs_page_info_entry",
				sizeof(struct page_list));
	if (!page_info_slab)
		return -ENOMEM;

	return 0;
}

void destroy_page_info_slab(void)
{
	if (!page_info_slab)
		return;

	kmem_cache_destroy(page_info_slab);
}

/*
 * need lock_op and acquire_orphan by caller
 */
void f2fs_drop_deduped_link(struct inode *inner)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inner);

	f2fs_down_write(&F2FS_I(inner)->i_sem);
	f2fs_i_links_write(inner, false);
	f2fs_up_write(&F2FS_I(inner)->i_sem);

	if (inner->i_nlink == 0)
		f2fs_add_orphan_inode(inner);
	else
		f2fs_release_orphan_inode(sbi);
}

int f2fs_set_inode_addr(struct inode* inode, block_t addr)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct dnode_of_data dn;
	struct page *node_page;
	struct f2fs_inode *ri;
	int count = 1;
	bool need_update;
	block_t blkaddr;
	int i, end_offset;

	if (time_to_inject(sbi, FAULT_DEDUP_FILL_INODE))
		return -EIO;

repeat:
	node_page = f2fs_get_node_page(sbi, inode->i_ino);
	if (PTR_ERR(node_page) == -ENOMEM) {
		if (!(count++ % DEDUP_LOOP_MOD))
			f2fs_err(sbi,
				"%s: try to get node page %d", __func__, count);

		cond_resched();
		goto repeat;
	} else if (IS_ERR(node_page)) {
		f2fs_err(sbi, "%s: get node page fail", __func__);
		return PTR_ERR(node_page);
	}

	f2fs_wait_on_page_writeback(node_page, NODE, true, true);
	ri = F2FS_INODE(node_page);

	end_offset = ADDRS_PER_PAGE(node_page, inode);
	set_new_dnode(&dn, inode, NULL, node_page, inode->i_ino);
	dn.ofs_in_node = 0;

	for (; dn.ofs_in_node < end_offset; dn.ofs_in_node++) {
		blkaddr = data_blkaddr(inode, node_page, dn.ofs_in_node);

		if (__is_valid_data_blkaddr(blkaddr) &&
			f2fs_is_valid_blkaddr(sbi, blkaddr, DATA_GENERIC_ENHANCE))
			f2fs_err(sbi, "%s: inode[%lu] leak data addr[%d:%u]",
				__func__, inode->i_ino, dn.ofs_in_node, blkaddr);
		else {
			__set_data_blkaddr(&dn, addr);
			need_update = true;
		}
	}

	for (i = 0; i < DEF_NIDS_PER_INODE; i++) {
		if (ri->i_nid[i])
			f2fs_err(sbi, "%s: inode[%lu] leak node addr[%d:%u]",
				__func__, inode->i_ino, i, ri->i_nid[i]);
		else {
			ri->i_nid[i] = cpu_to_le32(0);
			need_update = true;
		}
	}

	if (need_update)
		set_page_dirty(node_page);
	f2fs_put_page(node_page, 1);

	return 0;
}

static int __revoke_deduped_inode_begin(struct inode *dedup)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dedup);
	int err;

	f2fs_lock_op(sbi);

	if (time_to_inject(sbi, FAULT_DEDUP_ORPHAN_INODE))
		err = -ENOSPC;
	else
		err = f2fs_acquire_orphan_inode(sbi);	/* for layer inode */
	if (err) {
		f2fs_unlock_op(sbi);
		f2fs_err(sbi, "revoke inode[%lu] begin fail, ret:%d",
			dedup->i_ino, err);
		return err;
	}

	f2fs_add_orphan_inode(dedup);

	set_inode_flag(dedup, FI_REVOKE_DEDUP);

	f2fs_unlock_op(sbi);

	return 0;
}

/*
 * For kernel version < 5.4.0, we depend on inode lock in direct read IO
 */
static void dedup_wait_dio(struct inode *inode)
{
	struct f2fs_inode_info *fi = F2FS_I(inode);

	f2fs_down_write(&fi->i_gc_rwsem[READ]);
	inode_dio_wait(inode);
	f2fs_up_write(&fi->i_gc_rwsem[READ]);
}
static void prepare_free_inner_inode(struct inode *inode, struct inode *inner)
{
	struct f2fs_inode_info *fi = F2FS_I(inner);

	fi->i_flags &= ~F2FS_IMMUTABLE_FL;
	f2fs_set_inode_flags(inner);
	f2fs_mark_inode_dirty_sync(inner, true);

	/*
	 * Before free inner inode, we should wait all reader of
	 * the inner complete to avoid UAF or read unexpected data.
	 */
	wait_event(fi->dedup_wq,
			atomic_read(&fi->inflight_read_io) == 0);

	dedup_wait_dio(inode);
}

static int __revoke_deduped_inode_end(struct inode *dedup)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dedup);
	struct f2fs_inode_info *fi = F2FS_I(dedup);
	struct inode *inner = NULL;
	int err;

	f2fs_lock_op(sbi);

	f2fs_remove_orphan_inode(sbi, dedup->i_ino);

	f2fs_down_write(&fi->i_sem);
	clear_inode_flag(dedup, FI_REVOKE_DEDUP);
	clear_inode_flag(dedup, FI_DEDUPED);
	clear_inode_flag(dedup, FI_META_UN_MODIFY);
	clear_inode_flag(dedup, FI_DATA_UN_MODIFY);

	/*
	 * other reader flow:
	 * 1) lock inode
	 * 2) judge whether inner_inode is NULL
	 * 3) if no, then __iget inner inode
	 */
	inner = fi->inner_inode;
	fi->inner_inode = NULL;
	fi->dedup_cp_ver = cur_cp_version(F2FS_CKPT(sbi));
	f2fs_up_write(&fi->i_sem);

	if (time_to_inject(sbi, FAULT_DEDUP_ORPHAN_INODE))
		err = -ENOSPC;
	else
		err = f2fs_acquire_orphan_inode(sbi);	/* for inner inode */
	if (err) {
		set_sbi_flag(sbi, SBI_NEED_FSCK);	/* delete inner inode */
		f2fs_warn(sbi,
			"%s: orphan failed (ino=%lx), run fsck to fix.",
			__func__, inner->i_ino);
	} else {
		f2fs_drop_deduped_link(inner);
	}
	f2fs_unlock_op(sbi);

	trace_f2fs_dedup_revoke_inode(dedup, inner);

	if (inner->i_nlink == 0)
		prepare_free_inner_inode(dedup, inner);

	iput(inner);
	return err;
}

bool f2fs_is_hole_blkaddr(struct inode *inode, pgoff_t pgofs)
{
	struct dnode_of_data dn;
	block_t blkaddr;
	int err = 0;

	if (time_to_inject(F2FS_I_SB(inode), FAULT_DEDUP_HOLE))
		return true;

	if (f2fs_has_inline_data(inode) ||
		f2fs_has_inline_dentry(inode))
		return false;

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = f2fs_get_dnode_of_data(&dn, pgofs, LOOKUP_NODE);
	if (err && err != -ENOENT)
		return false;

	/* direct node does not exists */
	if (err == -ENOENT)
		return true;

	blkaddr = f2fs_data_blkaddr(&dn);
	f2fs_put_dnode(&dn);

	if (__is_valid_data_blkaddr(blkaddr) &&
		!f2fs_is_valid_blkaddr(F2FS_I_SB(inode),
			blkaddr, DATA_GENERIC))
		return false;

	if (blkaddr != NULL_ADDR)
		return false;

	return true;
}

static int revoke_deduped_blocks(struct inode *dedup, pgoff_t page_idx, int len)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dedup);
	struct address_space *mapping = dedup->i_mapping;
	pgoff_t redirty_idx = page_idx;
	int i, page_len = 0, ret = 0;
	struct dnode_of_data dn;
	filler_t *filler = NULL;
	struct page *page;
	LIST_HEAD(pages);

	DEFINE_READAHEAD(ractl, NULL, NULL, mapping, page_idx);
	page_cache_ra_unbounded(&ractl, len, 0);

	/* readahead pages in file */
	for (i = 0; i < len; i++, page_idx++) {
		if (time_to_inject(sbi, FAULT_DEDUP_REVOKE)) {
			ret = -EIO;
			goto out;
		}
		page = read_cache_page(mapping, page_idx, filler, NULL);
		if (IS_ERR(page)) {
			ret = PTR_ERR(page);
			goto out;
		}
		page_len++;
		LOG_PAGE_INTO_LIST(pages, page);
	}

	/* rewrite pages above */
	for (i = 0; i < page_len; i++, redirty_idx++) {
		if (time_to_inject(sbi, FAULT_DEDUP_REVOKE)) {
			ret = -ENOMEM;
			break;
		}
		page = find_lock_page(mapping, redirty_idx);
		if (!page) {
			ret = -ENOMEM;
			break;
		}

		if (!f2fs_is_hole_blkaddr(F2FS_I(dedup)->inner_inode, redirty_idx)) {
			set_new_dnode(&dn, dedup, NULL, NULL, 0);
			if (time_to_inject(sbi, FAULT_DEDUP_REVOKE))
				ret = -ENOSPC;
			else
				ret = f2fs_get_block_locked(&dn, redirty_idx);
			f2fs_put_dnode(&dn);
			f2fs_bug_on(sbi, !PageUptodate(page));
			if (!ret)
				set_page_dirty(page);
		}

		f2fs_put_page(page, 1);
		if (ret)
			break;
	}

out:
	while (!list_empty(&pages))
		FREE_FIRST_PAGE_IN_LIST(pages);

	return ret;
}

static int __revoke_deduped_data(struct inode *dedup)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dedup);
	pgoff_t page_idx = 0, last_idx;
	int blk_per_seg = sbi->blocks_per_seg;
	int count;
	int ret1 = 0;
	int ret2 = 0;

	f2fs_set_inode_addr(dedup, NULL_ADDR);
	last_idx = DIV_ROUND_UP(i_size_read(dedup), PAGE_SIZE);

	count = last_idx - page_idx;
	while (count) {
		int len = min(blk_per_seg, count);
		ret1 = revoke_deduped_blocks(dedup, page_idx, len);
		if (ret1 < 0)
			break;

		filemap_fdatawrite(dedup->i_mapping);

		count -= len;
		page_idx += len;
	}

	ret2 = f2fs_filemap_write_and_wait_range(dedup);
	if (ret1 || ret2)
		f2fs_warn(sbi, "%s: The deduped inode[%lu] revoked fail(errno=%d,%d).",
				__func__, dedup->i_ino, ret1, ret2);

	return ret1 ? : ret2;
}

static void _revoke_error_handle(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	f2fs_lock_op(sbi);
	f2fs_truncate_dedup_inode(inode, FI_REVOKE_DEDUP);
	f2fs_remove_orphan_inode(sbi, inode->i_ino);
	F2FS_I(inode)->dedup_cp_ver = cur_cp_version(F2FS_CKPT(sbi));
	f2fs_unlock_op(sbi);
	trace_f2fs_dedup_revoke_fail(inode, F2FS_I(inode)->inner_inode);
}

/*
 * need inode_lock by caller
 */
int f2fs_revoke_deduped_inode(struct inode *dedup, const char *revoke_source)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dedup);
	int err = 0;
	struct inode *inner_inode = NULL;
	nid_t inner_ino = 0;

	if (unlikely(f2fs_cp_error(sbi)))
		return -EIO;

	if (!f2fs_is_outer_inode(dedup))
		return -EINVAL;

	if (is_inode_flag_set(dedup, FI_SNAPSHOTED))
		return -EOPNOTSUPP;

	if (is_inode_flag_set(dedup, FI_SNAPSHOT_PREPARED))
		return 0;

	err = f2fs_dquot_initialize(dedup);
	if (err)
		return err;

	f2fs_balance_fs(sbi, true);

	inner_inode = F2FS_I(dedup)->inner_inode;
	if (inner_inode)
		inner_ino = inner_inode->i_ino;

	err = __revoke_deduped_inode_begin(dedup);
	if (err)
		goto ret;

	err = __revoke_deduped_data(dedup);
	if (err) {
		_revoke_error_handle(dedup);
		goto ret;
	}

	err = __revoke_deduped_inode_end(dedup);

ret:
	return err;
}

static void f2fs_set_modify_check(struct inode *inode,
		struct f2fs_modify_check_info *info)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	if (info->flag & DEDUP_META_UN_MODIFY_FL) {
		if (is_inode_flag_set(inode, FI_META_UN_MODIFY))
			f2fs_err(sbi,
				"inode[%lu] had set meta unmodified flag",
				inode->i_ino);
		else
			set_inode_flag(inode, FI_META_UN_MODIFY);
	}

	if (info->flag & DEDUP_DATA_UN_MODIFY_FL) {
		if (is_inode_flag_set(inode, FI_DATA_UN_MODIFY))
			f2fs_err(sbi,
				"inode[%lu] had set data unmodified flag",
				inode->i_ino);
		else
			set_inode_flag(inode, FI_DATA_UN_MODIFY);
	}
}

static void f2fs_get_modify_check(struct inode *inode,
		struct f2fs_modify_check_info *info)
{
	memset(&(info->flag), 0, sizeof(info->flag));

	if (is_inode_flag_set(inode, FI_META_UN_MODIFY))
		info->flag = info->flag | DEDUP_META_UN_MODIFY_FL;

	if (is_inode_flag_set(inode, FI_DATA_UN_MODIFY))
		info->flag = info->flag | DEDUP_DATA_UN_MODIFY_FL;
}

static void f2fs_clear_modify_check(struct inode *inode,
		struct f2fs_modify_check_info *info)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	if (info->flag & DEDUP_META_UN_MODIFY_FL) {
		if (!is_inode_flag_set(inode, FI_META_UN_MODIFY)) {
			f2fs_err(sbi,
				"inode[%lu] had clear unmodified meta flag",
				inode->i_ino);
		}

		clear_inode_flag(inode, FI_META_UN_MODIFY);
	}

	if (info->flag & DEDUP_DATA_UN_MODIFY_FL) {
		if (!is_inode_flag_set(inode, FI_DATA_UN_MODIFY)) {
			f2fs_err(sbi,
				"inode[%lu] had clear unmodified data flag",
				inode->i_ino);
		}

		clear_inode_flag(inode, FI_DATA_UN_MODIFY);
	}

	f2fs_mark_inode_dirty_sync(inode, true);
}

bool f2fs_inode_support_dedup(struct f2fs_sb_info *sbi,
		struct inode *inode)
{
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct f2fs_inode *ri;

	if (!f2fs_sb_has_dedup(sbi))
		return false;

	if (!f2fs_has_extra_attr(inode))
		return false;

	if (!F2FS_FITS_IN_INODE(ri, fi->i_extra_isize, i_inner_ino))
		return false;

	if (f2fs_compressed_file(inode))
		return false;

	return true;
}

static int f2fs_inode_param_check(struct f2fs_sb_info *sbi,
		struct inode *inode, int type)
{
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	if (type == OUTER_INODE &&
		!is_inode_flag_set(inode, FI_SNAPSHOTED) &&
		!is_inode_flag_set(inode, FI_SNAPSHOT_PREPARED) &&
					inode->i_size < DEDUP_MIN_SIZE) {
		f2fs_err(sbi, "dedup fails, inode[%lu] size < %d bytes.",
			inode->i_ino, DEDUP_MIN_SIZE);
		return -EINVAL;
	}

	if (type == OUTER_INODE &&
		!is_inode_flag_set(inode, FI_DATA_UN_MODIFY)) {
		f2fs_err(sbi, "dedup fails, inode[%lu] has been modified.",
			inode->i_ino);
		return -EINVAL;
	}

	if (IS_VERITY(inode)) {
		f2fs_err(sbi, "dedup fails, inode[%lu] enable verity.",
			inode->i_ino);
		return -EACCES;
	}

	if (f2fs_is_atomic_file(inode)) {
		f2fs_err(sbi, "dedup fails, inode[%lu] is atomic file.",
			inode->i_ino);
		return -EACCES;
	}

	if (f2fs_is_pinned_file(inode)) {
		f2fs_err(sbi, "dedup fails, inode[%lu] is pinned file.",
			inode->i_ino);
		return -EACCES;
	}

	if (type != INNER_INODE && IS_IMMUTABLE(inode)) {
		f2fs_err(sbi, "dedup fails, inode[%lu] is immutable.",
			inode->i_ino);
		return -EACCES;
	}
	return 0;
}

static int f2fs_dedup_param_check(struct f2fs_sb_info *sbi,
		struct inode *inode1, int type1,
		struct inode *inode2, int type2)
{
	int ret;

	if (time_to_inject(sbi, FAULT_DEDUP_PARAM_CHECK))
		return -EINVAL;

	if (inode1->i_sb != inode2->i_sb || inode1 == inode2) {
		f2fs_err(sbi, "%s: input inode[%lu] and [%lu] are illegal.",
			__func__, inode1->i_ino, inode2->i_ino);
		return -EINVAL;
	}

	if (type1 == OUTER_INODE && type2 == OUTER_INODE &&
		!is_inode_flag_set(inode2, FI_SNAPSHOTED) &&
		inode1->i_size != inode2->i_size) {
		f2fs_err(sbi,
			"dedup file size not match inode1[%lu] %lld, inode2[%lu] %lld",
			inode1->i_ino, inode1->i_size,
			inode2->i_ino, inode2->i_size);
		return -EINVAL;
	}

	ret = f2fs_inode_param_check(sbi, inode1, type1);
	if (ret)
		return ret;

	ret = f2fs_inode_param_check(sbi, inode2, type2);
	if (ret)
		return ret;

	return 0;
}

static int f2fs_ioc_modify_check(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct user_namespace *mnt_userns = file_mnt_user_ns(filp);
	struct f2fs_modify_check_info info;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	int ret;

	if (unlikely(f2fs_cp_error(sbi)))
		return -EIO;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	if (!inode_owner_or_capable(mnt_userns, inode))
		return -EACCES;

	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	if (!f2fs_inode_support_dedup(sbi, inode))
		return -EOPNOTSUPP;

	if (f2fs_has_inline_data(inode))
		return -EINVAL;

	if (copy_from_user(&info,
		(struct f2fs_modify_check_info __user *)arg, sizeof(info)))
		return -EFAULT;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	inode_lock(inode);
	if (info.mode & DEDUP_SET_MODIFY_CHECK) {
		struct address_space *mapping = inode->i_mapping;
		bool dirty = false;
		int nrpages = 0;

		if (mapping_mapped(mapping)) {
			f2fs_err(sbi, "inode[%lu] has mapped vma", inode->i_ino);
			ret = -EBUSY;
			goto out;
		}

		ret = f2fs_inode_param_check(sbi, inode, NORMAL_INODE);
		if (ret)
			goto out;

		if (mapping_tagged(mapping, PAGECACHE_TAG_DIRTY) ||
				mapping_tagged(mapping, PAGECACHE_TAG_WRITEBACK)) {
			dirty = true;
			nrpages = get_dirty_pages(inode);
		}

		if (dirty && (info.flag & DEDUP_SYNC_DATA)) {
			ret = f2fs_filemap_write_and_wait_range(inode);
			if (ret) {
				f2fs_err(sbi, "inode[%lu] write data fail(%d)\n",
						inode->i_ino, ret);
				goto out;
			}
		} else if (dirty) {
			f2fs_err(sbi, "inode[%lu] have dirty page[%d]\n",
					inode->i_ino, nrpages);
			ret = -EINVAL;
			goto out;
		}

		f2fs_set_modify_check(inode, &info);
	} else if (info.mode & DEDUP_GET_MODIFY_CHECK) {
		f2fs_get_modify_check(inode, &info);
	} else if (info.mode & DEDUP_CLEAR_MODIFY_CHECK) {
		f2fs_clear_modify_check(inode, &info);
	} else {
		ret = -EINVAL;
	}

out:
	inode_unlock(inode);
	mnt_drop_write_file(filp);

	if (copy_to_user((struct f2fs_modify_check_info __user *)arg,
		&info, sizeof(info)))
		ret = -EFAULT;

	return ret;
}

static int f2fs_ioc_dedup_permission_check(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct user_namespace *mnt_userns = file_mnt_user_ns(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	if (unlikely(f2fs_cp_error(sbi)))
		return -EIO;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	if (!inode_owner_or_capable(mnt_userns, inode))
		return -EACCES;

	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	if (!f2fs_inode_support_dedup(sbi, inode))
		return -EOPNOTSUPP;

	if (f2fs_has_inline_data(inode))
		return -EINVAL;

	return f2fs_inode_param_check(sbi, inode, OUTER_INODE);
}

static int f2fs_copy_data(struct inode *dst_inode,
		struct inode *src_inode, pgoff_t page_idx, int len)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dst_inode);
	struct address_space *src_mapping = src_inode->i_mapping;
	struct address_space *dst_mapping = dst_inode->i_mapping;
	filler_t *filler = NULL;
	struct page *page, *newpage;
	pgoff_t copy_idx = page_idx;
	int i, page_len = 0, ret = 0;
	struct dnode_of_data dn;
	DEFINE_READAHEAD(ractl, NULL, NULL, src_mapping, page_idx);
	LIST_HEAD(pages);

	page_cache_ra_unbounded(&ractl, len, 0);

	for (i = 0; i < len; i++, page_idx++) {
		if (time_to_inject(sbi, FAULT_DEDUP_CLONE)) {
			ret = -ENOMEM;
			break;
		}
		page = read_cache_page(src_mapping, page_idx, filler, NULL);
		if (IS_ERR(page)) {
			ret = PTR_ERR(page);
			goto out;
		}
		page_len++;
		LOG_PAGE_INTO_LIST(pages, page);
	}

	for (i = 0; i < page_len; i++, copy_idx++) {
		if (time_to_inject(sbi, FAULT_DEDUP_CLONE)) {
			ret = -ENOMEM;
			break;
		}
		page = find_lock_page(src_mapping, copy_idx);
		if (!page) {
			ret = -ENOMEM;
			break;
		}

		if (f2fs_is_hole_blkaddr(src_inode, copy_idx)) {
			f2fs_put_page(page, 1);
			continue;
		}

		set_new_dnode(&dn, dst_inode, NULL, NULL, 0);
		if (time_to_inject(sbi, FAULT_DEDUP_CLONE))
			ret = -ENOSPC;
		else
			ret = f2fs_get_block_locked(&dn, copy_idx);
		f2fs_put_dnode(&dn);
		if (ret) {
			f2fs_put_page(page, 1);
			break;
		}

		if (time_to_inject(sbi, FAULT_DEDUP_CLONE))
			newpage = NULL;
		else
			newpage = f2fs_grab_cache_page(dst_mapping, copy_idx, true);
		if (!newpage) {
			ret = -ENOMEM;
			f2fs_put_page(page, 1);
			break;
		}
		memcpy_page(newpage, 0, page, 0, PAGE_SIZE);

		set_page_dirty(newpage);
		f2fs_put_page(newpage, 1);
		f2fs_put_page(page, 1);
	}

out:
	while (!list_empty(&pages))
		FREE_FIRST_PAGE_IN_LIST(pages);

	return ret;
}

static int f2fs_clone_data(struct inode *dst_inode,
		struct inode *src_inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(src_inode);
	pgoff_t page_idx = 0, last_idx;
	int blk_per_seg = sbi->blocks_per_seg;
	int count;
	int ret = 0;

	f2fs_balance_fs(sbi, true);
	last_idx = DIV_ROUND_UP(i_size_read(src_inode), PAGE_SIZE);
	count = last_idx - page_idx;

	while (count) {
		int len = min(blk_per_seg, count);
		ret = f2fs_copy_data(dst_inode, src_inode, page_idx, len);
		if (ret < 0)
			break;

		filemap_fdatawrite(dst_inode->i_mapping);
		count -= len;
		page_idx += len;
	}

	if (!ret)
		ret = f2fs_filemap_write_and_wait_range(dst_inode);

	return ret;
}

static int __f2fs_ioc_clone_file(struct inode *dst_inode,
		struct inode *src_inode, struct f2fs_clone_info *info)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(src_inode);
	int ret;

	ret = f2fs_convert_inline_inode(dst_inode);
	if (ret) {
		f2fs_err(sbi,
			"inode[%lu] convert inline inode failed, ret:%d",
			dst_inode->i_ino, ret);
		return ret;
	}

	if (info->flags & DEDUP_CLONE_META) {
		dst_inode->i_uid = src_inode->i_uid;
		dst_inode->i_gid = src_inode->i_gid;
		dst_inode->i_size = src_inode->i_size;
	}

	if (info->flags & DEDUP_CLONE_DATA) {
		dst_inode->i_size = src_inode->i_size;
		ret = f2fs_clone_data(dst_inode, src_inode);
		if (ret) {
			/* No need to truncate, beacuse tmpfile will be removed. */
			f2fs_err(sbi,
				"src inode[%lu] dst inode[%lu] ioc clone failed. ret=%d",
				src_inode->i_ino, dst_inode->i_ino, ret);
			return ret;
		}
	}

	set_inode_flag(dst_inode, FI_DATA_UN_MODIFY);

	return 0;
}

static int f2fs_ioc_clone_file(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct user_namespace *mnt_userns = file_mnt_user_ns(filp);
	struct inode *src_inode;
	struct f2fs_clone_info info;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct fd f;
	int ret = 0;

	if (unlikely(f2fs_cp_error(F2FS_I_SB(inode))))
		return -EIO;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	if (!inode_owner_or_capable(mnt_userns, inode))
		return -EACCES;

	if (copy_from_user(&info, (struct f2fs_clone_info __user *)arg, sizeof(info)))
		return -EFAULT;

	f = fdget_pos(info.src_fd);
	if (!f.file)
		return -EBADF;

	src_inode = file_inode(f.file);
	if (inode->i_sb != src_inode->i_sb) {
		f2fs_err(sbi, "%s: files should be in same FS ino:%lu, src_ino:%lu",
				__func__, inode->i_ino, src_inode->i_ino);
		ret = -EINVAL;
		goto out;
	}

	if (!f2fs_inode_support_dedup(sbi, src_inode)) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	ret = mnt_want_write_file(filp);
	if (ret)
		goto out;

	inode_lock(inode);
	ret = f2fs_dedup_param_check(sbi, src_inode, info.flags &
		DEDUP_FOR_SNAPSHOT ? NORMAL_INODE : OUTER_INODE,
			inode, INNER_INODE);
	if (ret)
		goto unlock;

	ret = __f2fs_ioc_clone_file(inode, src_inode, &info);
	if (ret)
		goto unlock;

	F2FS_I(inode)->i_flags |= F2FS_IMMUTABLE_FL;
	f2fs_set_inode_flags(inode);
	f2fs_mark_inode_dirty_sync(inode, true);

unlock:
	inode_unlock(inode);
	mnt_drop_write_file(filp);
out:
	fdput_pos(f);
	return ret;
}

static inline void _truncate_error_handle(struct inode *inode,
		int ret)
{
	set_sbi_flag(F2FS_I_SB(inode), SBI_NEED_FSCK);
	f2fs_err(F2FS_I_SB(inode),
		"truncate data failed, inode:%lu ret:%d",
		inode->i_ino, ret);
}

int f2fs_truncate_dedup_inode(struct inode *inode, unsigned int flag)
{
	int ret = 0;

	if (!f2fs_is_outer_inode(inode)) {
		f2fs_err(F2FS_I_SB(inode),
			"inode:%lu is not dedup inode", inode->i_ino);
		f2fs_bug_on(F2FS_I_SB(inode), 1);
		return 0;
	}

	clear_inode_flag(inode, flag);

	if (time_to_inject(F2FS_I_SB(inode), FAULT_DEDUP_TRUNCATE)) {
		ret = -EIO;
		goto err;
	}
	ret = f2fs_truncate_blocks(inode, 0, false);
	if (ret)
		goto err;

	ret = f2fs_set_inode_addr(inode, DEDUP_ADDR);
	if (ret)
		goto err;

	return 0;
err:
	_truncate_error_handle(inode, ret);
	return ret;
}

static int is_inode_match_dir_crypt_policy(struct dentry *dir,
		struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	if (time_to_inject(sbi, FAULT_DEDUP_CRYPT_POLICY))
		return -EPERM;

	if (IS_ENCRYPTED(d_inode(dir)) &&
		!fscrypt_has_permitted_context(d_inode(dir), inode)) {
		f2fs_err(sbi, "inode[%lu] not match dir[%lu] fscrypt policy",
			inode->i_ino, d_inode(dir)->i_ino);
		return -EPERM;
	}

	return 0;
}

static int deduped_files_match_fscrypt_policy(struct file *file1,
		struct file *file2)
{
	struct dentry *dir1 = dget_parent(file_dentry(file1));
	struct dentry *dir2 = dget_parent(file_dentry(file2));
	struct inode *inode1 = file_inode(file1);
	struct inode *inode2 = file_inode(file2);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode1);
	int err = 0;

	if (time_to_inject(sbi, FAULT_DEDUP_CRYPT_POLICY)) {
		err = -EPERM;
		goto out;
	}

	if (IS_ENCRYPTED(d_inode(dir1)) &&
		!fscrypt_has_permitted_context(d_inode(dir1), inode2)) {
		f2fs_err(sbi, "dir[%lu] inode[%lu] and inode[%lu] fscrypt policy not match.",
			d_inode(dir1)->i_ino, inode1->i_ino, inode2->i_ino);
		err = -EPERM;
		goto out;
	}

	if (IS_ENCRYPTED(d_inode(dir2)) &&
		!fscrypt_has_permitted_context(d_inode(dir2), inode1)) {
		f2fs_err(sbi, "inode[%lu] and dir[%lu] inode[%lu] fscrypt policy not match.",
			inode1->i_ino, d_inode(dir2)->i_ino, inode2->i_ino);
		err = -EPERM;
	}

out:
	dput(dir2);
	dput(dir1);
	return err;
}

static int f2fs_compare_page(struct page *src, struct page *dst)
{
	int ret;
	char *src_kaddr = kmap_atomic(src);
	char *dst_kaddr = kmap_atomic(dst);

	flush_dcache_page(src);
	flush_dcache_page(dst);

	ret = memcmp(dst_kaddr, src_kaddr, PAGE_SIZE);
	kunmap_atomic(src_kaddr);
	kunmap_atomic(dst_kaddr);

	return ret;
}

static inline void lock_two_pages(struct page *page1, struct page *page2)
{
	lock_page(page1);
	if (page1 != page2)
		lock_page(page2);
}

static inline void unlock_two_pages(struct page *page1, struct page *page2)
{
	unlock_page(page1);
	if (page1 != page2)
		unlock_page(page2);
}

static bool dedup_file_is_same(struct inode *src, struct inode *dst, int nr_pages)
{
	struct page *src_page, *dst_page;
	pgoff_t index, last_idx;
	int i, ret;
	bool same = true;

	if (time_to_inject(F2FS_I_SB(src), FAULT_DEDUP_SAME_FILE))
		return false;

	if (i_size_read(src) != i_size_read(dst))
		return false;

	last_idx = DIV_ROUND_UP(i_size_read(src), PAGE_SIZE);

	for (i = 0; i < nr_pages; i++) {
		index = get_random_u32() % last_idx;

		src_page = read_mapping_page(src->i_mapping, index, NULL);
		if (IS_ERR(src_page)) {
			ret = PTR_ERR(src_page);
			same = false;
			break;
		}

		dst_page = read_mapping_page(dst->i_mapping, index, NULL);
		if (IS_ERR(dst_page)) {
			ret = PTR_ERR(dst_page);
			put_page(src_page);
			same = false;
			break;
		}

		lock_two_pages(src_page, dst_page);
		if (!PageUptodate(src_page) || !PageUptodate(dst_page) ||
				src_page->mapping != src->i_mapping ||
				dst_page->mapping != dst->i_mapping) {
			ret = -EINVAL;
			same = false;
			goto unlock;
		}
		ret = f2fs_compare_page(src_page, dst_page);
		if (ret)
			same = false;
unlock:
		unlock_two_pages(src_page, dst_page);
		put_page(dst_page);
		put_page(src_page);

		if (!same) {
			f2fs_err(F2FS_I_SB(src),
					"src: %lu, dst: %lu page index: %lu is diff ret[%d]",
					src->i_ino, dst->i_ino, index, ret);
			break;
		}
	}

	return same;
}

int f2fs_filemap_write_and_wait_range(struct inode *inode)
{
	if (time_to_inject(F2FS_I_SB(inode), FAULT_DEDUP_WRITEBACK)) {
		int ret;

		ret = filemap_write_and_wait_range(inode->i_mapping, 0,
						i_size_read(inode) / 2);
		if (!ret)
			ret = -EIO;
		return ret;
	}

	return filemap_write_and_wait_range(inode->i_mapping, 0, LLONG_MAX);
}

static int __f2fs_ioc_create_layered_inode(struct inode *inode,
		struct inode *inner_inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	int ret;
	struct f2fs_inode_info *fi = F2FS_I(inode);

	if (!dedup_file_is_same(inode, inner_inode, DEDUP_COMPARE_PAGES))
		return -ESTALE;

	f2fs_lock_op(sbi);

	if (time_to_inject(sbi, FAULT_DEDUP_ORPHAN_INODE))
		ret = -ENOSPC;
	else
		ret = f2fs_acquire_orphan_inode(sbi);
	if (ret) {
		f2fs_err(sbi,
			"create layer file acquire orphan fail, ino[%lu], inner[%lu]",
			inode->i_ino, inner_inode->i_ino);
		f2fs_unlock_op(sbi);
		return ret;
	}
	f2fs_add_orphan_inode(inode);

	f2fs_down_write(&F2FS_I(inner_inode)->i_sem);
	igrab(inner_inode);
	set_inode_flag(inner_inode, FI_INNER_INODE);
	set_inode_flag(inner_inode, FI_DEDUPED);
	if (is_inode_flag_set(inode, FI_SNAPSHOT_PREPARED))
		set_inode_flag(inner_inode, FI_SNAPSHOT_PREPARED);
	f2fs_i_links_write(inner_inode, true);
	f2fs_up_write(&F2FS_I(inner_inode)->i_sem);

	f2fs_down_write(&fi->i_sem);
	fi->inner_inode = inner_inode;
	set_inode_flag(inode, FI_DEDUPED);
	//set_inode_flag(inode, FI_DOING_DEDUP);
	f2fs_up_write(&fi->i_sem);

	f2fs_remove_orphan_inode(sbi, inner_inode->i_ino);
	f2fs_unlock_op(sbi);

	wait_event(fi->dedup_wq,
			atomic_read(&fi->inflight_read_io) == 0);
	dedup_wait_dio(inode);

	f2fs_down_write(&fi->i_gc_rwsem[WRITE]);
	/* GC may dirty pages before holding lock */
	ret = f2fs_filemap_write_and_wait_range(inode);
	if (ret)
		goto out;

	f2fs_lock_op(sbi);
	f2fs_remove_orphan_inode(sbi, inode->i_ino);
	ret = f2fs_truncate_dedup_inode(inode, FI_DOING_DEDUP);
	/*
	 * Since system may do checkpoint after unlock cp,
	 * we set cp_ver here to let fsync know dedup have finish.
	 */
	F2FS_I(inode)->dedup_cp_ver = cur_cp_version(F2FS_CKPT(sbi));
	f2fs_unlock_op(sbi);

out:
	f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
	f2fs_dedup_info(sbi, "inode[%lu] create layered success, inner[%lu] ret: %d",
			inode->i_ino, inner_inode->i_ino, ret);
	trace_f2fs_dedup_ioc_create_layered_inode(inode, inner_inode);
	return ret;
}

static int f2fs_ioc_create_layered_inode(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct user_namespace *mnt_userns = file_mnt_user_ns(filp);
	struct inode *inner_inode;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_dedup_src info;
	struct dentry *dir;
	struct fd f;
	int ret;

	if (unlikely(f2fs_cp_error(F2FS_I_SB(inode))))
		return -EIO;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	if (!inode_owner_or_capable(mnt_userns, inode))
		return -EACCES;

	if (!f2fs_inode_support_dedup(sbi, inode))
		return -EOPNOTSUPP;

	if (copy_from_user(&info, (struct f2fs_dedup_src __user *)arg, sizeof(info)))
		return -EFAULT;

	f = fdget_pos(info.inner_fd);
	if (!f.file)
		return -EBADF;

	ret = mnt_want_write_file(filp);
	if (ret)
		goto out;

	inode_lock(inode);
	if (f2fs_is_deduped_inode(inode)) {
		f2fs_err(sbi, "The inode[%lu] has been two layer file.",
			inode->i_ino);
		ret = -EINVAL;
		goto unlock;
	}

	inner_inode = file_inode(f.file);
	if (inode->i_sb != inner_inode->i_sb) {
		f2fs_err(sbi, "%s files should be in same FS ino:%lu, inner_ino:%lu",
				__func__, inode->i_ino, inner_inode->i_ino);
		ret = -EINVAL;
		goto unlock;
	}

	if (!IS_IMMUTABLE(inner_inode)) {
		f2fs_err(sbi, "create layer fail inner[%lu] is not immutable.",
			inner_inode->i_ino);
		ret = -EINVAL;
		goto unlock;
	}

	ret = f2fs_dedup_param_check(sbi, inode, OUTER_INODE,
			inner_inode, INNER_INODE);
	if (ret)
		goto unlock;

	if (inode->i_nlink == 0) {
		f2fs_err(sbi,
			"The inode[%lu] has been removed.", inode->i_ino);
		ret = -ENOENT;
		goto unlock;
	}

	dir = dget_parent(file_dentry(filp));
	ret = is_inode_match_dir_crypt_policy(dir, inner_inode);
	dput(dir);
	if (ret)
		goto unlock;

	filemap_fdatawrite(inode->i_mapping);
	ret = f2fs_filemap_write_and_wait_range(inode);
	if (ret)
		goto unlock;

	ret = __f2fs_ioc_create_layered_inode(inode, inner_inode);

unlock:
	inode_unlock(inode);
	mnt_drop_write_file(filp);
out:
	fdput_pos(f);
	return ret;
}

static int __f2fs_ioc_dedup_file(struct inode *base_inode,
		struct inode *dedup_inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dedup_inode);
	struct inode *inner = get_inner_inode(base_inode);
	int ret = 0;
	struct f2fs_inode_info *fi = F2FS_I(dedup_inode);

	if (!inner)
		return -EBADF;

	if (is_inode_flag_set(dedup_inode, FI_SNAPSHOTED))
		goto skip;
	if (!dedup_file_is_same(base_inode, dedup_inode, DEDUP_COMPARE_PAGES)) {
		put_inner_inode(inner);
		return -ESTALE;
	}
skip:
	f2fs_lock_op(sbi);

	if (time_to_inject(sbi, FAULT_DEDUP_ORPHAN_INODE))
		ret = -ENOSPC;
	else
		ret = f2fs_acquire_orphan_inode(sbi);
	if (ret) {
		f2fs_unlock_op(sbi);
		f2fs_err(sbi,
			"dedup file acquire orphan fail, ino[%lu], base ino[%lu]",
			dedup_inode->i_ino, base_inode->i_ino);
		put_inner_inode(inner);
		return ret;
	}
	f2fs_add_orphan_inode(dedup_inode);

	f2fs_down_write(&fi->i_sem);
	fi->inner_inode = inner;
	set_inode_flag(dedup_inode, FI_DEDUPED);
	set_inode_flag(dedup_inode, FI_DOING_DEDUP);
	f2fs_up_write(&fi->i_sem);

	f2fs_down_write(&F2FS_I(inner)->i_sem);
	f2fs_i_links_write(inner, true);
	f2fs_up_write(&F2FS_I(inner)->i_sem);
	f2fs_unlock_op(sbi);

	wait_event(fi->dedup_wq,
			atomic_read(&fi->inflight_read_io) == 0);
	dedup_wait_dio(dedup_inode);

	f2fs_down_write(&fi->i_gc_rwsem[WRITE]);
	/* GC may dirty pages before holding lock */
	ret = f2fs_filemap_write_and_wait_range(dedup_inode);
	if (ret)
		goto out;

	f2fs_lock_op(sbi);
	f2fs_remove_orphan_inode(sbi, dedup_inode->i_ino);
	ret = f2fs_truncate_dedup_inode(dedup_inode, FI_DOING_DEDUP);
	/*
	 * Since system may do checkpoint after unlock cp,
	 * we set cp_ver here to let fsync know dedup have finish.
	 */
	F2FS_I(dedup_inode)->dedup_cp_ver = cur_cp_version(F2FS_CKPT(sbi));
	f2fs_unlock_op(sbi);
out:
	f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
	f2fs_dedup_info(sbi, "%s inode[%lu] dedup success, inner[%lu], ret[%d]",
			__func__, dedup_inode->i_ino, inner->i_ino, ret);
	trace_f2fs_dedup_ioc_dedup_inode(dedup_inode, inner);
	return ret;
}

static int f2fs_ioc_dedup_file(struct file *filp, unsigned long arg)
{
	struct inode *dedup_inode = file_inode(filp);
	struct user_namespace *mnt_userns = file_mnt_user_ns(filp);
	struct inode *base_inode, *inner_inode;
	struct dentry *dir;
	struct f2fs_sb_info *sbi = F2FS_I_SB(dedup_inode);
	struct f2fs_dedup_dst info;
	struct fd f;
	int ret;

	if (unlikely(f2fs_cp_error(F2FS_I_SB(dedup_inode))))
		return -EIO;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	if (!inode_owner_or_capable(mnt_userns, dedup_inode))
		return -EACCES;

	if (!f2fs_inode_support_dedup(sbi, dedup_inode))
		return -EOPNOTSUPP;

	if (copy_from_user(&info, (struct f2fs_dedup_dst __user *)arg, sizeof(info)))
		return -EFAULT;

	f = fdget_pos(info.base_fd);
	if (!f.file)
		return -EBADF;

	base_inode = file_inode(f.file);
	if (dedup_inode->i_sb != base_inode->i_sb) {
		f2fs_err(sbi, "%s: files should be in same FS ino:%lu, base_ino:%lu",
				__func__, dedup_inode->i_ino, base_inode->i_ino);
		ret = -EINVAL;
		goto out;
	}

	if (!f2fs_inode_support_dedup(sbi, base_inode)) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (base_inode == dedup_inode) {
		f2fs_err(sbi, "%s: input inode[%lu] and [%lu] are same.",
			__func__, base_inode->i_ino, dedup_inode->i_ino);
		ret = -EINVAL;
		goto out;
	}

	// if try to dedup, clear FI_SNAPSHOT_PREPARED of base inode, else set for snapshot
	if (is_inode_flag_set(dedup_inode, FI_SNAPSHOTED)) {
		set_inode_flag(base_inode, FI_SNAPSHOT_PREPARED);
	} else {
		clear_inode_flag(base_inode, FI_SNAPSHOT_PREPARED);
	}

	ret = mnt_want_write_file(filp);
	if (ret)
		goto out;

	// dedup_inode lock already hold if create snapshot
	if (!is_inode_flag_set(dedup_inode, FI_SNAPSHOTED))
		inode_lock(dedup_inode);
	if (!inode_trylock(base_inode)) {
		f2fs_err(sbi, "inode[%lu] can't get lock", base_inode->i_ino);
		ret = -EAGAIN;
		goto unlock2;
	}

#ifdef CONFIG_F2FS_SEQZONE
	if (f2fs_seqzone_file(base_inode) ||
		f2fs_seqzone_file(dedup_inode)) {
		ret = -EINVAL;
		goto unlock1;
	}
#endif
	if (f2fs_is_deduped_inode(dedup_inode)) {
		f2fs_err(sbi, "dedup inode[%lu] has been two layer inode",
			dedup_inode->i_ino);
		ret = -EINVAL;
		goto unlock1;
	}

	if (dedup_inode->i_nlink == 0) {
		f2fs_err(sbi,
			"dedup inode[%lu] has been removed.", dedup_inode->i_ino);
		ret = -ENOENT;
		goto unlock1;
	}

	if (!f2fs_is_outer_inode(base_inode)) {
		f2fs_err(sbi, "base inode[%lu] is not outer inode",
			base_inode->i_ino);
		ret = -EINVAL;
		goto unlock1;
	}

	ret = f2fs_dedup_param_check(sbi, base_inode, OUTER_INODE,
			dedup_inode, OUTER_INODE);
	if (ret)
		goto unlock1;

	dir = dget_parent(file_dentry(filp));
	inner_inode = get_inner_inode(base_inode);
	if (likely(inner_inode)) {
		ret = is_inode_match_dir_crypt_policy(dir, inner_inode);
		put_inner_inode(inner_inode);
	}
	dput(dir);
	if (ret)
		goto unlock1;

	ret = deduped_files_match_fscrypt_policy(filp, f.file);
	if (ret)
		goto unlock1;

	filemap_fdatawrite(dedup_inode->i_mapping);
	ret = f2fs_filemap_write_and_wait_range(dedup_inode);
	if (ret)
		goto unlock1;

	ret = __f2fs_ioc_dedup_file(base_inode, dedup_inode);

unlock1:
	inode_unlock(base_inode);
unlock2:
	if (!is_inode_flag_set(dedup_inode, FI_SNAPSHOTED))
		inode_unlock(dedup_inode);
	mnt_drop_write_file(filp);
out:
	fdput_pos(f);
	return ret;
}

static int f2fs_ioc_dedup_revoke(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct user_namespace *mnt_userns = file_mnt_user_ns(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	int ret = 0;

	if (unlikely(f2fs_cp_error(F2FS_I_SB(inode))))
		return -EIO;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	if (!inode_owner_or_capable(mnt_userns, inode))
		return -EACCES;

	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	if (!f2fs_inode_support_dedup(sbi, inode))
		return -EOPNOTSUPP;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	inode_lock(inode);
	ret = f2fs_revoke_deduped_inode(inode, __func__);
	inode_unlock(inode);

	mnt_drop_write_file(filp);
	return ret;
}

static int f2fs_ioc_get_dedupd_file_info(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_dedup_file_info info = {0};
	struct inode *inner_inode;
	int ret = 0;

	if (unlikely(f2fs_cp_error(F2FS_I_SB(inode))))
		return -EIO;

	if (!f2fs_inode_support_dedup(sbi, inode))
		return -EOPNOTSUPP;

	inode_lock(inode);

	if (!is_inode_flag_set(inode, FI_DEDUPED)) {
		info.is_layered = false;
		info.is_deduped = false;
	} else {
		info.is_layered = true;
		inner_inode = F2FS_I(inode)->inner_inode;
		if (inner_inode) {
			f2fs_down_write(&F2FS_I(inner_inode)->i_sem);
			if (inner_inode->i_nlink > 1)
				info.is_deduped = true;

			info.group = inner_inode->i_ino;
			f2fs_up_write(&F2FS_I(inner_inode)->i_sem);
		}
	}

	inode_unlock(inode);

	if (copy_to_user((struct f2fs_dedup_file_info __user *)arg, &info, sizeof(info)))
		ret = -EFAULT;

	return ret;
}

/* used for dedup big data statistics */
static int f2fs_ioc_get_dedup_sysinfo(struct file *filp, unsigned long arg)
{
	return 0;
}

static int f2fs_ioc_create_snapshot(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	int ret = 0;
	struct inode *base_inode;
	struct f2fs_dedup_dst info;
	struct fd f;

	if (copy_from_user(&info, (struct f2fs_dedup_dst __user *)arg, sizeof(info)))
		return -EFAULT;

	inode_lock(inode);

	//do not create snapshot for a snapshot file
	if (is_inode_flag_set(inode, FI_SNAPSHOTED)) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	set_inode_flag(inode, FI_SNAPSHOTED);
	ret = f2fs_ioc_dedup_file(filp, arg);
	if (!ret) {
		f = fdget_pos(info.base_fd);
		if (!f.file) {
			ret = -EBADF;
			goto unlock;
		}
		base_inode = file_inode(f.file);
		inode_lock(base_inode);
		clear_inode_flag(base_inode, FI_SNAPSHOT_PREPARED);
		//clone ctime/mtime... from base to snapshot
		inode->i_atime.tv_sec = base_inode->i_atime.tv_sec;
		inode->i_ctime.tv_sec = base_inode->i_ctime.tv_sec;
		inode->i_mtime.tv_sec = base_inode->i_mtime.tv_sec;
		inode->i_atime.tv_nsec = base_inode->i_atime.tv_nsec;
		inode->i_ctime.tv_nsec = base_inode->i_ctime.tv_nsec;
		inode->i_mtime.tv_nsec = base_inode->i_mtime.tv_nsec;
		inode_unlock(base_inode);
		fdput_pos(f);
	} else {
		clear_inode_flag(inode, FI_SNAPSHOTED);
	}

unlock:
	inode_unlock(inode);
	return ret;
}

static int f2fs_ioc_prepare_snapshot(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	int ret = 0;

	set_inode_flag(inode, FI_SNAPSHOT_PREPARED);
	ret = f2fs_ioc_create_layered_inode(filp, arg);
	if (ret)
		clear_inode_flag(inode, FI_SNAPSHOT_PREPARED);
	return ret;
}

#endif
static vm_fault_t f2fs_filemap_fault(struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	vm_fault_t ret;

	ret = filemap_fault(vmf);
	if (!ret)
		f2fs_update_iostat(F2FS_I_SB(inode), inode,
					APP_MAPPED_READ_IO, F2FS_BLKSIZE);

	trace_f2fs_filemap_fault(inode, vmf->pgoff, (unsigned long)ret);

	return ret;
}

static vm_fault_t f2fs_vm_page_mkwrite(struct vm_fault *vmf)
{
	struct page *page = vmf->page;
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct dnode_of_data dn;
	bool need_alloc = true;
	int err = 0;

	if (unlikely(IS_IMMUTABLE(inode)))
		return VM_FAULT_SIGBUS;

	if (is_inode_flag_set(inode, FI_COMPRESS_RELEASED)) {
#ifdef CONFIG_F2FS_FS_COMPRESSION_FIXED_OUTPUT
		inode_lock(inode);
		err = f2fs_reserve_compress_blocks(inode, NULL);
		inode_unlock(inode);
		if (err < 0)
			goto err;
#else
		return VM_FAULT_SIGBUS;
#endif
	}

	if (unlikely(f2fs_cp_error(sbi))) {
		err = -EIO;
		goto err;
	}

	if (!f2fs_is_checkpoint_ready(sbi)) {
		err = -ENOSPC;
		goto err;
	}

	err = f2fs_convert_inline_inode(inode);
	if (err)
		goto err;

#ifdef CONFIG_F2FS_FS_DEDUP
	inode_lock(inode);
	mark_file_modified(inode);
	if (f2fs_is_outer_inode(inode)) {
		err = f2fs_revoke_deduped_inode(inode, __func__);
		if (err) {
			inode_unlock(inode);
			goto err;
		}
	}
	inode_unlock(inode);
#endif

#ifdef CONFIG_F2FS_FS_COMPRESSION
	if (f2fs_compressed_file(inode)) {
		int ret;
		CLEAR_IFLAG_IF_SET(inode, F2FS_NOCOMP_FL);
		ret = f2fs_is_compressed_cluster(inode, page->index);

		if (ret < 0) {
			err = ret;
			goto err;
		} else if (ret) {
			need_alloc = false;
		}
#ifdef CONFIG_F2FS_FS_COMPRESSION_FIXED_OUTPUT
		inode_lock(inode);
		clear_inode_flag(inode, FI_ENABLE_COMPRESS);
		inode_unlock(inode);
#endif
	}
#endif
	/* should do out of any locked page */
	if (need_alloc)
		f2fs_balance_fs(sbi, true);

	sb_start_pagefault(inode->i_sb);

	f2fs_bug_on(sbi, f2fs_has_inline_data(inode));

	file_update_time(vmf->vma->vm_file);
	filemap_invalidate_lock_shared(inode->i_mapping);
	lock_page(page);
	if (unlikely(page->mapping != inode->i_mapping ||
			page_offset(page) > i_size_read(inode) ||
			!PageUptodate(page))) {
		unlock_page(page);
		err = -EFAULT;
		goto out_sem;
	}

	if (need_alloc) {
		/* block allocation */
		set_new_dnode(&dn, inode, NULL, NULL, 0);
		err = f2fs_get_block_locked(&dn, page->index);
	}

#ifdef CONFIG_F2FS_FS_COMPRESSION
	if (!need_alloc) {
		set_new_dnode(&dn, inode, NULL, NULL, 0);
		err = f2fs_get_dnode_of_data(&dn, page->index, LOOKUP_NODE);
		f2fs_put_dnode(&dn);
	}
#endif
	if (err) {
		unlock_page(page);
		goto out_sem;
	}

	f2fs_wait_on_page_writeback(page, DATA, false, true);

	/* wait for GCed page writeback via META_MAPPING */
	f2fs_wait_on_block_writeback(inode, dn.data_blkaddr);

	/*
	 * check to see if the page is mapped already (no holes)
	 */
	if (PageMappedToDisk(page))
		goto out_sem;

	/* page is wholly or partially inside EOF */
	if (((loff_t)(page->index + 1) << PAGE_SHIFT) >
						i_size_read(inode)) {
		loff_t offset;

		offset = i_size_read(inode) & ~PAGE_MASK;
		zero_user_segment(page, offset, PAGE_SIZE);
	}
	set_page_dirty(page);
	if (!PageUptodate(page))
		SetPageUptodate(page);

	f2fs_update_iostat(sbi, inode, APP_MAPPED_IO, F2FS_BLKSIZE);
	f2fs_update_time(sbi, REQ_TIME);

	trace_f2fs_vm_page_mkwrite(page, DATA);
out_sem:
	filemap_invalidate_unlock_shared(inode->i_mapping);

	sb_end_pagefault(inode->i_sb);
err:
	return block_page_mkwrite_return(err);
}

static const struct vm_operations_struct f2fs_file_vm_ops = {
	.fault		= f2fs_filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= f2fs_vm_page_mkwrite,
};

static int get_parent_ino(struct inode *inode, nid_t *pino)
{
	struct dentry *dentry;

	/*
	 * Make sure to get the non-deleted alias.  The alias associated with
	 * the open file descriptor being fsync()'ed may be deleted already.
	 */
	dentry = d_find_alias(inode);
	if (!dentry)
		return 0;

	*pino = parent_ino(dentry);
	dput(dentry);
	return 1;
}

static inline enum cp_reason_type need_do_checkpoint(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	enum cp_reason_type cp_reason = CP_NO_NEEDED;

	if (!S_ISREG(inode->i_mode))
		cp_reason = CP_NON_REGULAR;
	else if (f2fs_compressed_file(inode))
		cp_reason = CP_COMPRESSED;
#ifdef CONFIG_F2FS_FS_DEDUP
	/*
	 * If inode have do dedup or revoke recently, we need to do
	 * checkpoint to avoid roll forward recovery after fsync,
	 * which may cause data inconsistency.
	 */
	else if (F2FS_I(inode)->dedup_cp_ver == cur_cp_version(F2FS_CKPT(sbi)))
		cp_reason = CP_DEDUPED;
#endif
	else if (inode->i_nlink != 1)
		cp_reason = CP_HARDLINK;
	else if (is_sbi_flag_set(sbi, SBI_NEED_CP))
		cp_reason = CP_SB_NEED_CP;
	else if (file_wrong_pino(inode))
		cp_reason = CP_WRONG_PINO;
	else if (!f2fs_space_for_roll_forward(sbi))
		cp_reason = CP_NO_SPC_ROLL;
	else if (!f2fs_is_checkpointed_node(sbi, F2FS_I(inode)->i_pino))
		cp_reason = CP_NODE_NEED_CP;
	else if (test_opt(sbi, FASTBOOT))
		cp_reason = CP_FASTBOOT_MODE;
	else if (F2FS_OPTION(sbi).active_logs == 2)
		cp_reason = CP_SPEC_LOG_NUM;
	else if (F2FS_OPTION(sbi).fsync_mode == FSYNC_MODE_STRICT &&
		f2fs_need_dentry_mark(sbi, inode->i_ino) &&
		f2fs_exist_written_data(sbi, F2FS_I(inode)->i_pino,
							TRANS_DIR_INO))
		cp_reason = CP_RECOVER_DIR;
	else if (f2fs_exist_written_data(sbi, F2FS_I(inode)->i_pino,
							XATTR_DIR_INO))
		cp_reason = CP_XATTR_DIR;

	return cp_reason;
}

static bool need_inode_page_update(struct f2fs_sb_info *sbi, nid_t ino)
{
	struct page *i = find_get_page(NODE_MAPPING(sbi), ino);
	bool ret = false;
	/* But we need to avoid that there are some inode updates */
	if ((i && PageDirty(i)) || f2fs_need_inode_block_update(sbi, ino))
		ret = true;
	f2fs_put_page(i, 0);
	return ret;
}

static void try_to_fix_pino(struct inode *inode)
{
	struct f2fs_inode_info *fi = F2FS_I(inode);
	nid_t pino;

	f2fs_down_write(&fi->i_sem);
	if (file_wrong_pino(inode) && inode->i_nlink == 1 &&
			get_parent_ino(inode, &pino)) {
		f2fs_i_pino_write(inode, pino);
		file_got_pino(inode);
	}
	f2fs_up_write(&fi->i_sem);
}

static int f2fs_do_sync_file(struct file *file, loff_t start, loff_t end,
						int datasync, bool atomic)
{
	struct inode *inode = file->f_mapping->host;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	nid_t ino = inode->i_ino;
	int ret = 0;
	enum cp_reason_type cp_reason = 0;
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = LONG_MAX,
		.for_reclaim = 0,
	};
	unsigned int seq_id = 0;
#ifdef CONFIG_F2FS_FS_DEDUP
	struct inode *inner = NULL, *outer = NULL;

	if(is_inode_flag_set(inode, FI_SNAPSHOT_PREPARED)) {
		inner = get_inner_inode(outer);
		if (inner) {
			outer = inode;
			inode = inner;
		}
	}
#endif
	if (unlikely(f2fs_readonly(inode->i_sb)))
		return 0;

	trace_f2fs_sync_file_enter(inode);

	if (S_ISDIR(inode->i_mode))
		goto go_write;

	/* if fdatasync is triggered, let's do in-place-update */
#ifdef CONFIG_F2FS_SEQZONE
	if (datasync || get_dirty_pages(inode) <= DEF_MIN_FSYNC_BLOCKS)
#else
	if (datasync || get_dirty_pages(inode) <= SM_I(sbi)->min_fsync_blocks)
#endif
		set_inode_flag(inode, FI_NEED_IPU);
	ret = file_write_and_wait_range(file, start, end);
	clear_inode_flag(inode, FI_NEED_IPU);

	if (ret || is_sbi_flag_set(sbi, SBI_CP_DISABLED)) {
		trace_f2fs_sync_file_exit(inode, cp_reason, datasync, ret);
		return ret;
	}

	/* if the inode is dirty, let's recover all the time */
	if (!f2fs_skip_inode_update(inode, datasync)) {
		f2fs_write_inode(inode, NULL);
		goto go_write;
	}

	/*
	 * if there is no written data, don't waste time to write recovery info.
	 */
	if (!is_inode_flag_set(inode, FI_APPEND_WRITE) &&
			!f2fs_exist_written_data(sbi, ino, APPEND_INO)) {

		/* it may call write_inode just prior to fsync */
		if (need_inode_page_update(sbi, ino))
			goto go_write;

		if (is_inode_flag_set(inode, FI_UPDATE_WRITE) ||
				f2fs_exist_written_data(sbi, ino, UPDATE_INO))
			goto flush_out;
		goto out;
	} else {
		/*
		 * for OPU case, during fsync(), node can be persisted before
		 * data when lower device doesn't support write barrier, result
		 * in data corruption after SPO.
		 * So for strict fsync mode, force to use atomic write semantics
		 * to keep write order in between data/node and last node to
		 * avoid potential data corruption.
		 */
		if (F2FS_OPTION(sbi).fsync_mode ==
				FSYNC_MODE_STRICT && !atomic)
			atomic = true;
	}
go_write:
	/*
	 * Both of fdatasync() and fsync() are able to be recovered from
	 * sudden-power-off.
	 */
	f2fs_down_read(&F2FS_I(inode)->i_sem);
	cp_reason = need_do_checkpoint(inode);
	f2fs_up_read(&F2FS_I(inode)->i_sem);

	if (cp_reason) {
		/* all the dirty node pages should be flushed for POR */
		ret = f2fs_sync_fs(inode->i_sb, 1);

		/*
		 * We've secured consistency through sync_fs. Following pino
		 * will be used only for fsynced inodes after checkpoint.
		 */
		try_to_fix_pino(inode);
		clear_inode_flag(inode, FI_APPEND_WRITE);
		clear_inode_flag(inode, FI_UPDATE_WRITE);
		goto out;
	}
sync_nodes:
	atomic_inc(&sbi->wb_sync_req[NODE]);
	ret = f2fs_fsync_node_pages(sbi, inode, &wbc, atomic, &seq_id);
	atomic_dec(&sbi->wb_sync_req[NODE]);
	if (ret)
		goto out;

	/* if cp_error was enabled, we should avoid infinite loop */
	if (unlikely(f2fs_cp_error(sbi))) {
		ret = -EIO;
		goto out;
	}

	if (f2fs_need_inode_block_update(sbi, ino)) {
		f2fs_mark_inode_dirty_sync(inode, true);
		f2fs_write_inode(inode, NULL);
		goto sync_nodes;
	}

	/*
	 * If it's atomic_write, it's just fine to keep write ordering. So
	 * here we don't need to wait for node write completion, since we use
	 * node chain which serializes node blocks. If one of node writes are
	 * reordered, we can see simply broken chain, resulting in stopping
	 * roll-forward recovery. It means we'll recover all or none node blocks
	 * given fsync mark.
	 */
	if (!atomic) {
		ret = f2fs_wait_on_node_pages_writeback(sbi, seq_id);
		if (ret)
			goto out;
	}

	/* once recovery info is written, don't need to tack this */
	f2fs_remove_ino_entry(sbi, ino, APPEND_INO);
	clear_inode_flag(inode, FI_APPEND_WRITE);
flush_out:
	if ((!atomic && F2FS_OPTION(sbi).fsync_mode != FSYNC_MODE_NOBARRIER) ||
	    (atomic && !test_opt(sbi, NOBARRIER) && f2fs_sb_has_blkzoned(sbi)))
		ret = f2fs_issue_flush(sbi, inode->i_ino);
	if (!ret) {
		f2fs_remove_ino_entry(sbi, ino, UPDATE_INO);
		clear_inode_flag(inode, FI_UPDATE_WRITE);
		f2fs_remove_ino_entry(sbi, ino, FLUSH_INO);
	}
	f2fs_update_time(sbi, REQ_TIME);
out:
#ifdef CONFIG_F2FS_FS_DEDUP
	if (inner) {
		inode = outer;
		put_inner_inode(inner);
	}
#endif
	trace_f2fs_sync_file_exit(inode, cp_reason, datasync, ret);
	return ret;
}

int f2fs_sync_file(struct file *file, loff_t start, loff_t end, int datasync)
{
	if (unlikely(f2fs_cp_error(F2FS_I_SB(file_inode(file)))))
		return -EIO;
	return f2fs_do_sync_file(file, start, end, datasync, false);
}

static bool __found_offset(struct address_space *mapping,
		struct dnode_of_data *dn, pgoff_t index, int whence)
{
	block_t blkaddr = f2fs_data_blkaddr(dn);
	struct inode *inode = mapping->host;
	bool compressed_cluster = false;

	if (f2fs_compressed_file(inode)) {
		block_t first_blkaddr = data_blkaddr(dn->inode, dn->node_page,
		    ALIGN_DOWN(dn->ofs_in_node, F2FS_I(inode)->i_cluster_size));

		compressed_cluster = first_blkaddr == COMPRESS_ADDR;
	}

	switch (whence) {
	case SEEK_DATA:
		if (__is_valid_data_blkaddr(blkaddr))
			return true;
		if (blkaddr == NEW_ADDR &&
		    xa_get_mark(&mapping->i_pages, index, PAGECACHE_TAG_DIRTY))
			return true;
		if (compressed_cluster)
			return true;
		break;
	case SEEK_HOLE:
		if (compressed_cluster)
			return false;
		if (blkaddr == NULL_ADDR)
			return true;
		break;
	}
	return false;
}

static loff_t f2fs_seek_block(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;
	loff_t maxbytes = inode->i_sb->s_maxbytes;
	struct dnode_of_data dn;
	pgoff_t pgofs, end_offset;
	loff_t data_ofs = offset;
	loff_t isize;
	int err = 0;
#ifdef CONFIG_F2FS_FS_DEDUP
	struct inode *inner = NULL, *exter = NULL;
#endif

	inode_lock(inode);

	isize = i_size_read(inode);
	if (offset >= isize)
		goto fail;

	/* handle inline data case */
	if (f2fs_has_inline_data(inode)) {
		if (whence == SEEK_HOLE) {
			data_ofs = isize;
			goto found;
		} else if (whence == SEEK_DATA) {
			data_ofs = offset;
			goto found;
		}
	}

	pgofs = (pgoff_t)(offset >> PAGE_SHIFT);

#ifdef CONFIG_F2FS_FS_DEDUP
	inner = get_inner_inode(inode);
	if (inner) {
		exter = inode;
		inode = inner;
	}
#endif
	for (; data_ofs < isize; data_ofs = (loff_t)pgofs << PAGE_SHIFT) {
		set_new_dnode(&dn, inode, NULL, NULL, 0);
		err = f2fs_get_dnode_of_data(&dn, pgofs, LOOKUP_NODE);
		if (err && err != -ENOENT) {
			goto fail;
		} else if (err == -ENOENT) {
			/* direct node does not exists */
			if (whence == SEEK_DATA) {
				pgofs = f2fs_get_next_page_offset(&dn, pgofs);
				continue;
			} else {
				goto found;
			}
		}

		end_offset = ADDRS_PER_PAGE(dn.node_page, inode);

		/* find data/hole in dnode block */
		for (; dn.ofs_in_node < end_offset;
				dn.ofs_in_node++, pgofs++,
				data_ofs = (loff_t)pgofs << PAGE_SHIFT) {
			block_t blkaddr;

			blkaddr = f2fs_data_blkaddr(&dn);

			if (__is_valid_data_blkaddr(blkaddr) &&
				!f2fs_is_valid_blkaddr(F2FS_I_SB(inode),
					blkaddr, DATA_GENERIC_ENHANCE)) {
				f2fs_put_dnode(&dn);
				goto fail;
			}

			if (__found_offset(file->f_mapping, &dn,
							pgofs, whence)) {
				f2fs_put_dnode(&dn);
				goto found;
			}
		}
		f2fs_put_dnode(&dn);
	}

	if (whence == SEEK_DATA)
		goto fail;
found:
	if (whence == SEEK_HOLE && data_ofs > isize)
		data_ofs = isize;
#ifdef CONFIG_F2FS_FS_DEDUP
	if (inner) {
		inode = exter;
		put_inner_inode(inner);
	}
#endif
	inode_unlock(inode);
	return vfs_setpos(file, data_ofs, maxbytes);
fail:
#ifdef CONFIG_F2FS_FS_DEDUP
	if (inner) {
		inode = exter;
		put_inner_inode(inner);
	}
#endif
	inode_unlock(inode);
	return -ENXIO;
}

static loff_t f2fs_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;
	loff_t maxbytes = inode->i_sb->s_maxbytes;

	if (f2fs_compressed_file(inode))
		maxbytes = max_file_blocks(inode) << F2FS_BLKSIZE_BITS;

	switch (whence) {
	case SEEK_SET:
	case SEEK_CUR:
	case SEEK_END:
		return generic_file_llseek_size(file, offset, whence,
						maxbytes, i_size_read(inode));
	case SEEK_DATA:
	case SEEK_HOLE:
		if (offset < 0)
			return -ENXIO;
		return f2fs_seek_block(file, offset, whence);
	}

	return -EINVAL;
}

static int f2fs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file_inode(file);

	if (unlikely(f2fs_cp_error(F2FS_I_SB(inode))))
		return -EIO;

	if (!f2fs_is_compress_backend_ready(inode))
		return -EOPNOTSUPP;

	file_accessed(file);
	vma->vm_ops = &f2fs_file_vm_ops;

	f2fs_down_read(&F2FS_I(inode)->i_sem);
	set_inode_flag(inode, FI_MMAP_FILE);
	f2fs_up_read(&F2FS_I(inode)->i_sem);

	return 0;
}

static int finish_preallocate_blocks(struct inode *inode)
{
	int ret;

	inode_lock(inode);
	if (is_inode_flag_set(inode, FI_OPENED_FILE)) {
		inode_unlock(inode);
		return 0;
	}

	if (!file_should_truncate(inode)) {
		set_inode_flag(inode, FI_OPENED_FILE);
		inode_unlock(inode);
		return 0;
	}

	f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
	filemap_invalidate_lock(inode->i_mapping);

	truncate_setsize(inode, i_size_read(inode));
	ret = f2fs_truncate(inode);

	filemap_invalidate_unlock(inode->i_mapping);
	f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);

	if (!ret)
		set_inode_flag(inode, FI_OPENED_FILE);

	inode_unlock(inode);
	if (ret)
		return ret;

	file_dont_truncate(inode);
	return 0;
}

static int f2fs_release_file(struct inode *inode, struct file *filp);
static int f2fs_file_open(struct inode *inode, struct file *filp)
{
#ifdef CONFIG_F2FS_FS_DEDUP
	struct inode *inner = NULL;
#endif
	int err;

#ifdef CONFIG_F2FS_FS_DEDUP
	if (time_to_inject(F2FS_I_SB(inode), FAULT_DEDUP_OPEN))
		return -EIO;
#endif

	err = fscrypt_file_open(inode, filp);
	if (err)
		return err;

	if (!f2fs_is_compress_backend_ready(inode))
		return -EOPNOTSUPP;

	err = fsverity_file_open(inode, filp);
	if (err)
		return err;

	filp->f_mode |= FMODE_NOWAIT;

#ifdef CONFIG_F2FS_FS_DEDUP
	err = dquot_file_open(inode, filp);
	if (err)
		return err;

	if (f2fs_is_outer_inode(inode)) {
		inner = get_inner_inode(inode);
		if (inner) {
			err = f2fs_file_open(inner, filp);
		} else {
			f2fs_release_file(inode, filp);
			return -ENOENT;
		}
		put_inner_inode(inner);
		return err;
	}
	return finish_preallocate_blocks(inode);
#else
	err = dquot_file_open(inode, filp);
	if (err)
		return err;

	return finish_preallocate_blocks(inode);
#endif
}

void f2fs_truncate_data_blocks_range(struct dnode_of_data *dn, int count)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dn->inode);
	int nr_free = 0, ofs = dn->ofs_in_node, len = count;
	__le32 *addr;
	bool compressed_cluster = false;
	int cluster_index = 0, valid_blocks = 0;
	int cluster_size = F2FS_I(dn->inode)->i_cluster_size;
	bool released = !atomic_read(&F2FS_I(dn->inode)->i_compr_blocks);

	addr = get_dnode_addr(dn->inode, dn->node_page) + ofs;

	/* Assumption: truncation starts with cluster */
	for (; count > 0; count--, addr++, dn->ofs_in_node++, cluster_index++) {
		block_t blkaddr = le32_to_cpu(*addr);

		if (f2fs_compressed_file(dn->inode) &&
					!(cluster_index & (cluster_size - 1))) {
			if (compressed_cluster)
				f2fs_i_compr_blocks_update(dn->inode,
							valid_blocks, false);
			compressed_cluster = (blkaddr == COMPRESS_ADDR);
			valid_blocks = 0;
		}

		if (blkaddr == NULL_ADDR)
			continue;

#ifdef CONFIG_F2FS_FS_SEQZONE
		if (f2fs_seqzone_file(dn->inode))
			dn->seqzone_index = NULL_ADDR;
#endif
		f2fs_set_data_blkaddr(dn, NULL_ADDR);

#ifdef CONFIG_F2FS_FS_DEDUP
		if (blkaddr == DEDUP_ADDR)
			continue;
#endif
		if (__is_valid_data_blkaddr(blkaddr)) {
			if (!f2fs_is_valid_blkaddr(sbi, blkaddr,
					DATA_GENERIC_ENHANCE))
				continue;
			if (compressed_cluster)
				valid_blocks++;
		}

		f2fs_invalidate_blocks(sbi, blkaddr);

		if (!released || blkaddr != COMPRESS_ADDR)
			nr_free++;
	}

	if (compressed_cluster)
		f2fs_i_compr_blocks_update(dn->inode, valid_blocks, false);

	if (nr_free) {
		pgoff_t fofs;
		/*
		 * once we invalidate valid blkaddr in range [ofs, ofs + count],
		 * we will invalidate all blkaddr in the whole range.
		 */
		fofs = f2fs_start_bidx_of_node(ofs_of_node(dn->node_page),
							dn->inode) + ofs;
		f2fs_update_read_extent_cache_range(dn, fofs, 0, len);
		f2fs_update_age_extent_cache_range(dn, fofs, len);
		dec_valid_block_count(sbi, dn->inode, nr_free);
	}
	dn->ofs_in_node = ofs;

	f2fs_update_time(sbi, REQ_TIME);
	trace_f2fs_truncate_data_blocks_range(dn->inode, dn->nid,
					 dn->ofs_in_node, nr_free);
}

void f2fs_truncate_data_blocks(struct dnode_of_data *dn)
{
	f2fs_truncate_data_blocks_range(dn, ADDRS_PER_BLOCK(dn->inode));
}

static int truncate_partial_data_page(struct inode *inode, u64 from,
								bool cache_only)
{
	loff_t offset = from & (PAGE_SIZE - 1);
	pgoff_t index = from >> PAGE_SHIFT;
	struct address_space *mapping = inode->i_mapping;
	struct page *page;

	if (!offset && !cache_only)
		return 0;

	if (cache_only) {
		page = find_lock_page(mapping, index);
		if (page && PageUptodate(page))
			goto truncate_out;
		f2fs_put_page(page, 1);
		return 0;
	}

	page = f2fs_get_lock_data_page(inode, index, true);
	if (IS_ERR(page))
		return PTR_ERR(page) == -ENOENT ? 0 : PTR_ERR(page);
truncate_out:
	f2fs_wait_on_page_writeback(page, DATA, true, true);
	zero_user(page, offset, PAGE_SIZE - offset);

	/* An encrypted inode should have a key and truncate the last page. */
	f2fs_bug_on(F2FS_I_SB(inode), cache_only && IS_ENCRYPTED(inode));
	if (!cache_only)
		set_page_dirty(page);
	f2fs_put_page(page, 1);
	return 0;
}

int f2fs_do_truncate_blocks(struct inode *inode, u64 from, bool lock)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct dnode_of_data dn;
	pgoff_t free_from;
	int count = 0, err = 0;
	struct page *ipage;
	bool truncate_page = false;

	trace_f2fs_truncate_blocks_enter(inode, from);

	free_from = (pgoff_t)F2FS_BLK_ALIGN(from);

	if (free_from >= max_file_blocks(inode))
		goto free_partial;

	if (lock)
		f2fs_lock_op(sbi);

	ipage = f2fs_get_node_page(sbi, inode->i_ino);
	if (IS_ERR(ipage)) {
		err = PTR_ERR(ipage);
		goto out;
	}

	if (f2fs_has_inline_data(inode)) {
		f2fs_truncate_inline_inode(inode, ipage, from);
		f2fs_put_page(ipage, 1);
		truncate_page = true;
		goto out;
	}

	set_new_dnode(&dn, inode, ipage, NULL, 0);
	err = f2fs_get_dnode_of_data(&dn, free_from, LOOKUP_NODE_RA);
	if (err) {
		if (err == -ENOENT)
			goto free_next;
		goto out;
	}

	count = ADDRS_PER_PAGE(dn.node_page, inode);

	count -= dn.ofs_in_node;
	f2fs_bug_on(sbi, count < 0);

	if (dn.ofs_in_node || IS_INODE(dn.node_page)) {
		f2fs_truncate_data_blocks_range(&dn, count);
		free_from += count;
	}

	f2fs_put_dnode(&dn);
free_next:
	err = f2fs_truncate_inode_blocks(inode, free_from);
out:
	if (lock)
		f2fs_unlock_op(sbi);
free_partial:
	/* lastly zero out the first data page */
	if (!err)
		err = truncate_partial_data_page(inode, from, truncate_page);

	trace_f2fs_truncate_blocks_exit(inode, err);
	return err;
}

int f2fs_truncate_blocks(struct inode *inode, u64 from, bool lock)
{
	u64 free_from = from;
	int err;

#ifdef CONFIG_F2FS_FS_COMPRESSION
	/*
	 * for compressed file, only support cluster size
	 * aligned truncation.
	 */
	if (f2fs_compressed_file(inode))
		free_from = round_up(from,
				F2FS_I(inode)->i_cluster_size << PAGE_SHIFT);
#endif

	err = f2fs_do_truncate_blocks(inode, free_from, lock);
	if (err)
		return err;

#ifdef CONFIG_F2FS_FS_COMPRESSION
	/*
	 * For compressed file, after release compress blocks, don't allow write
	 * direct, but we should allow write direct after truncate to zero.
	 */
	if (f2fs_compressed_file(inode) && !free_from
			&& is_inode_flag_set(inode, FI_COMPRESS_RELEASED))
		clear_inode_flag(inode, FI_COMPRESS_RELEASED);

	if (from != free_from) {
		err = f2fs_truncate_partial_cluster(inode, from, lock);
		if (err)
			return err;
	}
#endif

	return 0;
}

int f2fs_truncate(struct inode *inode)
{
	int err;

	if (unlikely(f2fs_cp_error(F2FS_I_SB(inode))))
		return -EIO;

	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
				S_ISLNK(inode->i_mode)))
		return 0;

	trace_f2fs_truncate(inode);

	if (time_to_inject(F2FS_I_SB(inode), FAULT_TRUNCATE))
		return -EIO;

	err = f2fs_dquot_initialize(inode);
	if (err)
		return err;

	/* we should check inline_data size */
	if (!f2fs_may_inline_data(inode)) {
		err = f2fs_convert_inline_inode(inode);
		if (err)
			return err;
	}

	err = f2fs_truncate_blocks(inode, i_size_read(inode), true);
	if (err)
		return err;

	inode->i_mtime = inode->i_ctime = current_time(inode);
	f2fs_mark_inode_dirty_sync(inode, false);
	return 0;
}

static bool f2fs_force_buffered_io(struct inode *inode, int rw)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	if (!fscrypt_dio_supported(inode))
		return true;
	if (fsverity_active(inode))
		return true;
	if (f2fs_compressed_file(inode))
		return true;
	if (f2fs_has_inline_data(inode))
		return true;

	/* disallow direct IO if any of devices has unaligned blksize */
	if (f2fs_is_multi_device(sbi) && !sbi->aligned_blksize)
		return true;
	/*
	 * for blkzoned device, fallback direct IO to buffered IO, so
	 * all IOs can be serialized by log-structured write.
	 */
	if (f2fs_sb_has_blkzoned(sbi) && (rw == WRITE))
		return true;
	if (f2fs_lfs_mode(sbi) && rw == WRITE && F2FS_IO_ALIGNED(sbi))
		return true;
	if (is_sbi_flag_set(sbi, SBI_CP_DISABLED))
		return true;

	return false;
}

int f2fs_getattr(struct user_namespace *mnt_userns, const struct path *path,
		 struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct f2fs_inode *ri = NULL;
	unsigned int flags;
#ifdef CONFIG_F2FS_FS_DEDUP
	struct inode *inner = NULL;
#endif

	if (f2fs_has_extra_attr(inode) &&
			f2fs_sb_has_inode_crtime(F2FS_I_SB(inode)) &&
			F2FS_FITS_IN_INODE(ri, fi->i_extra_isize, i_crtime)) {
		stat->result_mask |= STATX_BTIME;
		stat->btime.tv_sec = fi->i_crtime.tv_sec;
		stat->btime.tv_nsec = fi->i_crtime.tv_nsec;
	}

	/*
	 * Return the DIO alignment restrictions if requested.  We only return
	 * this information when requested, since on encrypted files it might
	 * take a fair bit of work to get if the file wasn't opened recently.
	 *
	 * f2fs sometimes supports DIO reads but not DIO writes.  STATX_DIOALIGN
	 * cannot represent that, so in that case we report no DIO support.
	 */
	if ((request_mask & STATX_DIOALIGN) && S_ISREG(inode->i_mode)) {
		unsigned int bsize = i_blocksize(inode);

		stat->result_mask |= STATX_DIOALIGN;
		if (!f2fs_force_buffered_io(inode, WRITE)) {
			stat->dio_mem_align = bsize;
			stat->dio_offset_align = bsize;
		}
	}

	flags = fi->i_flags;
	if ((flags & F2FS_COMPR_FL) && may_compress)
		stat->attributes |= STATX_ATTR_COMPRESSED;
	if (flags & F2FS_APPEND_FL)
		stat->attributes |= STATX_ATTR_APPEND;
	if (IS_ENCRYPTED(inode))
		stat->attributes |= STATX_ATTR_ENCRYPTED;
	if (flags & F2FS_IMMUTABLE_FL)
		stat->attributes |= STATX_ATTR_IMMUTABLE;
	if (flags & F2FS_NODUMP_FL)
		stat->attributes |= STATX_ATTR_NODUMP;
	if (flags & F2FS_NOCOMP_FL)
		stat->attributes |= STATX_ATTR_NOCOMPR;
	if (IS_VERITY(inode))
		stat->attributes |= STATX_ATTR_VERITY;

	stat->attributes_mask |= (STATX_ATTR_COMPRESSED |
				  STATX_ATTR_APPEND |
				  STATX_ATTR_ENCRYPTED |
				  STATX_ATTR_IMMUTABLE |
				  STATX_ATTR_NODUMP |
				  STATX_ATTR_NOCOMPR |
				  STATX_ATTR_VERITY);

	generic_fillattr(mnt_userns, inode, stat);

#ifdef CONFIG_F2FS_FS_DEDUP
	inner = get_inner_inode(inode);
	if (inner) {
		f2fs_down_read(&F2FS_I(inner)->i_sem);
		if (inner->i_nlink == 0)
			f2fs_bug_on(F2FS_I_SB(inode), 1);
		else
			stat->blocks = inner->i_blocks / inner->i_nlink;
		f2fs_up_read(&F2FS_I(inner)->i_sem);
	}
	put_inner_inode(inner);
#endif

	/* we need to show initial sectors used for inline_data/dentries */
	if ((S_ISREG(inode->i_mode) && f2fs_has_inline_data(inode)) ||
					f2fs_has_inline_dentry(inode))
		stat->blocks += (stat->size + 511) >> 9;

	return 0;
}

#ifdef CONFIG_F2FS_FS_POSIX_ACL
static void __setattr_copy(struct user_namespace *mnt_userns,
			   struct inode *inode, const struct iattr *attr)
{
	unsigned int ia_valid = attr->ia_valid;

	i_uid_update(mnt_userns, attr, inode);
	i_gid_update(mnt_userns, attr, inode);
	if (ia_valid & ATTR_ATIME)
		inode->i_atime = attr->ia_atime;
	if (ia_valid & ATTR_MTIME)
		inode->i_mtime = attr->ia_mtime;
	if (ia_valid & ATTR_CTIME)
		inode->i_ctime = attr->ia_ctime;
	if (ia_valid & ATTR_MODE) {
		umode_t mode = attr->ia_mode;
		vfsgid_t vfsgid = i_gid_into_vfsgid(mnt_userns, inode);

		if (!vfsgid_in_group_p(vfsgid) &&
		    !capable_wrt_inode_uidgid(mnt_userns, inode, CAP_FSETID))
			mode &= ~S_ISGID;
		set_acl_inode(inode, mode);
	}
}
#else
#define __setattr_copy setattr_copy
#endif

int f2fs_setattr(struct user_namespace *mnt_userns, struct dentry *dentry,
		 struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int err;

	if (unlikely(f2fs_cp_error(F2FS_I_SB(inode))))
		return -EIO;

	if (unlikely(IS_IMMUTABLE(inode)))
		return -EPERM;

	if (unlikely(IS_APPEND(inode) &&
			(attr->ia_valid & (ATTR_MODE | ATTR_UID |
				  ATTR_GID | ATTR_TIMES_SET))))
		return -EPERM;

	if ((attr->ia_valid & ATTR_SIZE) &&
		!f2fs_is_compress_backend_ready(inode))
		return -EOPNOTSUPP;

	err = setattr_prepare(mnt_userns, dentry, attr);
	if (err)
		return err;

	err = fscrypt_prepare_setattr(dentry, attr);
	if (err)
		return err;

	err = fsverity_prepare_setattr(dentry, attr);
	if (err)
		return err;

	if (is_quota_modification(mnt_userns, inode, attr)) {
		err = f2fs_dquot_initialize(inode);
		if (err)
			return err;
	}
	if (i_uid_needs_update(mnt_userns, attr, inode) ||
	    i_gid_needs_update(mnt_userns, attr, inode)) {
		f2fs_lock_op(F2FS_I_SB(inode));
		err = dquot_transfer(mnt_userns, inode, attr);
		if (err) {
			set_sbi_flag(F2FS_I_SB(inode),
					SBI_QUOTA_NEED_REPAIR);
			f2fs_unlock_op(F2FS_I_SB(inode));
			return err;
		}
		/*
		 * update uid/gid under lock_op(), so that dquot and inode can
		 * be updated atomically.
		 */
		i_uid_update(mnt_userns, attr, inode);
		i_gid_update(mnt_userns, attr, inode);
		f2fs_mark_inode_dirty_sync(inode, true);
		f2fs_unlock_op(F2FS_I_SB(inode));
	}

	if (attr->ia_valid & ATTR_SIZE) {
		loff_t old_size = i_size_read(inode);

		if (attr->ia_size > MAX_INLINE_DATA(inode)) {
			/*
			 * should convert inline inode before i_size_write to
			 * keep smaller than inline_data size with inline flag.
			 */
			err = f2fs_convert_inline_inode(inode);
			if (err)
				return err;
		}

#ifdef CONFIG_F2FS_FS_COMPRESSION_FIXED_OUTPUT
		CLEAR_IFLAG_IF_SET(inode, F2FS_NOCOMP_FL);
		if (attr->ia_size <= old_size && f2fs_compressed_file(inode) &&
		    is_inode_flag_set(inode, FI_COMPRESS_RELEASED)) {
			err = f2fs_reserve_compress_blocks(inode, NULL);
			if (err < 0)
				return err;
		}
#endif

#ifdef CONFIG_F2FS_FS_DEDUP
		/*
		 * caller have hold inode lock
		 */
		if (is_inode_flag_set(inode, FI_SNAPSHOTED))
			return -EOPNOTSUPP;
		if (attr->ia_size <= old_size && f2fs_is_outer_inode(inode)) {
			mark_file_modified(inode);
			err = f2fs_revoke_deduped_inode(inode, __func__);
			if (err)
				return err;
		}
#endif

		f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
		filemap_invalidate_lock(inode->i_mapping);

		truncate_setsize(inode, attr->ia_size);

		if (attr->ia_size <= old_size)
			err = f2fs_truncate(inode);
		/*
		 * do not trim all blocks after i_size if target size is
		 * larger than i_size.
		 */
		filemap_invalidate_unlock(inode->i_mapping);
		f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
		if (err)
			return err;

		spin_lock(&F2FS_I(inode)->i_size_lock);
		inode->i_mtime = inode->i_ctime = current_time(inode);
		F2FS_I(inode)->last_disk_size = i_size_read(inode);
		spin_unlock(&F2FS_I(inode)->i_size_lock);
	}

	__setattr_copy(mnt_userns, inode, attr);

	if (attr->ia_valid & ATTR_MODE) {
		err = posix_acl_chmod(mnt_userns, inode, f2fs_get_inode_mode(inode));

		if (is_inode_flag_set(inode, FI_ACL_MODE)) {
			if (!err)
				inode->i_mode = F2FS_I(inode)->i_acl_mode;
			clear_inode_flag(inode, FI_ACL_MODE);
		}
	}

	/* file size may changed here */
	f2fs_mark_inode_dirty_sync(inode, true);

	/* inode change will produce dirty node pages flushed by checkpoint */
	f2fs_balance_fs(F2FS_I_SB(inode), true);

	return err;
}

const struct inode_operations f2fs_file_inode_operations = {
	.getattr	= f2fs_getattr,
	.setattr	= f2fs_setattr,
	.get_acl	= f2fs_get_acl,
	.set_acl	= f2fs_set_acl,
	.listxattr	= f2fs_listxattr,
	.fiemap		= f2fs_fiemap,
	.fileattr_get	= f2fs_fileattr_get,
	.fileattr_set	= f2fs_fileattr_set,
};

static int fill_zero(struct inode *inode, pgoff_t index,
					loff_t start, loff_t len)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct page *page;

	if (!len)
		return 0;

	f2fs_balance_fs(sbi, true);

	f2fs_lock_op(sbi);
	page = f2fs_get_new_data_page(inode, NULL, index, false);
	f2fs_unlock_op(sbi);

	if (IS_ERR(page))
		return PTR_ERR(page);

	f2fs_wait_on_page_writeback(page, DATA, true, true);
	zero_user(page, start, len);
	set_page_dirty(page);
	f2fs_put_page(page, 1);
	return 0;
}

int f2fs_truncate_hole(struct inode *inode, pgoff_t pg_start, pgoff_t pg_end)
{
	int err;

	while (pg_start < pg_end) {
		struct dnode_of_data dn;
		pgoff_t end_offset, count;

		set_new_dnode(&dn, inode, NULL, NULL, 0);
		err = f2fs_get_dnode_of_data(&dn, pg_start, LOOKUP_NODE);
		if (err) {
			if (err == -ENOENT) {
				pg_start = f2fs_get_next_page_offset(&dn,
								pg_start);
				continue;
			}
			return err;
		}

		end_offset = ADDRS_PER_PAGE(dn.node_page, inode);
		count = min(end_offset - dn.ofs_in_node, pg_end - pg_start);

		f2fs_bug_on(F2FS_I_SB(inode), count == 0 || count > end_offset);

		f2fs_truncate_data_blocks_range(&dn, count);
		f2fs_put_dnode(&dn);

		pg_start += count;
	}
	return 0;
}

static int f2fs_punch_hole(struct inode *inode, loff_t offset, loff_t len)
{
	pgoff_t pg_start, pg_end;
	loff_t off_start, off_end;
	int ret;

	ret = f2fs_convert_inline_inode(inode);
	if (ret)
		return ret;

	pg_start = ((unsigned long long) offset) >> PAGE_SHIFT;
	pg_end = ((unsigned long long) offset + len) >> PAGE_SHIFT;

	off_start = offset & (PAGE_SIZE - 1);
	off_end = (offset + len) & (PAGE_SIZE - 1);

	if (pg_start == pg_end) {
		ret = fill_zero(inode, pg_start, off_start,
						off_end - off_start);
		if (ret)
			return ret;
	} else {
		if (off_start) {
			ret = fill_zero(inode, pg_start++, off_start,
						PAGE_SIZE - off_start);
			if (ret)
				return ret;
		}
		if (off_end) {
			ret = fill_zero(inode, pg_end, 0, off_end);
			if (ret)
				return ret;
		}

		if (pg_start < pg_end) {
			loff_t blk_start, blk_end;
			struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

			f2fs_balance_fs(sbi, true);

			blk_start = (loff_t)pg_start << PAGE_SHIFT;
			blk_end = (loff_t)pg_end << PAGE_SHIFT;

			f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
			filemap_invalidate_lock(inode->i_mapping);

			truncate_pagecache_range(inode, blk_start, blk_end - 1);

			f2fs_lock_op(sbi);
			ret = f2fs_truncate_hole(inode, pg_start, pg_end);
			f2fs_unlock_op(sbi);

			filemap_invalidate_unlock(inode->i_mapping);
			f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
		}
	}

	return ret;
}

static int __read_out_blkaddrs(struct inode *inode, block_t *blkaddr,
				int *do_replace, pgoff_t off, pgoff_t len)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct dnode_of_data dn;
	int ret, done, i;

next_dnode:
	set_new_dnode(&dn, inode, NULL, NULL, 0);
	ret = f2fs_get_dnode_of_data(&dn, off, LOOKUP_NODE_RA);
	if (ret && ret != -ENOENT) {
		return ret;
	} else if (ret == -ENOENT) {
		if (dn.max_level == 0)
			return -ENOENT;
		done = min((pgoff_t)ADDRS_PER_BLOCK(inode) -
						dn.ofs_in_node, len);
		blkaddr += done;
		do_replace += done;
		goto next;
	}

	done = min((pgoff_t)ADDRS_PER_PAGE(dn.node_page, inode) -
							dn.ofs_in_node, len);
	for (i = 0; i < done; i++, blkaddr++, do_replace++, dn.ofs_in_node++) {
		*blkaddr = f2fs_data_blkaddr(&dn);

		if (__is_valid_data_blkaddr(*blkaddr) &&
			!f2fs_is_valid_blkaddr(sbi, *blkaddr,
					DATA_GENERIC_ENHANCE)) {
			f2fs_put_dnode(&dn);
			f2fs_handle_error(sbi, ERROR_INVALID_BLKADDR);
			return -EFSCORRUPTED;
		}

		if (!f2fs_is_checkpointed_data(sbi, *blkaddr)) {

			if (f2fs_lfs_mode(sbi)) {
				f2fs_put_dnode(&dn);
				return -EOPNOTSUPP;
			}

			/* do not invalidate this block address */
			f2fs_update_data_blkaddr(&dn, NULL_ADDR);
			*do_replace = 1;
		}
	}
	f2fs_put_dnode(&dn);
next:
	len -= done;
	off += done;
	if (len)
		goto next_dnode;
	return 0;
}

static int __roll_back_blkaddrs(struct inode *inode, block_t *blkaddr,
				int *do_replace, pgoff_t off, int len)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct dnode_of_data dn;
	int ret, i;

	for (i = 0; i < len; i++, do_replace++, blkaddr++) {
		if (*do_replace == 0)
			continue;

		set_new_dnode(&dn, inode, NULL, NULL, 0);
		ret = f2fs_get_dnode_of_data(&dn, off + i, LOOKUP_NODE_RA);
		if (ret) {
			dec_valid_block_count(sbi, inode, 1);
			f2fs_invalidate_blocks(sbi, *blkaddr);
		} else {
			f2fs_update_data_blkaddr(&dn, *blkaddr);
		}
		f2fs_put_dnode(&dn);
	}
	return 0;
}

static int __clone_blkaddrs(struct inode *src_inode, struct inode *dst_inode,
			block_t *blkaddr, int *do_replace,
			pgoff_t src, pgoff_t dst, pgoff_t len, bool full)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(src_inode);
	pgoff_t i = 0;
	int ret;

	while (i < len) {
		if (blkaddr[i] == NULL_ADDR && !full) {
			i++;
			continue;
		}

		if (do_replace[i] || blkaddr[i] == NULL_ADDR) {
			struct dnode_of_data dn;
			struct node_info ni;
			size_t new_size;
			pgoff_t ilen;

			set_new_dnode(&dn, dst_inode, NULL, NULL, 0);
			ret = f2fs_get_dnode_of_data(&dn, dst + i, ALLOC_NODE);
			if (ret)
				return ret;

			ret = f2fs_get_node_info(sbi, dn.nid, &ni, false);
			if (ret) {
				f2fs_put_dnode(&dn);
				return ret;
			}

			ilen = min((pgoff_t)
				ADDRS_PER_PAGE(dn.node_page, dst_inode) -
						dn.ofs_in_node, len - i);
			do {
				dn.data_blkaddr = f2fs_data_blkaddr(&dn);
				f2fs_truncate_data_blocks_range(&dn, 1);

				if (do_replace[i]) {
					f2fs_i_blocks_write(src_inode,
							1, false, false);
					f2fs_i_blocks_write(dst_inode,
							1, true, false);
					f2fs_replace_block(sbi, &dn, dn.data_blkaddr,
					blkaddr[i], ni.version, true, false);

					do_replace[i] = 0;
				}
				dn.ofs_in_node++;
				i++;
				new_size = (loff_t)(dst + i) << PAGE_SHIFT;
				if (dst_inode->i_size < new_size)
					f2fs_i_size_write(dst_inode, new_size);
			} while (--ilen && (do_replace[i] || blkaddr[i] == NULL_ADDR));

			f2fs_put_dnode(&dn);
		} else {
			struct page *psrc, *pdst;

			psrc = f2fs_get_lock_data_page(src_inode,
							src + i, true);
			if (IS_ERR(psrc))
				return PTR_ERR(psrc);
			pdst = f2fs_get_new_data_page(dst_inode, NULL, dst + i,
								true);
			if (IS_ERR(pdst)) {
				f2fs_put_page(psrc, 1);
				return PTR_ERR(pdst);
			}
			memcpy_page(pdst, 0, psrc, 0, PAGE_SIZE);
			set_page_dirty(pdst);
			f2fs_put_page(pdst, 1);
			f2fs_put_page(psrc, 1);

			ret = f2fs_truncate_hole(src_inode,
						src + i, src + i + 1);
			if (ret)
				return ret;
			i++;
		}
	}
	return 0;
}

static int __exchange_data_block(struct inode *src_inode,
			struct inode *dst_inode, pgoff_t src, pgoff_t dst,
			pgoff_t len, bool full)
{
	block_t *src_blkaddr;
	int *do_replace;
	pgoff_t olen;
	int ret;

	while (len) {
		olen = min((pgoff_t)4 * ADDRS_PER_BLOCK(src_inode), len);

		src_blkaddr = f2fs_kvzalloc(F2FS_I_SB(src_inode),
					array_size(olen, sizeof(block_t)),
					GFP_NOFS);
		if (!src_blkaddr)
			return -ENOMEM;

		do_replace = f2fs_kvzalloc(F2FS_I_SB(src_inode),
					array_size(olen, sizeof(int)),
					GFP_NOFS);
		if (!do_replace) {
			kvfree(src_blkaddr);
			return -ENOMEM;
		}

		ret = __read_out_blkaddrs(src_inode, src_blkaddr,
					do_replace, src, olen);
		if (ret)
			goto roll_back;

		ret = __clone_blkaddrs(src_inode, dst_inode, src_blkaddr,
					do_replace, src, dst, olen, full);
		if (ret)
			goto roll_back;

		src += olen;
		dst += olen;
		len -= olen;

		kvfree(src_blkaddr);
		kvfree(do_replace);
	}
	return 0;

roll_back:
	__roll_back_blkaddrs(src_inode, src_blkaddr, do_replace, src, olen);
	kvfree(src_blkaddr);
	kvfree(do_replace);
	return ret;
}

static int f2fs_do_collapse(struct inode *inode, loff_t offset, loff_t len)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	pgoff_t nrpages = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	pgoff_t start = offset >> PAGE_SHIFT;
	pgoff_t end = (offset + len) >> PAGE_SHIFT;
	int ret;

	f2fs_balance_fs(sbi, true);

	/* avoid gc operation during block exchange */
	f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
	filemap_invalidate_lock(inode->i_mapping);

	f2fs_lock_op(sbi);
	f2fs_drop_extent_tree(inode);
	truncate_pagecache(inode, offset);
	ret = __exchange_data_block(inode, inode, end, start, nrpages - end, true);
	f2fs_unlock_op(sbi);

	filemap_invalidate_unlock(inode->i_mapping);
	f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
	return ret;
}

static int f2fs_collapse_range(struct inode *inode, loff_t offset, loff_t len)
{
	loff_t new_size;
	int ret;

	if (offset + len >= i_size_read(inode))
		return -EINVAL;

	/* collapse range should be aligned to block size of f2fs. */
	if (offset & (F2FS_BLKSIZE - 1) || len & (F2FS_BLKSIZE - 1))
		return -EINVAL;

	ret = f2fs_convert_inline_inode(inode);
	if (ret)
		return ret;

	/* write out all dirty pages from offset */
	ret = filemap_write_and_wait_range(inode->i_mapping, offset, LLONG_MAX);
	if (ret)
		return ret;

	ret = f2fs_do_collapse(inode, offset, len);
	if (ret)
		return ret;

	/* write out all moved pages, if possible */
	filemap_invalidate_lock(inode->i_mapping);
	filemap_write_and_wait_range(inode->i_mapping, offset, LLONG_MAX);
	truncate_pagecache(inode, offset);

	new_size = i_size_read(inode) - len;
	ret = f2fs_truncate_blocks(inode, new_size, true);
	filemap_invalidate_unlock(inode->i_mapping);
	if (!ret)
		f2fs_i_size_write(inode, new_size);
	return ret;
}

static int f2fs_do_zero_range(struct dnode_of_data *dn, pgoff_t start,
								pgoff_t end)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dn->inode);
	pgoff_t index = start;
	unsigned int ofs_in_node = dn->ofs_in_node;
	blkcnt_t count = 0;
	int ret;

	for (; index < end; index++, dn->ofs_in_node++) {
		if (f2fs_data_blkaddr(dn) == NULL_ADDR)
			count++;
	}

	dn->ofs_in_node = ofs_in_node;
	ret = f2fs_reserve_new_blocks(dn, count);
	if (ret)
		return ret;

	dn->ofs_in_node = ofs_in_node;
	for (index = start; index < end; index++, dn->ofs_in_node++) {
		dn->data_blkaddr = f2fs_data_blkaddr(dn);
		/*
		 * f2fs_reserve_new_blocks will not guarantee entire block
		 * allocation.
		 */
		if (dn->data_blkaddr == NULL_ADDR) {
			ret = -ENOSPC;
			break;
		}

		if (dn->data_blkaddr == NEW_ADDR)
			continue;

		if (!f2fs_is_valid_blkaddr(sbi, dn->data_blkaddr,
					DATA_GENERIC_ENHANCE)) {
			ret = -EFSCORRUPTED;
			f2fs_handle_error(sbi, ERROR_INVALID_BLKADDR);
			break;
		}

		f2fs_invalidate_blocks(sbi, dn->data_blkaddr);
#ifdef CONFIG_F2FS_FS_SEQZONE
		if (f2fs_seqzone_file(dn->inode))
			dn->seqzone_index = NULL_ADDR;
#endif
		f2fs_set_data_blkaddr(dn, NEW_ADDR);
	}

	f2fs_update_read_extent_cache_range(dn, start, 0, index - start);
	f2fs_update_age_extent_cache_range(dn, start, index - start);

	return ret;
}

static int f2fs_zero_range(struct inode *inode, loff_t offset, loff_t len,
								int mode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct address_space *mapping = inode->i_mapping;
	pgoff_t index, pg_start, pg_end;
	loff_t new_size = i_size_read(inode);
	loff_t off_start, off_end;
	int ret = 0;

	ret = inode_newsize_ok(inode, (len + offset));
	if (ret)
		return ret;

	ret = f2fs_convert_inline_inode(inode);
	if (ret)
		return ret;

	ret = filemap_write_and_wait_range(mapping, offset, offset + len - 1);
	if (ret)
		return ret;

	pg_start = ((unsigned long long) offset) >> PAGE_SHIFT;
	pg_end = ((unsigned long long) offset + len) >> PAGE_SHIFT;

	off_start = offset & (PAGE_SIZE - 1);
	off_end = (offset + len) & (PAGE_SIZE - 1);

	if (pg_start == pg_end) {
		ret = fill_zero(inode, pg_start, off_start,
						off_end - off_start);
		if (ret)
			return ret;

		new_size = max_t(loff_t, new_size, offset + len);
	} else {
		if (off_start) {
			ret = fill_zero(inode, pg_start++, off_start,
						PAGE_SIZE - off_start);
			if (ret)
				return ret;

			new_size = max_t(loff_t, new_size,
					(loff_t)pg_start << PAGE_SHIFT);
		}

		for (index = pg_start; index < pg_end;) {
			struct dnode_of_data dn;
			unsigned int end_offset;
			pgoff_t end;

			f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
			filemap_invalidate_lock(mapping);

			truncate_pagecache_range(inode,
				(loff_t)index << PAGE_SHIFT,
				((loff_t)pg_end << PAGE_SHIFT) - 1);

			f2fs_lock_op(sbi);

			set_new_dnode(&dn, inode, NULL, NULL, 0);
			ret = f2fs_get_dnode_of_data(&dn, index, ALLOC_NODE);
			if (ret) {
				f2fs_unlock_op(sbi);
				filemap_invalidate_unlock(mapping);
				f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
				goto out;
			}

			end_offset = ADDRS_PER_PAGE(dn.node_page, inode);
			end = min(pg_end, end_offset - dn.ofs_in_node + index);

			ret = f2fs_do_zero_range(&dn, index, end);
			f2fs_put_dnode(&dn);

			f2fs_unlock_op(sbi);
			filemap_invalidate_unlock(mapping);
			f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);

			f2fs_balance_fs(sbi, dn.node_changed);

			if (ret)
				goto out;

			index = end;
			new_size = max_t(loff_t, new_size,
					(loff_t)index << PAGE_SHIFT);
		}

		if (off_end) {
			ret = fill_zero(inode, pg_end, 0, off_end);
			if (ret)
				goto out;

			new_size = max_t(loff_t, new_size, offset + len);
		}
	}

out:
	if (new_size > i_size_read(inode)) {
		if (mode & FALLOC_FL_KEEP_SIZE)
			file_set_keep_isize(inode);
		else
			f2fs_i_size_write(inode, new_size);
	}
	return ret;
}

static int f2fs_insert_range(struct inode *inode, loff_t offset, loff_t len)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct address_space *mapping = inode->i_mapping;
	pgoff_t nr, pg_start, pg_end, delta, idx;
	loff_t new_size;
	int ret = 0;

	new_size = i_size_read(inode) + len;
	ret = inode_newsize_ok(inode, new_size);
	if (ret)
		return ret;

	if (offset >= i_size_read(inode))
		return -EINVAL;

	/* insert range should be aligned to block size of f2fs. */
	if (offset & (F2FS_BLKSIZE - 1) || len & (F2FS_BLKSIZE - 1))
		return -EINVAL;

	ret = f2fs_convert_inline_inode(inode);
	if (ret)
		return ret;

	f2fs_balance_fs(sbi, true);

	filemap_invalidate_lock(mapping);
	ret = f2fs_truncate_blocks(inode, i_size_read(inode), true);
	filemap_invalidate_unlock(mapping);
	if (ret)
		return ret;

	/* write out all dirty pages from offset */
	ret = filemap_write_and_wait_range(mapping, offset, LLONG_MAX);
	if (ret)
		return ret;

	pg_start = offset >> PAGE_SHIFT;
	pg_end = (offset + len) >> PAGE_SHIFT;
	delta = pg_end - pg_start;
	idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);

	/* avoid gc operation during block exchange */
	f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
	filemap_invalidate_lock(mapping);
	truncate_pagecache(inode, offset);

	while (!ret && idx > pg_start) {
		nr = idx - pg_start;
		if (nr > delta)
			nr = delta;
		idx -= nr;

		f2fs_lock_op(sbi);
		f2fs_drop_extent_tree(inode);

		ret = __exchange_data_block(inode, inode, idx,
					idx + delta, nr, false);
		f2fs_unlock_op(sbi);
	}
	filemap_invalidate_unlock(mapping);
	f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);

	/* write out all moved pages, if possible */
	filemap_invalidate_lock(mapping);
	filemap_write_and_wait_range(mapping, offset, LLONG_MAX);
	truncate_pagecache(inode, offset);
	filemap_invalidate_unlock(mapping);

	if (!ret)
		f2fs_i_size_write(inode, new_size);
	return ret;
}

static int f2fs_expand_inode_data(struct inode *inode, loff_t offset,
					loff_t len, int mode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_map_blocks map = { .m_next_pgofs = NULL,
			.m_next_extent = NULL, .m_seg_type = NO_CHECK_TYPE,
			.m_may_create = true };
	struct f2fs_gc_control gc_control = { .victim_segno = NULL_SEGNO,
			.init_gc_type = FG_GC,
			.should_migrate_blocks = false,
			.err_gc_skipped = true,
			.nr_free_secs = 0 };
	pgoff_t pg_start, pg_end;
	loff_t new_size;
	loff_t off_end;
	block_t expanded = 0;
	int err;

	err = inode_newsize_ok(inode, (len + offset));
	if (err)
		return err;

	err = f2fs_convert_inline_inode(inode);
	if (err)
		return err;

	f2fs_balance_fs(sbi, true);

	pg_start = ((unsigned long long)offset) >> PAGE_SHIFT;
	pg_end = ((unsigned long long)offset + len) >> PAGE_SHIFT;
	off_end = (offset + len) & (PAGE_SIZE - 1);

#ifdef CONFIG_F2FS_SEQZONE
	if (f2fs_seqzone_file(inode))
		map.m_seqblk = pg_start;
#endif
	map.m_lblk = pg_start;
	map.m_len = pg_end - pg_start;
	if (off_end)
		map.m_len++;

	if (!map.m_len)
		return 0;

	if (f2fs_is_pinned_file(inode)) {
		block_t sec_blks = CAP_BLKS_PER_SEC(sbi);
		block_t sec_len = roundup(map.m_len, sec_blks);

		map.m_len = sec_blks;
next_alloc:
		if (has_not_enough_free_secs(sbi, 0,
			GET_SEC_FROM_SEG(sbi, overprovision_segments(sbi)))) {
			f2fs_down_write(&sbi->gc_lock);
			err = f2fs_gc(sbi, &gc_control);
			if (err && err != -ENODATA)
				goto out_err;
		}

		f2fs_down_write(&sbi->pin_sem);

		f2fs_lock_op(sbi);
		f2fs_allocate_new_section(sbi, CURSEG_COLD_DATA_PINNED, false);
		f2fs_unlock_op(sbi);

		map.m_seg_type = CURSEG_COLD_DATA_PINNED;
		err = f2fs_map_blocks(inode, &map, F2FS_GET_BLOCK_PRE_DIO);
		file_dont_truncate(inode);

		f2fs_up_write(&sbi->pin_sem);

		expanded += map.m_len;
		sec_len -= map.m_len;
		map.m_lblk += map.m_len;
		if (!err && sec_len)
			goto next_alloc;

		map.m_len = expanded;
	} else {
		err = f2fs_map_blocks(inode, &map, F2FS_GET_BLOCK_PRE_AIO);
		expanded = map.m_len;
	}
out_err:
	if (err) {
		pgoff_t last_off;

		if (!expanded)
			return err;

		last_off = pg_start + expanded - 1;

		/* update new size to the failed position */
		new_size = (last_off == pg_end) ? offset + len :
					(loff_t)(last_off + 1) << PAGE_SHIFT;
	} else {
		new_size = ((loff_t)pg_end << PAGE_SHIFT) + off_end;
	}

	if (new_size > i_size_read(inode)) {
		if (mode & FALLOC_FL_KEEP_SIZE)
			file_set_keep_isize(inode);
		else
			f2fs_i_size_write(inode, new_size);
	}

	return err;
}

static long f2fs_fallocate(struct file *file, int mode,
				loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	long ret = 0;

	if (unlikely(f2fs_cp_error(F2FS_I_SB(inode))))
		return -EIO;
	if (!f2fs_is_checkpoint_ready(F2FS_I_SB(inode)))
		return -ENOSPC;
	if (!f2fs_is_compress_backend_ready(inode))
		return -EOPNOTSUPP;

	/* f2fs only support ->fallocate for regular file */
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	if (IS_ENCRYPTED(inode) &&
		(mode & (FALLOC_FL_COLLAPSE_RANGE | FALLOC_FL_INSERT_RANGE)))
		return -EOPNOTSUPP;

	/*
	 * Pinned file should not support partial truncation since the block
	 * can be used by applications.
	 */
	inode_lock(inode);
	if ((f2fs_compressed_file(inode) || f2fs_is_pinned_file(inode)) &&
		(mode & (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_COLLAPSE_RANGE |
			FALLOC_FL_ZERO_RANGE | FALLOC_FL_INSERT_RANGE))) {
#ifdef CONFIG_F2FS_FS_COMPRESSION_FIXED_OUTPUT
		if (f2fs_compressed_file(inode)) {
			CLEAR_IFLAG_IF_SET(inode, F2FS_NOCOMP_FL);
			if (is_inode_flag_set(inode, FI_COMPRESS_RELEASED)) {
				ret = f2fs_reserve_compress_blocks(inode, NULL);
				if (ret < 0)
					goto out;
			}
			ret = f2fs_decompress_inode(inode);
			if (ret < 0)
				goto out;
		} else {
			ret = -EOPNOTSUPP;
			goto out;
		}
#else
		ret = -EOPNOTSUPP;
		goto out;
#endif
	}

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |
			FALLOC_FL_COLLAPSE_RANGE | FALLOC_FL_ZERO_RANGE |
			FALLOC_FL_INSERT_RANGE)) {
		ret = -EOPNOTSUPP;
		goto out;
	}


#ifdef CONFIG_F2FS_SEQZONE
	if (f2fs_seqzone_file(inode) && (mode &
		(FALLOC_FL_COLLAPSE_RANGE | FALLOC_FL_INSERT_RANGE))) {
		ret = -EOPNOTSUPP;
		goto out;
	}
#endif
	ret = file_modified(file);
	if (ret)
		goto out;

#ifdef CONFIG_F2FS_FS_DEDUP
	mark_file_modified(inode);
	if (f2fs_is_outer_inode(inode) &&
			f2fs_revoke_deduped_inode(inode, __func__)) {
		ret = -EIO;
		goto out;
	}
#endif
	if (mode & FALLOC_FL_PUNCH_HOLE) {
		int i;
		if (offset >= inode->i_size)
			goto out;

		f2fs_info(F2FS_I_SB(inode), "punch ino %lu isize %lld offset %lld len %lld\n",
			inode->i_ino, i_size_read(inode), offset, len);
		for (i = 0; i < BITS_TO_LONGS(FI_MAX); i++)
			f2fs_info(F2FS_I_SB(inode), "flags[%d] %lx", i, F2FS_I(inode)->flags[i]);
		ret = f2fs_punch_hole(inode, offset, len);
	} else if (mode & FALLOC_FL_COLLAPSE_RANGE) {
		ret = f2fs_collapse_range(inode, offset, len);
	} else if (mode & FALLOC_FL_ZERO_RANGE) {
		int i;
		f2fs_info(F2FS_I_SB(inode), "zero ino %lu isize %lld offset %lld len %lld mode %d\n",
			inode->i_ino, i_size_read(inode), offset, len, mode);
		for (i = 0; i < BITS_TO_LONGS(FI_MAX); i++)
			f2fs_info(F2FS_I_SB(inode), "flags[%d] %lx", i, F2FS_I(inode)->flags[i]);
		ret = f2fs_zero_range(inode, offset, len, mode);
	} else if (mode & FALLOC_FL_INSERT_RANGE) {
		ret = f2fs_insert_range(inode, offset, len);
	} else {
		ret = f2fs_expand_inode_data(inode, offset, len, mode);
	}

#ifdef CONFIG_F2FS_APPBOOST
	/* file change, update mtime */
	inode->i_mtime = inode->i_ctime = current_time(inode);
	f2fs_mark_inode_dirty_sync(inode, false);
	f2fs_update_time(F2FS_I_SB(inode), REQ_TIME);
#else
	if (!ret) {
		inode->i_mtime = inode->i_ctime = current_time(inode);
		f2fs_mark_inode_dirty_sync(inode, false);
		f2fs_update_time(F2FS_I_SB(inode), REQ_TIME);
	}
#endif
out:
	inode_unlock(inode);

	trace_f2fs_fallocate(inode, mode, offset, len, ret);
	return ret;
}

static int f2fs_release_file(struct inode *inode, struct file *filp)
{
#ifdef CONFIG_F2FS_FS_DEDUP
	struct inode *inner = NULL;
#endif
	/*
	 * f2fs_release_file is called at every close calls. So we should
	 * not drop any inmemory pages by close called by other process.
	 */
	if (!(filp->f_mode & FMODE_WRITE) ||
			atomic_read(&inode->i_writecount) != 1)
		return 0;

	inode_lock(inode);
	f2fs_abort_atomic_write(inode, true);
	inode_unlock(inode);

#ifdef CONFIG_F2FS_FS_DEDUP
	if (f2fs_is_outer_inode(inode)) {
		inner = get_inner_inode(inode);
		if (inner)
			f2fs_release_file(inner, filp);
		put_inner_inode(inner);
	}
#endif
	return 0;
}

static int f2fs_file_flush(struct file *file, fl_owner_t id)
{
	struct inode *inode = file_inode(file);

	/*
	 * If the process doing a transaction is crashed, we should do
	 * roll-back. Otherwise, other reader/write can see corrupted database
	 * until all the writers close its file. Since this should be done
	 * before dropping file lock, it needs to do in ->flush.
	 */
	if (F2FS_I(inode)->atomic_write_task == current &&
				(current->flags & PF_EXITING)) {
		inode_lock(inode);
		f2fs_abort_atomic_write(inode, true);
		inode_unlock(inode);
	}

	return 0;
}

static int f2fs_setflags_common(struct inode *inode, u32 iflags, u32 mask)
{
	struct f2fs_inode_info *fi = F2FS_I(inode);
	u32 masked_flags = fi->i_flags & mask;

	/* mask can be shrunk by flags_valid selector */
	iflags &= mask;

	/* Is it quota file? Do not allow user to mess with it */
	if (IS_NOQUOTA(inode))
		return -EPERM;

	if ((iflags ^ masked_flags) & F2FS_CASEFOLD_FL) {
		if (!f2fs_sb_has_casefold(F2FS_I_SB(inode)))
			return -EOPNOTSUPP;
		if (!f2fs_empty_dir(inode))
			return -ENOTEMPTY;
	}

	if (iflags & (F2FS_COMPR_FL | F2FS_NOCOMP_FL)) {
		if (!f2fs_sb_has_compression(F2FS_I_SB(inode)))
			return -EOPNOTSUPP;
		if ((iflags & F2FS_COMPR_FL) && (iflags & F2FS_NOCOMP_FL))
			return -EINVAL;
	}

	if ((iflags ^ masked_flags) & F2FS_COMPR_FL) {
		if (masked_flags & F2FS_COMPR_FL) {
			if (!f2fs_disable_compressed_file(inode))
				return -EINVAL;
		} else {
			/* try to convert inline_data to support compression */
			int err = f2fs_convert_inline_inode(inode);
			if (err)
				return err;

			f2fs_down_write(&F2FS_I(inode)->i_sem);
			if (!f2fs_may_compress(inode) ||
					(S_ISREG(inode->i_mode) &&
					F2FS_HAS_BLOCKS(inode))) {
				f2fs_up_write(&F2FS_I(inode)->i_sem);
				return -EINVAL;
			}
#ifdef CONFIG_F2FS_SEQZONE
			if (f2fs_seqzone_file(inode)) {
				f2fs_up_write(&F2FS_I(inode)->i_sem);
				return -EINVAL;
			}
#endif
			if (!may_set_compr_fl) {
				f2fs_up_write(&F2FS_I(inode)->i_sem);
				return -EOPNOTSUPP;
			}
			err = set_compress_context(inode);
			f2fs_up_write(&F2FS_I(inode)->i_sem);

			if (err)
				return err;
		}
	}

	fi->i_flags = iflags | (fi->i_flags & ~mask);
	f2fs_bug_on(F2FS_I_SB(inode), (fi->i_flags & F2FS_COMPR_FL) &&
					(fi->i_flags & F2FS_NOCOMP_FL));

	if (fi->i_flags & F2FS_PROJINHERIT_FL)
		set_inode_flag(inode, FI_PROJ_INHERIT);
	else
		clear_inode_flag(inode, FI_PROJ_INHERIT);

	inode->i_ctime = current_time(inode);
	f2fs_set_inode_flags(inode);
	f2fs_mark_inode_dirty_sync(inode, true);
	return 0;
}

/* FS_IOC_[GS]ETFLAGS and FS_IOC_FS[GS]ETXATTR support */

/*
 * To make a new on-disk f2fs i_flag gettable via FS_IOC_GETFLAGS, add an entry
 * for it to f2fs_fsflags_map[], and add its FS_*_FL equivalent to
 * F2FS_GETTABLE_FS_FL.  To also make it settable via FS_IOC_SETFLAGS, also add
 * its FS_*_FL equivalent to F2FS_SETTABLE_FS_FL.
 *
 * Translating flags to fsx_flags value used by FS_IOC_FSGETXATTR and
 * FS_IOC_FSSETXATTR is done by the VFS.
 */

static const struct {
	u32 iflag;
	u32 fsflag;
} f2fs_fsflags_map[] = {
	{ F2FS_COMPR_FL,	FS_COMPR_FL },
	{ F2FS_SYNC_FL,		FS_SYNC_FL },
	{ F2FS_IMMUTABLE_FL,	FS_IMMUTABLE_FL },
	{ F2FS_APPEND_FL,	FS_APPEND_FL },
	{ F2FS_NODUMP_FL,	FS_NODUMP_FL },
	{ F2FS_NOATIME_FL,	FS_NOATIME_FL },
	{ F2FS_NOCOMP_FL,	FS_NOCOMP_FL },
	{ F2FS_INDEX_FL,	FS_INDEX_FL },
	{ F2FS_DIRSYNC_FL,	FS_DIRSYNC_FL },
	{ F2FS_PROJINHERIT_FL,	FS_PROJINHERIT_FL },
	{ F2FS_CASEFOLD_FL,	FS_CASEFOLD_FL },
};

#define F2FS_GETTABLE_FS_FL (		\
		FS_COMPR_FL |		\
		FS_SYNC_FL |		\
		FS_IMMUTABLE_FL |	\
		FS_APPEND_FL |		\
		FS_NODUMP_FL |		\
		FS_NOATIME_FL |		\
		FS_NOCOMP_FL |		\
		FS_INDEX_FL |		\
		FS_DIRSYNC_FL |		\
		FS_PROJINHERIT_FL |	\
		FS_ENCRYPT_FL |		\
		FS_INLINE_DATA_FL |	\
		FS_NOCOW_FL |		\
		FS_VERITY_FL |		\
		FS_CASEFOLD_FL)

#define F2FS_SETTABLE_FS_FL (		\
		FS_COMPR_FL |		\
		FS_SYNC_FL |		\
		FS_IMMUTABLE_FL |	\
		FS_APPEND_FL |		\
		FS_NODUMP_FL |		\
		FS_NOATIME_FL |		\
		FS_NOCOMP_FL |		\
		FS_DIRSYNC_FL |		\
		FS_PROJINHERIT_FL |	\
		FS_CASEFOLD_FL)

/* Convert f2fs on-disk i_flags to FS_IOC_{GET,SET}FLAGS flags */
static inline u32 f2fs_iflags_to_fsflags(u32 iflags)
{
	u32 fsflags = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(f2fs_fsflags_map); i++)
		if (iflags & f2fs_fsflags_map[i].iflag)
			fsflags |= f2fs_fsflags_map[i].fsflag;

	return fsflags;
}

/* Convert FS_IOC_{GET,SET}FLAGS flags to f2fs on-disk i_flags */
static inline u32 f2fs_fsflags_to_iflags(u32 fsflags)
{
	u32 iflags = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(f2fs_fsflags_map); i++)
		if (fsflags & f2fs_fsflags_map[i].fsflag)
			iflags |= f2fs_fsflags_map[i].iflag;

	return iflags;
}

static int f2fs_ioc_getversion(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);

	return put_user(inode->i_generation, (int __user *)arg);
}

static int f2fs_ioc_start_atomic_write(struct file *filp, bool truncate)
{
	struct inode *inode = file_inode(filp);
	struct user_namespace *mnt_userns = file_mnt_user_ns(filp);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	loff_t isize;
	int ret;

	if (!(filp->f_mode & FMODE_WRITE))
		return -EBADF;

	if (!inode_owner_or_capable(mnt_userns, inode))
		return -EACCES;

	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	if (filp->f_flags & O_DIRECT)
		return -EINVAL;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	inode_lock(inode);

#ifdef CONFIG_F2FS_FS_COMPRESSION_FIXED_OUTPUT
	if (f2fs_compressed_file(inode)) {
		CLEAR_IFLAG_IF_SET(inode, F2FS_NOCOMP_FL);
		if (is_inode_flag_set(inode, FI_COMPRESS_RELEASED)) {
			ret = f2fs_reserve_compress_blocks(inode, NULL);
			if (ret < 0)
				goto out;
		}
		ret = f2fs_decompress_inode(inode);
		if (ret < 0)
			goto out;
	}
#endif

#ifdef CONFIG_F2FS_FS_DEDUP
	mark_file_modified(inode);
	if (f2fs_is_outer_inode(inode)) {
		ret = f2fs_revoke_deduped_inode(inode, __func__);
		if (ret)
			goto out;
	}
#endif

	if (!f2fs_disable_compressed_file(inode)) {
		ret = -EINVAL;
		goto out;
	}

	if (f2fs_is_atomic_file(inode))
		goto out;

	ret = f2fs_convert_inline_inode(inode);
	if (ret)
		goto out;

	f2fs_down_write(&fi->i_gc_rwsem[WRITE]);

	/*
	 * Should wait end_io to count F2FS_WB_CP_DATA correctly by
	 * f2fs_is_atomic_file.
	 */
	if (get_dirty_pages(inode))
		f2fs_warn(sbi, "Unexpected flush for atomic writes: ino=%lu, npages=%u",
			  inode->i_ino, get_dirty_pages(inode));
	ret = filemap_write_and_wait_range(inode->i_mapping, 0, LLONG_MAX);
	if (ret) {
		f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
		goto out;
	}

	/* Check if the inode already has a COW inode */
	if (fi->cow_inode == NULL) {
		/* Create a COW inode for atomic write */
		struct dentry *dentry = file_dentry(filp);
		struct inode *dir = d_inode(dentry->d_parent);

		ret = f2fs_get_tmpfile(mnt_userns, dir, &fi->cow_inode);
		if (ret) {
			f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
			goto out;
		}

		set_inode_flag(fi->cow_inode, FI_COW_FILE);
		clear_inode_flag(fi->cow_inode, FI_INLINE_DATA);

		/* Set the COW inode's atomic_inode to the atomic inode */
		F2FS_I(fi->cow_inode)->atomic_inode = inode;
	} else {
		/* Reuse the already created COW inode */
		f2fs_bug_on(sbi, get_dirty_pages(fi->cow_inode));

		invalidate_mapping_pages(fi->cow_inode->i_mapping, 0, -1);

		ret = f2fs_do_truncate_blocks(fi->cow_inode, 0, true);
		if (ret) {
			f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
			goto out;
		}
	}

	f2fs_write_inode(inode, NULL);

	stat_inc_atomic_inode(inode);

#ifdef CONFIG_F2FS_SEQZONE
	if (f2fs_sb_has_seqzone(sbi) &&
		IS_ENCRYPTED(inode) && !f2fs_compressed_file(inode) &&
		f2fs_seqzone_file(inode))
		if (f2fs_inode_support_dedup(sbi, inode))
			set_inode_flag(fi->cow_inode, FI_SEQZONE);
#endif
	set_inode_flag(inode, FI_ATOMIC_FILE);

	isize = i_size_read(inode);
	fi->original_i_size = isize;
	if (truncate) {
		set_inode_flag(inode, FI_ATOMIC_REPLACE);
		truncate_inode_pages_final(inode->i_mapping);
		f2fs_i_size_write(inode, 0);
		isize = 0;
	}
	f2fs_i_size_write(fi->cow_inode, isize);

	f2fs_up_write(&fi->i_gc_rwsem[WRITE]);

	f2fs_update_time(sbi, REQ_TIME);
	fi->atomic_write_task = current;
	stat_update_max_atomic_write(inode);
	fi->atomic_write_cnt = 0;
out:
	inode_unlock(inode);
	mnt_drop_write_file(filp);
	return ret;
}

static int f2fs_ioc_commit_atomic_write(struct file *filp)
{
	struct inode *inode = file_inode(filp);
	struct user_namespace *mnt_userns = file_mnt_user_ns(filp);
	int ret;

	if (!(filp->f_mode & FMODE_WRITE))
		return -EBADF;

	if (!inode_owner_or_capable(mnt_userns, inode))
		return -EACCES;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	f2fs_balance_fs(F2FS_I_SB(inode), true);

	inode_lock(inode);

	if (f2fs_is_atomic_file(inode)) {
		ret = f2fs_commit_atomic_write(inode);
		if (!ret)
			ret = f2fs_do_sync_file(filp, 0, LLONG_MAX, 0, true);

		f2fs_abort_atomic_write(inode, ret);
	} else {
		ret = f2fs_do_sync_file(filp, 0, LLONG_MAX, 1, false);
	}

	inode_unlock(inode);
	mnt_drop_write_file(filp);
	return ret;
}

static int f2fs_ioc_abort_atomic_write(struct file *filp)
{
	struct inode *inode = file_inode(filp);
	struct user_namespace *mnt_userns = file_mnt_user_ns(filp);
	int ret;

	if (!(filp->f_mode & FMODE_WRITE))
		return -EBADF;

	if (!inode_owner_or_capable(mnt_userns, inode))
		return -EACCES;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	inode_lock(inode);

	f2fs_abort_atomic_write(inode, true);

	inode_unlock(inode);

	mnt_drop_write_file(filp);
	f2fs_update_time(F2FS_I_SB(inode), REQ_TIME);
	return ret;
}

static int f2fs_ioc_shutdown(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct super_block *sb = sbi->sb;
	__u32 in;
	int ret = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (get_user(in, (__u32 __user *)arg))
		return -EFAULT;

	if (in != F2FS_GOING_DOWN_FULLSYNC) {
		ret = mnt_want_write_file(filp);
		if (ret) {
			if (ret == -EROFS) {
				ret = 0;
				f2fs_stop_checkpoint(sbi, false,
						STOP_CP_REASON_SHUTDOWN);
				set_sbi_flag(sbi, SBI_IS_SHUTDOWN);
				trace_f2fs_shutdown(sbi, in, ret);
			}
			return ret;
		}
	}

	switch (in) {
	case F2FS_GOING_DOWN_FULLSYNC:
		ret = freeze_bdev(sb->s_bdev);
		if (ret)
			goto out;
		f2fs_stop_checkpoint(sbi, false, STOP_CP_REASON_SHUTDOWN);
		set_sbi_flag(sbi, SBI_IS_SHUTDOWN);
		thaw_bdev(sb->s_bdev);
		break;
	case F2FS_GOING_DOWN_METASYNC:
		/* do checkpoint only */
		ret = f2fs_sync_fs(sb, 1);
		if (ret) {
			if (ret == -EIO)
				ret = 0;
			goto out;
		}
		f2fs_stop_checkpoint(sbi, false, STOP_CP_REASON_SHUTDOWN);
		set_sbi_flag(sbi, SBI_IS_SHUTDOWN);
		break;
	case F2FS_GOING_DOWN_NOSYNC:
		f2fs_stop_checkpoint(sbi, false, STOP_CP_REASON_SHUTDOWN);
		set_sbi_flag(sbi, SBI_IS_SHUTDOWN);
		break;
	case F2FS_GOING_DOWN_METAFLUSH:
		f2fs_sync_meta_pages(sbi, META, LONG_MAX, FS_META_IO);
		f2fs_stop_checkpoint(sbi, false, STOP_CP_REASON_SHUTDOWN);
		set_sbi_flag(sbi, SBI_IS_SHUTDOWN);
		break;
	case F2FS_GOING_DOWN_NEED_FSCK:
		set_sbi_flag(sbi, SBI_NEED_FSCK);
		set_sbi_flag(sbi, SBI_CP_DISABLED_QUICK);
		set_sbi_flag(sbi, SBI_IS_DIRTY);
		/* do checkpoint only */
		ret = f2fs_sync_fs(sb, 1);
		if (ret == -EIO)
			ret = 0;
		goto out;
	default:
		ret = -EINVAL;
		goto out;
	}

	f2fs_stop_gc_thread(sbi);
	f2fs_stop_discard_thread(sbi);

	f2fs_drop_discard_cmd(sbi);
	clear_opt(sbi, DISCARD);

	f2fs_update_time(sbi, REQ_TIME);
out:
	if (in != F2FS_GOING_DOWN_FULLSYNC)
		mnt_drop_write_file(filp);

	trace_f2fs_shutdown(sbi, in, ret);

	return ret;
}

static int f2fs_ioc_fitrim(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct super_block *sb = inode->i_sb;
	struct fstrim_range range;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (!f2fs_hw_support_discard(F2FS_SB(sb)))
		return -EOPNOTSUPP;

	if (copy_from_user(&range, (struct fstrim_range __user *)arg,
				sizeof(range)))
		return -EFAULT;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	range.minlen = max((unsigned int)range.minlen,
			   bdev_discard_granularity(sb->s_bdev));
	ret = f2fs_trim_fs(F2FS_SB(sb), &range);
	mnt_drop_write_file(filp);
	if (ret < 0)
		return ret;

	if (copy_to_user((struct fstrim_range __user *)arg, &range,
				sizeof(range)))
		return -EFAULT;
	f2fs_update_time(F2FS_I_SB(inode), REQ_TIME);
	return 0;
}

static bool uuid_is_nonzero(__u8 u[16])
{
	int i;

	for (i = 0; i < 16; i++)
		if (u[i])
			return true;
	return false;
}

static int f2fs_ioc_set_encryption_policy(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);

	if (!f2fs_sb_has_encrypt(F2FS_I_SB(inode)))
		return -EOPNOTSUPP;

	f2fs_update_time(F2FS_I_SB(inode), REQ_TIME);

	return fscrypt_ioctl_set_policy(filp, (const void __user *)arg);
}

static int f2fs_ioc_get_encryption_policy(struct file *filp, unsigned long arg)
{
	if (!f2fs_sb_has_encrypt(F2FS_I_SB(file_inode(filp))))
		return -EOPNOTSUPP;
	return fscrypt_ioctl_get_policy(filp, (void __user *)arg);
}

static int f2fs_ioc_get_encryption_pwsalt(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	u8 encrypt_pw_salt[16];
	int err;

	if (!f2fs_sb_has_encrypt(sbi))
		return -EOPNOTSUPP;

	err = mnt_want_write_file(filp);
	if (err)
		return err;

	f2fs_down_write(&sbi->sb_lock);

	if (uuid_is_nonzero(sbi->raw_super->encrypt_pw_salt))
		goto got_it;

	/* update superblock with uuid */
	generate_random_uuid(sbi->raw_super->encrypt_pw_salt);

	err = f2fs_commit_super(sbi, false);
	if (err) {
		/* undo new data */
		memset(sbi->raw_super->encrypt_pw_salt, 0, 16);
		goto out_err;
	}
got_it:
	memcpy(encrypt_pw_salt, sbi->raw_super->encrypt_pw_salt, 16);
out_err:
	f2fs_up_write(&sbi->sb_lock);
	mnt_drop_write_file(filp);

	if (!err && copy_to_user((__u8 __user *)arg, encrypt_pw_salt, 16))
		err = -EFAULT;

	return err;
}

static int f2fs_ioc_get_encryption_policy_ex(struct file *filp,
					     unsigned long arg)
{
	if (!f2fs_sb_has_encrypt(F2FS_I_SB(file_inode(filp))))
		return -EOPNOTSUPP;

	return fscrypt_ioctl_get_policy_ex(filp, (void __user *)arg);
}

static int f2fs_ioc_add_encryption_key(struct file *filp, unsigned long arg)
{
	if (!f2fs_sb_has_encrypt(F2FS_I_SB(file_inode(filp))))
		return -EOPNOTSUPP;

	return fscrypt_ioctl_add_key(filp, (void __user *)arg);
}

static int f2fs_ioc_remove_encryption_key(struct file *filp, unsigned long arg)
{
	if (!f2fs_sb_has_encrypt(F2FS_I_SB(file_inode(filp))))
		return -EOPNOTSUPP;

	return fscrypt_ioctl_remove_key(filp, (void __user *)arg);
}

static int f2fs_ioc_remove_encryption_key_all_users(struct file *filp,
						    unsigned long arg)
{
	if (!f2fs_sb_has_encrypt(F2FS_I_SB(file_inode(filp))))
		return -EOPNOTSUPP;

	return fscrypt_ioctl_remove_key_all_users(filp, (void __user *)arg);
}

static int f2fs_ioc_get_encryption_key_status(struct file *filp,
					      unsigned long arg)
{
	if (!f2fs_sb_has_encrypt(F2FS_I_SB(file_inode(filp))))
		return -EOPNOTSUPP;

	return fscrypt_ioctl_get_key_status(filp, (void __user *)arg);
}

static int f2fs_ioc_get_encryption_nonce(struct file *filp, unsigned long arg)
{
	if (!f2fs_sb_has_encrypt(F2FS_I_SB(file_inode(filp))))
		return -EOPNOTSUPP;

	return fscrypt_ioctl_get_nonce(filp, (void __user *)arg);
}

static int f2fs_ioc_gc(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_gc_control gc_control = { .victim_segno = NULL_SEGNO,
			.no_bg_gc = false,
			.should_migrate_blocks = false,
			.nr_free_secs = 0 };
	__u32 sync;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (get_user(sync, (__u32 __user *)arg))
		return -EFAULT;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	if (!sync) {
		if (!f2fs_down_write_trylock(&sbi->gc_lock)) {
			ret = -EBUSY;
			goto out;
		}
	} else {
		f2fs_down_write(&sbi->gc_lock);
	}

	gc_control.init_gc_type = sync ? FG_GC : BG_GC;
	gc_control.err_gc_skipped = sync;
	ret = f2fs_gc(sbi, &gc_control);
out:
	mnt_drop_write_file(filp);
	return ret;
}

static int __f2fs_ioc_gc_range(struct file *filp, struct f2fs_gc_range *range)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(file_inode(filp));
	struct f2fs_gc_control gc_control = {
			.init_gc_type = range->sync ? FG_GC : BG_GC,
			.no_bg_gc = false,
			.should_migrate_blocks = false,
			.err_gc_skipped = range->sync,
			.nr_free_secs = 0 };
	u64 end;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	end = range->start + range->len;
	if (end < range->start || range->start < MAIN_BLKADDR(sbi) ||
					end >= MAX_BLKADDR(sbi))
		return -EINVAL;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

do_more:
	if (!range->sync) {
		if (!f2fs_down_write_trylock(&sbi->gc_lock)) {
			ret = -EBUSY;
			goto out;
		}
	} else {
		f2fs_down_write(&sbi->gc_lock);
	}

	gc_control.victim_segno = GET_SEGNO(sbi, range->start);
	ret = f2fs_gc(sbi, &gc_control);
	if (ret) {
		if (ret == -EBUSY)
			ret = -EAGAIN;
		goto out;
	}
	range->start += CAP_BLKS_PER_SEC(sbi);
	if (range->start <= end)
		goto do_more;
out:
	mnt_drop_write_file(filp);
	return ret;
}

static int f2fs_ioc_gc_range(struct file *filp, unsigned long arg)
{
	struct f2fs_gc_range range;

	if (copy_from_user(&range, (struct f2fs_gc_range __user *)arg,
							sizeof(range)))
		return -EFAULT;
	return __f2fs_ioc_gc_range(filp, &range);
}

static int f2fs_ioc_write_checkpoint(struct file *filp)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED))) {
		f2fs_info(sbi, "Skipping Checkpoint. Checkpoints currently disabled.");
		return -EINVAL;
	}

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	ret = f2fs_sync_fs(sbi->sb, 1);

	mnt_drop_write_file(filp);
	return ret;
}

static int f2fs_defragment_range(struct f2fs_sb_info *sbi,
					struct file *filp,
					struct f2fs_defragment *range)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_map_blocks map = { .m_next_extent = NULL,
					.m_seg_type = NO_CHECK_TYPE,
					.m_may_create = false };
	struct extent_info ei = {};
	pgoff_t pg_start, pg_end, next_pgofs;
	unsigned int blk_per_seg = sbi->blocks_per_seg;
	unsigned int total = 0, sec_num;
	block_t blk_end = 0;
	bool fragmented = false;
	int err;

	pg_start = range->start >> PAGE_SHIFT;
	pg_end = (range->start + range->len) >> PAGE_SHIFT;

	f2fs_balance_fs(sbi, true);

	inode_lock(inode);

#ifdef CONFIG_F2FS_FS_DEDUP
	mark_file_modified(inode);
	if (f2fs_is_outer_inode(inode)) {
		err = f2fs_revoke_deduped_inode(inode, __func__);
		if (err)
			goto unlock_out;
	}
#endif

#ifdef CONFIG_F2FS_FS_COMPRESSION_FIXED_OUTPUT
	if (f2fs_compressed_file(inode)) {
		CLEAR_IFLAG_IF_SET(inode, F2FS_NOCOMP_FL);
		if (is_inode_flag_set(inode, FI_COMPRESS_RELEASED)) {
			err = f2fs_reserve_compress_blocks(inode, NULL);
			if (err < 0)
				goto unlock_out;
		}
		err = f2fs_decompress_inode(inode);
		if (err < 0)
			goto unlock_out;
	}
#endif
	if (is_inode_flag_set(inode, FI_COMPRESS_RELEASED) ||
		f2fs_is_atomic_file(inode)) {
		err = -EINVAL;
		goto unlock_out;
	}

	/* if in-place-update policy is enabled, don't waste time here */
	set_inode_flag(inode, FI_OPU_WRITE);
	if (f2fs_should_update_inplace(inode, NULL)) {
		err = -EINVAL;
		goto out;
	}

	/* writeback all dirty pages in the range */
	err = filemap_write_and_wait_range(inode->i_mapping, range->start,
						range->start + range->len - 1);
	if (err)
		goto out;

	/*
	 * lookup mapping info in extent cache, skip defragmenting if physical
	 * block addresses are continuous.
	 */
	if (f2fs_lookup_read_extent_cache(inode, pg_start, &ei)) {
		if ((pgoff_t)ei.fofs + ei.len >= pg_end)
			goto out;
	}

	map.m_lblk = pg_start;
	map.m_next_pgofs = &next_pgofs;

	/*
	 * lookup mapping info in dnode page cache, skip defragmenting if all
	 * physical block addresses are continuous even if there are hole(s)
	 * in logical blocks.
	 */
	while (map.m_lblk < pg_end) {
		map.m_len = pg_end - map.m_lblk;
		err = f2fs_map_blocks(inode, &map, F2FS_GET_BLOCK_DEFAULT);
		if (err)
			goto out;

		if (!(map.m_flags & F2FS_MAP_FLAGS)) {
			map.m_lblk = next_pgofs;
			continue;
		}

		if (blk_end && blk_end != map.m_pblk)
			fragmented = true;

		/* record total count of block that we're going to move */
		total += map.m_len;

		blk_end = map.m_pblk + map.m_len;

		map.m_lblk += map.m_len;
	}

	if (!fragmented) {
		total = 0;
		goto out;
	}

	sec_num = DIV_ROUND_UP(total, CAP_BLKS_PER_SEC(sbi));

	/*
	 * make sure there are enough free section for LFS allocation, this can
	 * avoid defragment running in SSR mode when free section are allocated
	 * intensively
	 */
	if (has_not_enough_free_secs(sbi, 0, sec_num)) {
		err = -EAGAIN;
		goto out;
	}

	map.m_lblk = pg_start;
	map.m_len = pg_end - pg_start;
	total = 0;

	while (map.m_lblk < pg_end) {
		pgoff_t idx;
		int cnt = 0;

do_map:
		map.m_len = pg_end - map.m_lblk;
		err = f2fs_map_blocks(inode, &map, F2FS_GET_BLOCK_DEFAULT);
		if (err)
			goto clear_out;

		if (!(map.m_flags & F2FS_MAP_FLAGS)) {
			map.m_lblk = next_pgofs;
			goto check;
		}

		set_inode_flag(inode, FI_SKIP_WRITES);

		idx = map.m_lblk;
		while (idx < map.m_lblk + map.m_len && cnt < blk_per_seg) {
			struct page *page;

			page = f2fs_get_lock_data_page(inode, idx, true);
			if (IS_ERR(page)) {
				err = PTR_ERR(page);
				goto clear_out;
			}

			f2fs_wait_on_page_writeback(page, DATA, true, true);

			set_page_dirty(page);
			set_page_private_gcing(page);
			f2fs_put_page(page, 1);

			idx++;
			cnt++;
			total++;
		}

		map.m_lblk = idx;
check:
		if (map.m_lblk < pg_end && cnt < blk_per_seg)
			goto do_map;

		clear_inode_flag(inode, FI_SKIP_WRITES);

		err = filemap_fdatawrite(inode->i_mapping);
		if (err)
			goto out;
	}
clear_out:
	clear_inode_flag(inode, FI_SKIP_WRITES);
out:
	clear_inode_flag(inode, FI_OPU_WRITE);
unlock_out:
	inode_unlock(inode);
	if (!err)
		range->len = (u64)total << PAGE_SHIFT;
	return err;
}

static int f2fs_ioc_defragment(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_defragment range;
	int err;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (!S_ISREG(inode->i_mode) || f2fs_is_atomic_file(inode))
		return -EINVAL;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	if (copy_from_user(&range, (struct f2fs_defragment __user *)arg,
							sizeof(range)))
		return -EFAULT;

	/* verify alignment of offset & size */
	if (range.start & (F2FS_BLKSIZE - 1) || range.len & (F2FS_BLKSIZE - 1))
		return -EINVAL;

	if (unlikely((range.start + range.len) >> PAGE_SHIFT >
					max_file_blocks(inode)))
		return -EINVAL;

	err = mnt_want_write_file(filp);
	if (err)
		return err;

	err = f2fs_defragment_range(sbi, filp, &range);
	mnt_drop_write_file(filp);

	f2fs_update_time(sbi, REQ_TIME);
	if (err < 0)
		return err;

	if (copy_to_user((struct f2fs_defragment __user *)arg, &range,
							sizeof(range)))
		return -EFAULT;

	return 0;
}

static int f2fs_move_file_range(struct file *file_in, loff_t pos_in,
			struct file *file_out, loff_t pos_out, size_t len)
{
	struct inode *src = file_inode(file_in);
	struct inode *dst = file_inode(file_out);
	struct f2fs_sb_info *sbi = F2FS_I_SB(src);
	size_t olen = len, dst_max_i_size = 0;
	size_t dst_osize;
	int ret;

	if (file_in->f_path.mnt != file_out->f_path.mnt ||
				src->i_sb != dst->i_sb)
		return -EXDEV;

	if (unlikely(f2fs_readonly(src->i_sb)))
		return -EROFS;

	if (!S_ISREG(src->i_mode) || !S_ISREG(dst->i_mode))
		return -EINVAL;

	if (IS_ENCRYPTED(src) || IS_ENCRYPTED(dst))
		return -EOPNOTSUPP;

	if (pos_out < 0 || pos_in < 0)
		return -EINVAL;

	if (src == dst) {
		if (pos_in == pos_out)
			return 0;
		if (pos_out > pos_in && pos_out < pos_in + len)
			return -EINVAL;
	}

	inode_lock(src);
	if (src != dst) {
		ret = -EBUSY;
		if (!inode_trylock(dst))
			goto out;
	}

#ifdef CONFIG_F2FS_SEQZONE
	if (f2fs_seqzone_file(src) || f2fs_seqzone_file(dst)) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}
#endif

#ifdef CONFIG_F2FS_FS_DEDUP
	mark_file_modified(src);
	if (f2fs_is_outer_inode(src)) {
		ret = f2fs_revoke_deduped_inode(src, __func__);
		if (ret)
			goto out_unlock;
	}

	mark_file_modified(dst);
	if (f2fs_is_outer_inode(dst)) {
		ret = f2fs_revoke_deduped_inode(dst, __func__);
		if (ret)
			goto out_unlock;
	}
#endif

	if (f2fs_is_atomic_file(src) || f2fs_is_atomic_file(dst)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = -EINVAL;
	if (pos_in + len > src->i_size || pos_in + len < pos_in)
		goto out_unlock;
	if (len == 0)
		olen = len = src->i_size - pos_in;
	if (pos_in + len == src->i_size)
		len = ALIGN(src->i_size, F2FS_BLKSIZE) - pos_in;
	if (len == 0) {
		ret = 0;
		goto out_unlock;
	}

	dst_osize = dst->i_size;
	if (pos_out + olen > dst->i_size)
		dst_max_i_size = pos_out + olen;

	/* verify the end result is block aligned */
	if (!IS_ALIGNED(pos_in, F2FS_BLKSIZE) ||
			!IS_ALIGNED(pos_in + len, F2FS_BLKSIZE) ||
			!IS_ALIGNED(pos_out, F2FS_BLKSIZE))
		goto out_unlock;

	ret = f2fs_convert_inline_inode(src);
	if (ret)
		goto out_unlock;

	ret = f2fs_convert_inline_inode(dst);
	if (ret)
		goto out_unlock;

#ifdef CONFIG_F2FS_FS_COMPRESSION_FIXED_OUTPUT
	if (f2fs_compressed_file(src)) {
		CLEAR_IFLAG_IF_SET(src, F2FS_NOCOMP_FL);
		CLEAR_IFLAG_IF_SET(dst, F2FS_NOCOMP_FL);
		if (is_inode_flag_set(src, FI_COMPRESS_RELEASED)) {
			ret = f2fs_reserve_compress_blocks(src, NULL);
			if (ret < 0)
				goto out_unlock;
		}
		ret = f2fs_decompress_inode(src);
		if (ret < 0)
			goto out_unlock;
	}

	if (f2fs_compressed_file(dst)) {
		if (is_inode_flag_set(dst, FI_COMPRESS_RELEASED)) {
			ret = f2fs_reserve_compress_blocks(dst, NULL);
			if (ret < 0)
				goto out_unlock;
		}
		ret = f2fs_decompress_inode(dst);
		if (ret < 0)
			goto out_unlock;
	}
#endif

	/* write out all dirty pages from offset */
	ret = filemap_write_and_wait_range(src->i_mapping,
					pos_in, pos_in + len);
	if (ret)
		goto out_unlock;

	ret = filemap_write_and_wait_range(dst->i_mapping,
					pos_out, pos_out + len);
	if (ret)
		goto out_unlock;

	f2fs_balance_fs(sbi, true);

	f2fs_down_write(&F2FS_I(src)->i_gc_rwsem[WRITE]);
	if (src != dst) {
		ret = -EBUSY;
		if (!f2fs_down_write_trylock(&F2FS_I(dst)->i_gc_rwsem[WRITE]))
			goto out_src;
	}

	f2fs_lock_op(sbi);
	ret = __exchange_data_block(src, dst, pos_in >> F2FS_BLKSIZE_BITS,
				pos_out >> F2FS_BLKSIZE_BITS,
				len >> F2FS_BLKSIZE_BITS, false);

	if (!ret) {
		if (dst_max_i_size)
			f2fs_i_size_write(dst, dst_max_i_size);
		else if (dst_osize != dst->i_size)
			f2fs_i_size_write(dst, dst_osize);
	}
	f2fs_unlock_op(sbi);

	if (src != dst)
		f2fs_up_write(&F2FS_I(dst)->i_gc_rwsem[WRITE]);
out_src:
	f2fs_up_write(&F2FS_I(src)->i_gc_rwsem[WRITE]);
#ifdef CONFIG_F2FS_APPBOOST
	src->i_mtime = src->i_ctime = current_time(src);
	f2fs_mark_inode_dirty_sync(src, false);
	if (src != dst) {
		dst->i_mtime = dst->i_ctime = current_time(dst);
		f2fs_mark_inode_dirty_sync(dst, false);
	}
	f2fs_update_time(sbi, REQ_TIME);
#endif
out_unlock:
	if (src != dst)
		inode_unlock(dst);
out:
	inode_unlock(src);
	return ret;
}

static int __f2fs_ioc_move_range(struct file *filp,
				struct f2fs_move_range *range)
{
	struct fd dst;
	int err;

	if (!(filp->f_mode & FMODE_READ) ||
			!(filp->f_mode & FMODE_WRITE))
		return -EBADF;

	dst = fdget(range->dst_fd);
	if (!dst.file)
		return -EBADF;

	if (!(dst.file->f_mode & FMODE_WRITE)) {
		err = -EBADF;
		goto err_out;
	}

	err = mnt_want_write_file(filp);
	if (err)
		goto err_out;

	err = f2fs_move_file_range(filp, range->pos_in, dst.file,
					range->pos_out, range->len);

	mnt_drop_write_file(filp);
err_out:
	fdput(dst);
	return err;
}

static int f2fs_ioc_move_range(struct file *filp, unsigned long arg)
{
	struct f2fs_move_range range;

	if (copy_from_user(&range, (struct f2fs_move_range __user *)arg,
							sizeof(range)))
		return -EFAULT;
	return __f2fs_ioc_move_range(filp, &range);
}

static int f2fs_ioc_flush_device(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct sit_info *sm = SIT_I(sbi);
	unsigned int start_segno = 0, end_segno = 0;
	unsigned int dev_start_segno = 0, dev_end_segno = 0;
	struct f2fs_flush_device range;
	struct f2fs_gc_control gc_control = {
			.init_gc_type = FG_GC,
			.should_migrate_blocks = true,
			.err_gc_skipped = true,
			.nr_free_secs = 0 };
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED)))
		return -EINVAL;

	if (copy_from_user(&range, (struct f2fs_flush_device __user *)arg,
							sizeof(range)))
		return -EFAULT;

	if (!f2fs_is_multi_device(sbi) || sbi->s_ndevs - 1 <= range.dev_num ||
			__is_large_section(sbi)) {
		f2fs_warn(sbi, "Can't flush %u in %d for segs_per_sec %u != 1",
			  range.dev_num, sbi->s_ndevs, sbi->segs_per_sec);
		return -EINVAL;
	}

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	if (range.dev_num != 0)
		dev_start_segno = GET_SEGNO(sbi, FDEV(range.dev_num).start_blk);
	dev_end_segno = GET_SEGNO(sbi, FDEV(range.dev_num).end_blk);

	start_segno = sm->last_victim[FLUSH_DEVICE];
	if (start_segno < dev_start_segno || start_segno >= dev_end_segno)
		start_segno = dev_start_segno;
	end_segno = min(start_segno + range.segments, dev_end_segno);

	while (start_segno < end_segno) {
		if (!f2fs_down_write_trylock(&sbi->gc_lock)) {
			ret = -EBUSY;
			goto out;
		}
		sm->last_victim[GC_CB] = end_segno + 1;
		sm->last_victim[GC_GREEDY] = end_segno + 1;
		sm->last_victim[ALLOC_NEXT] = end_segno + 1;

		gc_control.victim_segno = start_segno;
		ret = f2fs_gc(sbi, &gc_control);
		if (ret == -EAGAIN)
			ret = 0;
		else if (ret < 0)
			break;
		start_segno++;
	}
out:
	mnt_drop_write_file(filp);
	return ret;
}

static int f2fs_ioc_get_features(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	u32 sb_feature = le32_to_cpu(F2FS_I_SB(inode)->raw_super->feature);

	/* Must validate to set it with SQLite behavior in Android. */
	sb_feature |= F2FS_FEATURE_ATOMIC_WRITE;

	return put_user(sb_feature, (u32 __user *)arg);
}

#ifdef CONFIG_QUOTA
int f2fs_transfer_project_quota(struct inode *inode, kprojid_t kprojid)
{
	struct dquot *transfer_to[MAXQUOTAS] = {};
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct super_block *sb = sbi->sb;
	int err;

	transfer_to[PRJQUOTA] = dqget(sb, make_kqid_projid(kprojid));
	if (IS_ERR(transfer_to[PRJQUOTA]))
		return PTR_ERR(transfer_to[PRJQUOTA]);

	err = __dquot_transfer(inode, transfer_to);
	if (err)
		set_sbi_flag(sbi, SBI_QUOTA_NEED_REPAIR);
	dqput(transfer_to[PRJQUOTA]);
	return err;
}

static int f2fs_ioc_setproject(struct inode *inode, __u32 projid)
{
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_inode *ri = NULL;
	kprojid_t kprojid;
	int err;

	if (!f2fs_sb_has_project_quota(sbi)) {
		if (projid != F2FS_DEF_PROJID)
			return -EOPNOTSUPP;
		else
			return 0;
	}

	if (!f2fs_has_extra_attr(inode))
		return -EOPNOTSUPP;

	kprojid = make_kprojid(&init_user_ns, (projid_t)projid);

	if (projid_eq(kprojid, fi->i_projid))
		return 0;

	err = -EPERM;
	/* Is it quota file? Do not allow user to mess with it */
	if (IS_NOQUOTA(inode))
		return err;

	if (!F2FS_FITS_IN_INODE(ri, fi->i_extra_isize, i_projid))
		return -EOVERFLOW;

	err = f2fs_dquot_initialize(inode);
	if (err)
		return err;

	f2fs_lock_op(sbi);
	err = f2fs_transfer_project_quota(inode, kprojid);
	if (err)
		goto out_unlock;

	fi->i_projid = kprojid;
	inode->i_ctime = current_time(inode);
	f2fs_mark_inode_dirty_sync(inode, true);
out_unlock:
	f2fs_unlock_op(sbi);
	return err;
}
#else
int f2fs_transfer_project_quota(struct inode *inode, kprojid_t kprojid)
{
	return 0;
}

static int f2fs_ioc_setproject(struct inode *inode, __u32 projid)
{
	if (projid != F2FS_DEF_PROJID)
		return -EOPNOTSUPP;
	return 0;
}
#endif

int f2fs_fileattr_get(struct dentry *dentry, struct fileattr *fa)
{
	struct inode *inode = d_inode(dentry);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	u32 fsflags = f2fs_iflags_to_fsflags(fi->i_flags);

	if (IS_ENCRYPTED(inode))
		fsflags |= FS_ENCRYPT_FL;
	if (IS_VERITY(inode))
		fsflags |= FS_VERITY_FL;
	if (f2fs_has_inline_data(inode) || f2fs_has_inline_dentry(inode))
		fsflags |= FS_INLINE_DATA_FL;
	if (is_inode_flag_set(inode, FI_PIN_FILE))
		fsflags |= FS_NOCOW_FL;
	if (!may_compress)
		fsflags &= ~FS_COMPR_FL;

	fileattr_fill_flags(fa, fsflags & F2FS_GETTABLE_FS_FL);

	if (f2fs_sb_has_project_quota(F2FS_I_SB(inode)))
		fa->fsx_projid = from_kprojid(&init_user_ns, fi->i_projid);

	return 0;
}

int f2fs_fileattr_set(struct user_namespace *mnt_userns,
		      struct dentry *dentry, struct fileattr *fa)
{
	struct inode *inode = d_inode(dentry);
	u32 fsflags = fa->flags, mask = F2FS_SETTABLE_FS_FL;
	u32 iflags;
	int err;

	if (unlikely(f2fs_cp_error(F2FS_I_SB(inode))))
		return -EIO;
	if (!f2fs_is_checkpoint_ready(F2FS_I_SB(inode)))
		return -ENOSPC;
	if (fsflags & ~F2FS_GETTABLE_FS_FL)
		return -EOPNOTSUPP;
	fsflags &= F2FS_SETTABLE_FS_FL;
	if (!fa->flags_valid)
		mask &= FS_COMMON_FL;

	iflags = f2fs_fsflags_to_iflags(fsflags);
	if (f2fs_mask_flags(inode->i_mode, iflags) != iflags)
		return -EOPNOTSUPP;

	err = f2fs_setflags_common(inode, iflags, f2fs_fsflags_to_iflags(mask));
	if (!err)
		err = f2fs_ioc_setproject(inode, fa->fsx_projid);

	return err;
}

int f2fs_pin_file_control(struct inode *inode, bool inc)
{
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	/* Use i_gc_failures for normal file as a risk signal. */
	if (inc)
		f2fs_i_gc_failures_write(inode,
				fi->i_gc_failures[GC_FAILURE_PIN] + 1);

	if (fi->i_gc_failures[GC_FAILURE_PIN] > sbi->gc_pin_file_threshold) {
		f2fs_warn(sbi, "%s: Enable GC = ino %lx after %x GC trials",
			  __func__, inode->i_ino,
			  fi->i_gc_failures[GC_FAILURE_PIN]);
		clear_inode_flag(inode, FI_PIN_FILE);
		return -EAGAIN;
	}
	return 0;
}

static int f2fs_ioc_set_pin_file(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	__u32 pin;
	int ret = 0;

	if (get_user(pin, (__u32 __user *)arg))
		return -EFAULT;

	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	if (f2fs_readonly(F2FS_I_SB(inode)->sb))
		return -EROFS;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	inode_lock(inode);

	if (f2fs_is_atomic_file(inode)) {
		ret = -EINVAL;
		goto out;
	}

	if (!pin) {
		clear_inode_flag(inode, FI_PIN_FILE);
		f2fs_i_gc_failures_write(inode, 0);
		goto done;
	}

	if (f2fs_should_update_outplace(inode, NULL)) {
		ret = -EINVAL;
		goto out;
	}

#ifdef CONFIG_F2FS_FS_DEDUP
	mark_file_modified(inode);
	if (f2fs_is_outer_inode(inode)) {
		ret = f2fs_revoke_deduped_inode(inode, __func__);
		if (ret)
			goto out;
	}
#endif

	if (f2fs_pin_file_control(inode, false)) {
		ret = -EAGAIN;
		goto out;
	}

	ret = f2fs_convert_inline_inode(inode);
	if (ret)
		goto out;

#ifdef CONFIG_F2FS_FS_COMPRESSION_FIXED_OUTPUT
	if (f2fs_compressed_file(inode)) {
		if (is_inode_flag_set(inode, FI_COMPRESS_RELEASED)) {
			ret = f2fs_reserve_compress_blocks(inode, NULL);
			if (ret < 0)
				goto out;
		}
		ret = f2fs_decompress_inode(inode);
		if (ret < 0)
			goto out;
	}
#endif

	if (!f2fs_disable_compressed_file(inode)) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	set_inode_flag(inode, FI_PIN_FILE);
	ret = F2FS_I(inode)->i_gc_failures[GC_FAILURE_PIN];
done:
	f2fs_update_time(F2FS_I_SB(inode), REQ_TIME);
out:
	inode_unlock(inode);
	mnt_drop_write_file(filp);
	return ret;
}

static int f2fs_ioc_get_pin_file(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	__u32 pin = 0;

	if (is_inode_flag_set(inode, FI_PIN_FILE))
		pin = F2FS_I(inode)->i_gc_failures[GC_FAILURE_PIN];
	return put_user(pin, (u32 __user *)arg);
}

int f2fs_precache_extents(struct inode *inode)
{
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct f2fs_map_blocks map;
	pgoff_t m_next_extent;
	loff_t end;
	int err;

	if (is_inode_flag_set(inode, FI_NO_EXTENT))
		return -EOPNOTSUPP;

	map.m_lblk = 0;
	map.m_pblk = 0;
	map.m_next_pgofs = NULL;
	map.m_next_extent = &m_next_extent;
	map.m_seg_type = NO_CHECK_TYPE;
	map.m_may_create = false;
	end = max_file_blocks(inode);

	while (map.m_lblk < end) {
		map.m_len = end - map.m_lblk;

		f2fs_down_write(&fi->i_gc_rwsem[WRITE]);
		err = f2fs_map_blocks(inode, &map, F2FS_GET_BLOCK_PRECACHE);
		f2fs_up_write(&fi->i_gc_rwsem[WRITE]);
		if (err)
			return err;

		map.m_lblk = m_next_extent;
	}

	return 0;
}

static int f2fs_ioc_precache_extents(struct file *filp)
{
	return f2fs_precache_extents(file_inode(filp));
}

static int f2fs_ioc_resize_fs(struct file *filp, unsigned long arg)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(file_inode(filp));
	__u64 block_count;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	if (copy_from_user(&block_count, (void __user *)arg,
			   sizeof(block_count)))
		return -EFAULT;

	return f2fs_resize_fs(filp, block_count);
}

static int f2fs_ioc_enable_verity(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);

	f2fs_update_time(F2FS_I_SB(inode), REQ_TIME);

	if (!f2fs_sb_has_verity(F2FS_I_SB(inode))) {
		f2fs_warn(F2FS_I_SB(inode),
			  "Can't enable fs-verity on inode %lu: the verity feature is not enabled on this filesystem",
			  inode->i_ino);
		return -EOPNOTSUPP;
	}

	return fsverity_ioctl_enable(filp, (const void __user *)arg);
}

static int f2fs_ioc_measure_verity(struct file *filp, unsigned long arg)
{
	if (!f2fs_sb_has_verity(F2FS_I_SB(file_inode(filp))))
		return -EOPNOTSUPP;

	return fsverity_ioctl_measure(filp, (void __user *)arg);
}

static int f2fs_ioc_read_verity_metadata(struct file *filp, unsigned long arg)
{
	if (!f2fs_sb_has_verity(F2FS_I_SB(file_inode(filp))))
		return -EOPNOTSUPP;

	return fsverity_ioctl_read_metadata(filp, (const void __user *)arg);
}

static int f2fs_ioc_getfslabel(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	char *vbuf;
	int count;
	int err = 0;

	vbuf = f2fs_kzalloc(sbi, MAX_VOLUME_NAME, GFP_KERNEL);
	if (!vbuf)
		return -ENOMEM;

	f2fs_down_read(&sbi->sb_lock);
	count = utf16s_to_utf8s(sbi->raw_super->volume_name,
			ARRAY_SIZE(sbi->raw_super->volume_name),
			UTF16_LITTLE_ENDIAN, vbuf, MAX_VOLUME_NAME);
	f2fs_up_read(&sbi->sb_lock);

	if (copy_to_user((char __user *)arg, vbuf,
				min(FSLABEL_MAX, count)))
		err = -EFAULT;

	kfree(vbuf);
	return err;
}

static int f2fs_ioc_setfslabel(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	char *vbuf;
	int err = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	vbuf = strndup_user((const char __user *)arg, FSLABEL_MAX);
	if (IS_ERR(vbuf))
		return PTR_ERR(vbuf);

	err = mnt_want_write_file(filp);
	if (err)
		goto out;

	f2fs_down_write(&sbi->sb_lock);

	memset(sbi->raw_super->volume_name, 0,
			sizeof(sbi->raw_super->volume_name));
	utf8s_to_utf16s(vbuf, strlen(vbuf), UTF16_LITTLE_ENDIAN,
			sbi->raw_super->volume_name,
			ARRAY_SIZE(sbi->raw_super->volume_name));

	err = f2fs_commit_super(sbi, false);

	f2fs_up_write(&sbi->sb_lock);

	mnt_drop_write_file(filp);
out:
	kfree(vbuf);
	return err;
}

static int f2fs_get_compress_blocks(struct inode *inode, __u64 *blocks)
{
	if (!f2fs_sb_has_compression(F2FS_I_SB(inode)))
		return -EOPNOTSUPP;

	if (!f2fs_compressed_file(inode))
		return -EINVAL;

	*blocks = atomic_read(&F2FS_I(inode)->i_compr_blocks);

	return 0;
}

static int f2fs_ioc_get_compress_blocks(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	__u64 blocks;
	int ret;

	ret = f2fs_get_compress_blocks(inode, &blocks);
	if (ret < 0)
		return ret;

	return put_user(blocks, (u64 __user *)arg);
}

static int release_compress_blocks(struct dnode_of_data *dn, pgoff_t count)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dn->inode);
	unsigned int released_blocks = 0;
	int cluster_size = F2FS_I(dn->inode)->i_cluster_size;
	block_t blkaddr;
	int i;

	for (i = 0; i < count; i++) {
		blkaddr = data_blkaddr(dn->inode, dn->node_page,
						dn->ofs_in_node + i);

		if (!__is_valid_data_blkaddr(blkaddr))
			continue;
		if (unlikely(!f2fs_is_valid_blkaddr(sbi, blkaddr,
					DATA_GENERIC_ENHANCE))) {
			f2fs_handle_error(sbi, ERROR_INVALID_BLKADDR);
			return -EFSCORRUPTED;
		}
	}

	while (count) {
		int compr_blocks = 0;

		for (i = 0; i < cluster_size; i++, dn->ofs_in_node++) {
			blkaddr = f2fs_data_blkaddr(dn);

			if (i == 0) {
				if (blkaddr == COMPRESS_ADDR)
					continue;
				dn->ofs_in_node += cluster_size;
				goto next;
			}

			if (__is_valid_data_blkaddr(blkaddr))
				compr_blocks++;

			if (blkaddr != NEW_ADDR)
				continue;

			f2fs_set_data_blkaddr(dn, NULL_ADDR);
		}

		f2fs_i_compr_blocks_update(dn->inode, compr_blocks, false);
		dec_valid_block_count(sbi, dn->inode,
					cluster_size - compr_blocks);

		released_blocks += cluster_size - compr_blocks;
next:
		count -= cluster_size;
	}

	return released_blocks;
}

static int f2fs_release_compress_blocks(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	pgoff_t page_idx = 0, last_idx;
	unsigned int released_blocks = 0;
	int ret;
	int writecount;

	if (!f2fs_sb_has_compression(sbi))
		return -EOPNOTSUPP;

	if (!f2fs_compressed_file(inode))
		return -EINVAL;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	f2fs_balance_fs(sbi, true);

	inode_lock(inode);

	writecount = atomic_read(&inode->i_writecount);
	if ((filp->f_mode & FMODE_WRITE && writecount != 1) ||
			(!(filp->f_mode & FMODE_WRITE) && writecount)) {
		ret = -EBUSY;
		goto out;
	}

	if (is_inode_flag_set(inode, FI_COMPRESS_RELEASED)) {
		ret = -EINVAL;
		goto out;
	}

	ret = filemap_write_and_wait_range(inode->i_mapping, 0, LLONG_MAX);
	if (ret)
		goto out;

	if (!atomic_read(&F2FS_I(inode)->i_compr_blocks)) {
		ret = -EPERM;
		goto out;
	}

	f2fs_info(sbi, "start release cblocks ino %lu (%pd) size %llu blocks %llu "
		"cblocks %d\n", inode->i_ino, file_dentry(filp),
		i_size_read(inode), inode->i_blocks,
		atomic_read(&F2FS_I(inode)->i_compr_blocks));

	set_inode_flag(inode, FI_COMPRESS_RELEASED);
	inode->i_ctime = current_time(inode);
	f2fs_mark_inode_dirty_sync(inode, true);

	f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
	filemap_invalidate_lock(inode->i_mapping);

	last_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);

	while (page_idx < last_idx) {
		struct dnode_of_data dn;
		pgoff_t end_offset, count;

		set_new_dnode(&dn, inode, NULL, NULL, 0);
		ret = f2fs_get_dnode_of_data(&dn, page_idx, LOOKUP_NODE);
		if (ret) {
			if (ret == -ENOENT) {
				page_idx = f2fs_get_next_page_offset(&dn,
								page_idx);
				ret = 0;
				continue;
			}
			break;
		}

		end_offset = ADDRS_PER_PAGE(dn.node_page, inode);
		count = min(end_offset - dn.ofs_in_node, last_idx - page_idx);
		count = round_up(count, F2FS_I(inode)->i_cluster_size);

		ret = release_compress_blocks(&dn, count);

		f2fs_put_dnode(&dn);

		if (ret < 0)
			break;

		page_idx += count;
		released_blocks += ret;
	}

	filemap_invalidate_unlock(inode->i_mapping);
	f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);

	f2fs_info(sbi, "end release cblocks ino %lu (%pd) size %llu blocks %llu "
		"cblocks %d rblocks %u ret %d\n", inode->i_ino, file_dentry(filp),
		i_size_read(inode), inode->i_blocks,
		atomic_read(&F2FS_I(inode)->i_compr_blocks), released_blocks, ret);

out:
	inode_unlock(inode);

	mnt_drop_write_file(filp);

	if (ret >= 0) {
		ret = put_user(released_blocks, (u64 __user *)arg);
	} else if (released_blocks &&
			atomic_read(&F2FS_I(inode)->i_compr_blocks)) {
		set_sbi_flag(sbi, SBI_NEED_FSCK);
		f2fs_warn(sbi, "%s: partial blocks were released i_ino=%lx "
			"iblocks=%llu, released=%u, compr_blocks=%u, "
			"run fsck to fix.",
			__func__, inode->i_ino, inode->i_blocks,
			released_blocks,
			atomic_read(&F2FS_I(inode)->i_compr_blocks));
	}

	return ret;
}

static int reserve_compress_blocks(struct dnode_of_data *dn, pgoff_t count,
		unsigned int *reserved_blocks)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dn->inode);
	int cluster_size = F2FS_I(dn->inode)->i_cluster_size;
	block_t blkaddr;
	int i;

	for (i = 0; i < count; i++) {
		blkaddr = data_blkaddr(dn->inode, dn->node_page,
						dn->ofs_in_node + i);

		if (!__is_valid_data_blkaddr(blkaddr))
			continue;
		if (unlikely(!f2fs_is_valid_blkaddr(sbi, blkaddr,
					DATA_GENERIC_ENHANCE))) {
			f2fs_handle_error(sbi, ERROR_INVALID_BLKADDR);
			return -EFSCORRUPTED;
		}
	}

	while (count) {
		int compr_blocks = 0;
		blkcnt_t reserved = 0;
		blkcnt_t to_reserved;
		int ret;

		for (i = 0; i < cluster_size; i++, dn->ofs_in_node++) {
			blkaddr = f2fs_data_blkaddr(dn);

			if (i == 0) {
				if (blkaddr == COMPRESS_ADDR)
					continue;
				dn->ofs_in_node += cluster_size;
				goto next;
			}

			/*
			 * compressed cluster was not released due to it
			 * fails in release_compress_blocks(), so NEW_ADDR
			 * is a possible case.
			 */
			if (blkaddr == NEW_ADDR) {
				reserved++;
				continue;
			}
			if (__is_valid_data_blkaddr(blkaddr)) {
				compr_blocks++;
				continue;
			}

			f2fs_set_data_blkaddr(dn,NEW_ADDR);
		}

		to_reserved = cluster_size - compr_blocks - reserved;
		if (time_to_inject(sbi, FAULT_COMPRESS_RESERVE_NOSPC))
			return -ENOSPC;

		/* for the case all blocks in cluster were reserved */
		if (to_reserved == 1) {
			dn->ofs_in_node += cluster_size;
			goto next;
		}

		ret = inc_valid_block_count(sbi, dn->inode,
						&to_reserved, false);
		if (unlikely(ret))
			return ret;

		if (reserved != cluster_size - compr_blocks)
			return -ENOSPC;
		for (i = 0; i < cluster_size; i++, dn->ofs_in_node++) {
			if (f2fs_data_blkaddr(dn) == NULL_ADDR)
				f2fs_set_data_blkaddr(dn, NEW_ADDR);
		}

		f2fs_i_compr_blocks_update(dn->inode, compr_blocks, true);

		*reserved_blocks += to_reserved;
next:
		count -= cluster_size;
	}

	return 0;
}

int f2fs_reserve_compress_blocks(struct inode *inode, unsigned int *ret_rsvd_blks)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	pgoff_t page_idx = 0, last_idx;
	unsigned int reserved_blocks = 0;
	int ret;

	f2fs_bug_on(sbi, !inode_is_locked(inode));

	if (!is_inode_flag_set(inode, FI_COMPRESS_RELEASED))
		return -EINVAL;

	f2fs_info(sbi, "start reserve cblocks ino %lu size %llu blocks %llu "
		"cblocks %d caller %ps\n", inode->i_ino, i_size_read(inode),
		inode->i_blocks, atomic_read(&F2FS_I(inode)->i_compr_blocks),
		__builtin_return_address(0));

	f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
	filemap_invalidate_lock(inode->i_mapping);

	last_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);

	while (page_idx < last_idx) {
		struct dnode_of_data dn;
		pgoff_t end_offset, count;

		set_new_dnode(&dn, inode, NULL, NULL, 0);
		ret = f2fs_get_dnode_of_data(&dn, page_idx, LOOKUP_NODE);
		if (ret) {
			if (ret == -ENOENT) {
				page_idx = f2fs_get_next_page_offset(&dn,
								page_idx);
				ret = 0;
				continue;
			}
			break;
		}

		end_offset = ADDRS_PER_PAGE(dn.node_page, inode);
		count = min(end_offset - dn.ofs_in_node, last_idx - page_idx);
		count = round_up(count, F2FS_I(inode)->i_cluster_size);

		ret = reserve_compress_blocks(&dn, count, &reserved_blocks);

		f2fs_put_dnode(&dn);

		if (ret < 0)
			break;

		page_idx += count;
	}

	filemap_invalidate_unlock(inode->i_mapping);
	f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);

	if (!ret) {
		clear_inode_flag(inode, FI_COMPRESS_RELEASED);
		inode->i_ctime = current_time(inode);
		f2fs_mark_inode_dirty_sync(inode, true);
		if (ret_rsvd_blks)
			*ret_rsvd_blks = reserved_blocks;
	} else if (reserved_blocks &&
			atomic_read(&F2FS_I(inode)->i_compr_blocks)) {
		set_sbi_flag(sbi, SBI_NEED_FSCK);
		f2fs_warn(sbi, "%s: partial blocks were released i_ino=%lx "
			"iblocks=%llu, reserved=%u, compr_blocks=%u, "
			"run fsck to fix.",
			__func__, inode->i_ino, inode->i_blocks,
			reserved_blocks,
			atomic_read(&F2FS_I(inode)->i_compr_blocks));
	}

	f2fs_info(sbi, "end reserve cblocks ino %lu size %llu blocks %llu "
		"cblocks %d rsvd %d caller %ps ret %d\n", inode->i_ino,
		i_size_read(inode), inode->i_blocks,
		atomic_read(&F2FS_I(inode)->i_compr_blocks), reserved_blocks,
		__builtin_return_address(0), ret);

	return ret;
}

static int f2fs_ioc_reserve_compress_blocks(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	unsigned int reserved_blocks = 0;
	int ret;

	if (!f2fs_sb_has_compression(F2FS_I_SB(inode)))
		return -EOPNOTSUPP;

	if (!f2fs_compressed_file(inode))
		return -EINVAL;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	if (atomic_read(&F2FS_I(inode)->i_compr_blocks))
		goto out;

	f2fs_balance_fs(F2FS_I_SB(inode), true);

	inode_lock(inode);
	ret = f2fs_reserve_compress_blocks(inode, &reserved_blocks);
	inode_unlock(inode);

out:
	mnt_drop_write_file(filp);

	if (ret >= 0)
		ret = put_user(reserved_blocks, (u64 __user *)arg);

	return ret;
}

static int f2fs_secure_erase(struct block_device *bdev, struct inode *inode,
		pgoff_t off, block_t block, block_t len, u32 flags)
{
	sector_t sector = SECTOR_FROM_BLOCK(block);
	sector_t nr_sects = SECTOR_FROM_BLOCK(len);
	int ret = 0;

	if (flags & F2FS_TRIM_FILE_DISCARD) {
		if (bdev_max_secure_erase_sectors(bdev))
			ret = blkdev_issue_secure_erase(bdev, sector, nr_sects,
					GFP_NOFS);
		else
			ret = blkdev_issue_discard(bdev, sector, nr_sects,
					GFP_NOFS);
	}

	if (!ret && (flags & F2FS_TRIM_FILE_ZEROOUT)) {
		if (IS_ENCRYPTED(inode))
			ret = fscrypt_zeroout_range(inode, off, block, len);
		else
			ret = blkdev_issue_zeroout(bdev, sector, nr_sects,
					GFP_NOFS, 0);
	}

	return ret;
}

static int f2fs_sec_trim_file(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct address_space *mapping = inode->i_mapping;
	struct block_device *prev_bdev = NULL;
	struct f2fs_sectrim_range range;
	pgoff_t index, pg_end, prev_index = 0;
	block_t prev_block = 0, len = 0;
	loff_t end_addr;
	bool to_end = false;
	int ret = 0;

	if (!(filp->f_mode & FMODE_WRITE))
		return -EBADF;

	if (copy_from_user(&range, (struct f2fs_sectrim_range __user *)arg,
				sizeof(range)))
		return -EFAULT;

	if (range.flags == 0 || (range.flags & ~F2FS_TRIM_FILE_MASK) ||
			!S_ISREG(inode->i_mode))
		return -EINVAL;

	if (((range.flags & F2FS_TRIM_FILE_DISCARD) &&
			!f2fs_hw_support_discard(sbi)) ||
			((range.flags & F2FS_TRIM_FILE_ZEROOUT) &&
			 IS_ENCRYPTED(inode) && f2fs_is_multi_device(sbi)))
		return -EOPNOTSUPP;

	file_start_write(filp);
	inode_lock(inode);

	if (f2fs_is_atomic_file(inode) || f2fs_compressed_file(inode) ||
			range.start >= inode->i_size) {
		ret = -EINVAL;
		goto err;
	}

#ifdef CONFIG_F2FS_FS_DEDUP
	mark_file_modified(inode);
	if (f2fs_is_outer_inode(inode)) {
		ret = f2fs_revoke_deduped_inode(inode, __func__);
		if (ret)
			goto err;
	}
#endif

	if (range.len == 0)
		goto err;

	if (inode->i_size - range.start > range.len) {
		end_addr = range.start + range.len;
	} else {
		end_addr = range.len == (u64)-1 ?
			sbi->sb->s_maxbytes : inode->i_size;
		to_end = true;
	}

	if (!IS_ALIGNED(range.start, F2FS_BLKSIZE) ||
			(!to_end && !IS_ALIGNED(end_addr, F2FS_BLKSIZE))) {
		ret = -EINVAL;
		goto err;
	}

	index = F2FS_BYTES_TO_BLK(range.start);
	pg_end = DIV_ROUND_UP(end_addr, F2FS_BLKSIZE);

	ret = f2fs_convert_inline_inode(inode);
	if (ret)
		goto err;

	f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
	filemap_invalidate_lock(mapping);

	ret = filemap_write_and_wait_range(mapping, range.start,
			to_end ? LLONG_MAX : end_addr - 1);
	if (ret)
		goto out;

	truncate_inode_pages_range(mapping, range.start,
			to_end ? -1 : end_addr - 1);

	while (index < pg_end) {
		struct dnode_of_data dn;
		pgoff_t end_offset, count;
		int i;

		set_new_dnode(&dn, inode, NULL, NULL, 0);
		ret = f2fs_get_dnode_of_data(&dn, index, LOOKUP_NODE);
		if (ret) {
			if (ret == -ENOENT) {
				index = f2fs_get_next_page_offset(&dn, index);
				continue;
			}
			goto out;
		}

		end_offset = ADDRS_PER_PAGE(dn.node_page, inode);
		count = min(end_offset - dn.ofs_in_node, pg_end - index);
		for (i = 0; i < count; i++, index++, dn.ofs_in_node++) {
			struct block_device *cur_bdev;
			block_t blkaddr = f2fs_data_blkaddr(&dn);

			if (!__is_valid_data_blkaddr(blkaddr))
				continue;

			if (!f2fs_is_valid_blkaddr(sbi, blkaddr,
						DATA_GENERIC_ENHANCE)) {
				ret = -EFSCORRUPTED;
				f2fs_put_dnode(&dn);
				f2fs_handle_error(sbi,
						ERROR_INVALID_BLKADDR);
				goto out;
			}

			cur_bdev = f2fs_target_device(sbi, blkaddr, NULL);
			if (f2fs_is_multi_device(sbi)) {
				int di = f2fs_target_device_index(sbi, blkaddr);

				blkaddr -= FDEV(di).start_blk;
			}

			if (len) {
				if (prev_bdev == cur_bdev &&
						index == prev_index + len &&
						blkaddr == prev_block + len) {
					len++;
				} else {
					ret = f2fs_secure_erase(prev_bdev,
						inode, prev_index, prev_block,
						len, range.flags);
					if (ret) {
						f2fs_put_dnode(&dn);
						goto out;
					}

					len = 0;
				}
			}

			if (!len) {
				prev_bdev = cur_bdev;
				prev_index = index;
				prev_block = blkaddr;
				len = 1;
			}
		}

		f2fs_put_dnode(&dn);

		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			goto out;
		}
		cond_resched();
	}

	if (len)
		ret = f2fs_secure_erase(prev_bdev, inode, prev_index,
				prev_block, len, range.flags);
out:
	filemap_invalidate_unlock(mapping);
	f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
err:
	inode_unlock(inode);
	file_end_write(filp);

	return ret;
}

static int f2fs_get_compress_option_v2(struct file *filp,
				       unsigned long attr, __u16 *attr_size)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_comp_option_v2 option;

	if (sizeof(option) < *attr_size)
		*attr_size = sizeof(option);

	if (!f2fs_sb_has_compression(F2FS_I_SB(inode)))
		return -EOPNOTSUPP;

	inode_lock_shared(inode);

	if (!f2fs_compressed_file(inode)) {
		inode_unlock_shared(inode);
		return -ENODATA;
	}

	option.algorithm = F2FS_I(inode)->i_compress_algorithm;
	option.log_cluster_size = F2FS_I(inode)->i_log_cluster_size;
	option.level = F2FS_I(inode)->i_compress_level;
	option.flag = F2FS_I(inode)->i_compress_flag;

	inode_unlock_shared(inode);

	if (copy_to_user((void __user *)attr, &option, *attr_size))
		return -EFAULT;

	return 0;
}

static int f2fs_ioc_get_compress_option(struct file *filp, unsigned long arg)
{
	__u16 size = sizeof(struct f2fs_comp_option);

	return f2fs_get_compress_option_v2(filp, arg, &size);
}

static int f2fs_set_compress_option_v2(struct file *filp,
				       unsigned long attr, __u16 *attr_size)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_comp_option_v2 option;
	/*
	 * if compress_layout is not set, set file in fixed-input mode.
	 * no need to shift COMPRESS_LEVEL
	 */
	short init_compr_flag = COMPRESS_FIXED_INPUT;
	int ret = 0;

	if (sizeof(option) < *attr_size)
		*attr_size = sizeof(option);

	if (!f2fs_sb_has_compression(sbi))
		return -EOPNOTSUPP;

	if (!(filp->f_mode & FMODE_WRITE))
		return -EBADF;

	if (copy_from_user(&option, (void __user *)attr, *attr_size))
		return -EFAULT;

	if (option.log_cluster_size < MIN_COMPRESS_LOG_SIZE ||
		option.log_cluster_size > MAX_COMPRESS_LOG_SIZE ||
		option.algorithm >= COMPRESS_MAX)
		return -EINVAL;

	if (*attr_size == sizeof(struct f2fs_comp_option_v2)) {
		if (!f2fs_is_compress_level_valid(option.algorithm,
						  option.level))
			return -EINVAL;
		/* fix coverity error: Operands don't affect result, COMPRESS_MAX_FLAG==9, always false*/
		//if (option.flag > BIT(COMPRESS_MAX_FLAG) - 1)
		//	return -EINVAL;
	}

	file_start_write(filp);
	inode_lock(inode);

	f2fs_down_write(&F2FS_I(inode)->i_sem);
	if (!f2fs_compressed_file(inode)) {
		ret = -EINVAL;
		goto out;
	}

	if (f2fs_is_mmap_file(inode) || get_dirty_pages(inode)) {
		ret = -EBUSY;
		goto out;
	}

	if (F2FS_HAS_BLOCKS(inode)) {
		if (*attr_size == sizeof(struct f2fs_comp_option_v2) &&
		    F2FS_I(inode)->i_compress_algorithm == option.algorithm &&
		    F2FS_I(inode)->i_log_cluster_size == option.log_cluster_size &&
		    F2FS_I(inode)->i_compress_level == option.level) {
#ifdef CONFIG_F2FS_FS_COMPRESSION_FIXED_OUTPUT
			if (option.flag & COMPRESS_ATIME_MASK) {
				F2FS_I(inode)->i_compress_flag |= BIT(COMPRESS_ATIME);
				inode->i_atime = current_time(inode);
			}
#endif
			goto mark_dirty;
		}
		ret = -EFBIG;
		goto out;
	}

	F2FS_I(inode)->i_compress_algorithm = option.algorithm;
	F2FS_I(inode)->i_log_cluster_size = option.log_cluster_size;
	F2FS_I(inode)->i_cluster_size = BIT(option.log_cluster_size);
	if (F2FS_I(inode)->i_compress_flag & COMPRESS_CHKSUM_MASK)
		init_compr_flag |= BIT(COMPRESS_CHKSUM);
	if (F2FS_I(inode)->i_compress_flag & COMPRESS_ATIME_MASK)
		init_compr_flag |= BIT(COMPRESS_ATIME);
	F2FS_I(inode)->i_compress_flag = init_compr_flag;
	if (*attr_size == sizeof(struct f2fs_comp_option_v2)) {
		F2FS_I(inode)->i_compress_level = option.level;
		F2FS_I(inode)->i_compress_flag = option.flag;
		if (f2fs_compress_layout(inode) == COMPRESS_FIXED_OUTPUT)
			F2FS_I(inode)->i_compress_flag &= ~COMPRESS_CHKSUM_MASK;
#ifdef CONFIG_F2FS_FS_COMPRESSION_FIXED_OUTPUT
		if (option.flag & COMPRESS_ATIME_MASK)
			inode->i_atime = current_time(inode);
#endif
	}
mark_dirty:
	f2fs_mark_inode_dirty_sync(inode, true);

	if (!f2fs_is_compress_backend_ready(inode))
		f2fs_warn(sbi, "compression algorithm is successfully set, "
			"but current kernel doesn't support this algorithm.");
out:
	f2fs_up_write(&F2FS_I(inode)->i_sem);
	inode_unlock(inode);
	file_end_write(filp);

	return ret;
}

static int f2fs_ioc_set_compress_option(struct file *filp, unsigned long arg)
{
	__u16 size = sizeof(struct f2fs_comp_option);

	return f2fs_set_compress_option_v2(filp, arg, &size);
}

static int redirty_blocks(struct inode *inode, pgoff_t page_idx, int len)
{
	DEFINE_READAHEAD(ractl, NULL, NULL, inode->i_mapping, page_idx);
	struct address_space *mapping = inode->i_mapping;
	struct page *page;
	pgoff_t redirty_idx = page_idx;
	int i, page_len = 0, ret = 0;

	page_cache_ra_unbounded(&ractl, len, 0);

	for (i = 0; i < len; i++, page_idx++) {
		if (time_to_inject(F2FS_M_SB(mapping), FAULT_COMPRESS_REDIRTY)) {
			ret = -ENOMEM;
			break;
		}
		page = read_cache_page(mapping, page_idx, NULL, NULL);
		if (IS_ERR(page)) {
			ret = PTR_ERR(page);
			break;
		}
		page_len++;
	}

	for (i = 0; i < page_len; i++, redirty_idx++) {
		page = find_lock_page(mapping, redirty_idx);

		/* It will never fail, when page has pinned above */
		f2fs_bug_on(F2FS_I_SB(inode), !page);

		f2fs_wait_on_page_writeback(page, DATA, true, true);

		set_page_dirty(page);
		f2fs_put_page(page, 1);
		f2fs_put_page(page, 0);
	}

	return ret;
}

int f2fs_decompress_inode(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	pgoff_t page_idx = 0, last_idx, cluster_idx;
	unsigned int blk_per_seg = sbi->blocks_per_seg;
	int ret;

	if (is_inode_flag_set(inode, FI_COMPRESS_RELEASED))
		return -EPERM;

	ret = filemap_write_and_wait_range(inode->i_mapping, 0, LLONG_MAX);
	if (ret)
		return ret;

	if (!atomic_read(&fi->i_compr_blocks))
		return 0;

	f2fs_info(sbi, "start decompress ino %lu size %llu blocks %llu "
		"cblocks %d caller %ps\n", inode->i_ino, i_size_read(inode),
		inode->i_blocks, atomic_read(&F2FS_I(inode)->i_compr_blocks),
		__builtin_return_address(0));

	clear_inode_flag(inode, FI_ENABLE_COMPRESS);
	inode->i_ctime = current_time(inode);

	last_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	last_idx >>= fi->i_log_cluster_size;

	for (cluster_idx = 0; cluster_idx < last_idx; cluster_idx++) {
		page_idx = cluster_idx << fi->i_log_cluster_size;

		if (!f2fs_is_compressed_cluster(inode, page_idx))
			continue;

		ret = redirty_blocks(inode, page_idx, fi->i_cluster_size);
		if (ret < 0)
			break;

		if (get_dirty_pages(inode) >= blk_per_seg) {
			ret = filemap_fdatawrite(inode->i_mapping);
			if (ret < 0)
				break;
		}

		cond_resched();
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}
	}

	if (!ret) {
		if (time_to_inject(sbi, FAULT_COMPRESS_WRITEBACK)) {
			ret = filemap_write_and_wait_range(inode->i_mapping, 0,
							i_size_read(inode) / 2);
			if (!ret)
				ret = -EIO;
			goto out;
		}
		ret = filemap_write_and_wait_range(inode->i_mapping, 0,
							LLONG_MAX);
	}

out:
	if (ret)
		f2fs_warn(sbi, "%s: The file might be partially decompressed (errno=%d). Please delete the file.",
			  __func__, ret);
	f2fs_info(sbi, "end decompress ino %lu size %llu blocks %llu cblocks %d ret %d\n",
		inode->i_ino, i_size_read(inode), inode->i_blocks,
		atomic_read(&F2FS_I(inode)->i_compr_blocks), ret);

	return ret;
}

static int f2fs_ioc_decompress_file(struct file *filp)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	int ret;

	if (!f2fs_sb_has_compression(sbi) ||
			F2FS_OPTION(sbi).compress_mode != COMPR_MODE_USER)
		return -EOPNOTSUPP;

	if (!(filp->f_mode & FMODE_WRITE))
		return -EBADF;

	if (!f2fs_compressed_file(inode))
		return -EINVAL;

	f2fs_balance_fs(F2FS_I_SB(inode), true);

	file_start_write(filp);
	inode_lock(inode);

#ifdef CONFIG_F2FS_FS_DEDUP
	if (f2fs_is_deduped_inode(inode)) {
		ret = -EACCES;
		goto out;
	}
#endif

	if (!f2fs_is_compress_backend_ready(inode)) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (is_inode_flag_set(inode, FI_COMPRESS_RELEASED)) {
		ret = -EINVAL;
		goto out;
	}

	ret = f2fs_decompress_inode(inode);
out:
	inode_unlock(inode);
	file_end_write(filp);

	return ret;
}

static int f2fs_ioc_compress_file(struct file *filp)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	pgoff_t page_idx = 0, last_idx, cluster_idx;
	unsigned int blk_per_seg = sbi->blocks_per_seg;
	int ret;

	if (!f2fs_sb_has_compression(sbi) || !may_compress ||
			F2FS_OPTION(sbi).compress_mode != COMPR_MODE_USER)
		return -EOPNOTSUPP;

	if (!(filp->f_mode & FMODE_WRITE))
		return -EBADF;

	if (!f2fs_compressed_file(inode))
		return -EINVAL;

	f2fs_balance_fs(sbi, true);

	file_start_write(filp);
	inode_lock(inode);

#ifdef CONFIG_F2FS_FS_DEDUP
	if (f2fs_is_deduped_inode(inode)) {
		ret = -EACCES;
		goto out;
	}
#endif

	if (!f2fs_is_compress_backend_ready(inode)) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	f2fs_info(sbi, "compress ino %lu (%pd) size %llu blocks %llu released %d\n",
		inode->i_ino, file_dentry(filp), i_size_read(inode),
		inode->i_blocks, is_inode_flag_set(inode, FI_COMPRESS_RELEASED));

	if (is_inode_flag_set(inode, FI_COMPRESS_RELEASED)) {
		ret = -EINVAL;
		goto out;
	}

	inode->i_ctime = current_time(inode);

	ret = filemap_write_and_wait_range(inode->i_mapping, 0, LLONG_MAX);
	if (ret)
		goto out;

	set_inode_flag(inode, FI_ENABLE_COMPRESS);

	last_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	last_idx >>= fi->i_log_cluster_size;

	for (cluster_idx = 0; cluster_idx < last_idx; cluster_idx++) {
		page_idx = cluster_idx << fi->i_log_cluster_size;

		if (f2fs_is_sparse_cluster(inode, page_idx))
			continue;

		ret = redirty_blocks(inode, page_idx, fi->i_cluster_size);
		if (ret < 0)
			break;

		if (get_dirty_pages(inode) >= blk_per_seg) {
			ret = filemap_fdatawrite(inode->i_mapping);
			if (ret < 0)
				break;
		}

		cond_resched();
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}
	}

	if (!ret) {
		if (time_to_inject(sbi, FAULT_COMPRESS_WRITEBACK)) {
			ret = filemap_write_and_wait_range(inode->i_mapping, 0,
							i_size_read(inode) / 2);
			if (!ret)
				ret = -EIO;
			goto next;
		}
		ret = filemap_write_and_wait_range(inode->i_mapping, 0,
							LLONG_MAX);
	}
next:

#ifdef CONFIG_F2FS_FS_COMPRESSION_FIXED_OUTPUT
	if (!atomic_read(&F2FS_I(inode)->i_compr_blocks)) {
		clear_inode_flag(inode, FI_ENABLE_COMPRESS);
		F2FS_I(inode)->i_flags |= F2FS_NOCOMP_FL;
		f2fs_mark_inode_dirty_sync(inode, true);
	}
#else
	clear_inode_flag(inode, FI_ENABLE_COMPRESS);
#endif

	if (ret)
		f2fs_warn(sbi, "%s: The file might be partially compressed (errno=%d). Please delete the file.",
			  __func__, ret);
out:
	f2fs_info(sbi, "end compress ino %lu (%pd) size %llu blocks %llu cblocks %d ret %d\n",
		inode->i_ino, file_dentry(filp), i_size_read(inode),
		inode->i_blocks, atomic_read(&F2FS_I(inode)->i_compr_blocks), ret);

	inode_unlock(inode);
	file_end_write(filp);

	return ret;
}

#ifdef CONFIG_F2FS_APPBOOST
#define BOOST_MAX_FILES 1019
#define BOOST_FILE_STATE_FINISH 1
#define F2FS_BOOSTFILE_VERSION 0xF2F5
#define BOOSTFILE_MAX_BITMAP (1<<20)
#define PRELOAD_MAX_TIME	(2000)

/* structure on disk */
struct merge_summary_dinfo {
	__le32 num;
	__le32 version;
	__le32 state;
	__le32 tail;
	__le32 checksum;
	__le32 fsize[BOOST_MAX_FILES];
};

struct merge_extent_dinfo {
	__le32 index;
	__le32 length;
	__le32 index_in_mfile;
};

struct merge_file_dinfo {
	__le32 ino;
	__le32 extent_count;
	__le32 i_generation;
	__le32 REV;
	__le64 mtime;
	struct merge_extent_dinfo extents[0];
};

/* inmem manage structure */
struct merge_summary {
	int num;
	int version;
	int state;
	int tail;
	u32 checksum;
	int fsize[BOOST_MAX_FILES];
};

struct merge_extent {
	unsigned index;
	unsigned length;
	unsigned index_in_mfile;
};

struct merge_file {
	unsigned ino;
	unsigned extent_count;
	unsigned i_generation;
	unsigned REV;
	u64 mtime;
	struct merge_extent extents[0];
};

/* manage structure in f2fs inode info */
struct fi_merge_manage {
	int num;
	unsigned long cur_blocks;
	struct list_head list;
};

struct file_list_node {
	struct list_head list;
	struct list_head ext_list;
	u64 bitmax;
	unsigned long *bitmap;
	struct merge_file merge_file;
};

struct extent_list_node {
	struct list_head list;
	struct merge_extent extent;
};

static bool f2fs_appboost_enable(struct f2fs_sb_info *sbi)
{
	return sbi->appboost;
}

static unsigned int f2fs_appboost_maxblocks(struct f2fs_sb_info *sbi)
{
	return sbi->appboost_max_blocks;
}

static int f2fs_file_read(struct file *file, loff_t offset, unsigned char *data, unsigned int size)
{
	struct inode *inode = file_inode(file);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	if (time_to_inject(sbi, FAULT_READ_ERROR)) {
		return -EIO;
	}
	return kernel_read(file, data, size, &offset);
}

static int f2fs_file_write(struct file *file, loff_t off, unsigned char *data, unsigned int size)
{
	struct inode *inode = file_inode(file);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct address_space *mapping = inode->i_mapping;
	const struct address_space_operations *a_ops = mapping->a_ops;
	loff_t offset = off & (PAGE_SIZE - 1);
	size_t towrite = size;
	struct page *page;
	void *fsdata = NULL;
	char *kaddr;
	int err = 0;
	int tocopy;

	if (time_to_inject(sbi, FAULT_WRITE_ERROR)) {
		return -EIO;
	}
	// if no set this, prepare_write_begin will return 0 directly and get the error block
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
	set_inode_flag(inode, FI_NO_PREALLOC);
#endif
	while (towrite > 0) {
		tocopy = min_t(unsigned long, PAGE_SIZE - offset, towrite);
retry:
		err = a_ops->write_begin(NULL, mapping, off, tocopy,
								&page, &fsdata);
		if (unlikely(err)) {
			if (err == -ENOMEM) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
				congestion_wait(BLK_RW_ASYNC,
							DEFAULT_IO_TIMEOUT);
#else
				f2fs_io_schedule_timeout(DEFAULT_IO_TIMEOUT);
#endif
				goto retry;
			}
			break;
		}

		kaddr = kmap_atomic(page);
		memcpy(kaddr + offset, data, tocopy);
		kunmap_atomic(kaddr);
		flush_dcache_page(page);
		a_ops->write_end(NULL, mapping, off, tocopy, tocopy,
							page, fsdata);
		offset = 0;
		towrite -= tocopy;
		off += tocopy;
		data += tocopy;
		cond_resched();
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
	clear_inode_flag(inode, FI_NO_PREALLOC);
#endif
	if (size == towrite)
		return err;

	inode->i_mtime = inode->i_ctime = current_time(inode);
	f2fs_mark_inode_dirty_sync(inode, false);

	return size - towrite;
}

static struct fi_merge_manage *f2fs_init_merge_manage(struct inode *inode)
{
	struct fi_merge_manage *fmm;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	fmm = f2fs_kmalloc(sbi, sizeof(struct fi_merge_manage), GFP_KERNEL);
	if (!fmm) {
		return NULL;
	}

	INIT_LIST_HEAD(&fmm->list);
	fmm->num = 0;
	fmm->cur_blocks = 0;

	return fmm;
}

void f2fs_boostfile_free(struct inode *inode)
{
	struct fi_merge_manage *fmm;
	struct file_list_node *fm_node, *fm_tmp;
	struct extent_list_node *fm_ext_node, *fm_ext_tmp;
	struct f2fs_inode_info *fi = F2FS_I(inode);

	if (!fi->i_boostfile) {
		return;
	}

	fmm = (struct fi_merge_manage *)fi->i_boostfile;
	list_for_each_entry_safe(fm_node, fm_tmp, &fmm->list, list) {
		kvfree(fm_node->bitmap);

		list_for_each_entry_safe(fm_ext_node, fm_ext_tmp, &fm_node->ext_list, list) {
			list_del(&fm_ext_node->list);
			kfree(fm_ext_node);
		}

		list_del(&fm_node->list);
		kfree(fm_node);
	}
	kfree(fmm);
	fi->i_boostfile = NULL;
}

static struct file_list_node *f2fs_search_merge_file(struct fi_merge_manage *fmm, unsigned ino)
{
	struct file_list_node *fm_node, *fm_tmp;

	list_for_each_entry_safe(fm_node, fm_tmp, &fmm->list, list) {
		if (fm_node->merge_file.ino == ino) {
			return fm_node;
		}
	}

	return NULL;
}

static int _f2fs_insert_merge_extent(struct f2fs_sb_info* sbi,
				     struct file_list_node *fm_node, unsigned start,
				     unsigned end, struct fi_merge_manage *fmm, u32 max_blocks)
{
	struct extent_list_node *ext_node;
	struct extent_list_node *ext_tail;
	unsigned length;

	if (fmm->cur_blocks >= max_blocks)
		return -EOVERFLOW;

	// update the end
	if (((end - start + 1) + fmm->cur_blocks) >= max_blocks)
		end = (max_blocks - fmm->cur_blocks) + start - 1;

	length = end - start + 1;
	ext_tail = list_last_entry(&(fm_node->ext_list), struct extent_list_node, list);
	if (ext_tail) {
		if (start == (ext_tail->extent.index + ext_tail->extent.length)) {
			ext_tail->extent.length += length;
			fmm->cur_blocks += length;
			return 0;
		}
	}

	ext_node = (struct extent_list_node *)f2fs_kmalloc(sbi, sizeof(struct extent_list_node), GFP_KERNEL);
	if (!ext_node) {
		return -ENOMEM;
	}
	ext_node->extent.index = start;
	ext_node->extent.length = length;
	ext_node->extent.index_in_mfile = 0;

	INIT_LIST_HEAD(&ext_node->list);
	list_add_tail(&(ext_node->list), &(fm_node->ext_list));
	fm_node->merge_file.extent_count++;
	fmm->cur_blocks += length;

	return 0;

}

static int f2fs_insert_merge_extent(struct fi_merge_manage *fmm, struct f2fs_sb_info *sbi,
				 struct file_list_node *fm_node, struct merge_extent *fm_ext)
{
	unsigned low = fm_ext->index;
	unsigned high = low + fm_ext->length - 1;
	unsigned start = 1, end = 0;
	unsigned i;
	bool spliting = false;
	int ret = 0;

	if (high >= fm_node->bitmax) {
		f2fs_warn(sbi, "f2fs_insert_merge_extent range bad value [%u,%u,%llu]",
							low, high, fm_node->bitmax);
		return -EINVAL;
	}

	for (i = low; i <= high; i++) {
		if (test_and_set_bit(i, fm_node->bitmap)) {
			if (spliting) {
				ret = _f2fs_insert_merge_extent(sbi, fm_node, start,
						end, fmm, f2fs_appboost_maxblocks(sbi));
				if (ret)
					return ret;

				// reset
				spliting = false;
				start = 1;
				end = 0;
			} else {
				continue;
			}
		} else {
			if (spliting) {
				end = i;
				continue;
			} else {
				start = i;
				end = i;
				spliting = true;
			}
		}
	}

	if (end >= start && spliting)
		ret = _f2fs_insert_merge_extent(sbi, fm_node, start,
						end, fmm, f2fs_appboost_maxblocks(sbi));

	return ret;
}

static int f2fs_insert_merge_file_user(struct fi_merge_manage *fmm, struct f2fs_sb_info *sbi,
					struct file_list_node *fm_node, struct merge_file_user *fm_u)
{
	int ret = 0;
	struct merge_extent *fm_ext = NULL;
	int i;

	fm_ext = (struct merge_extent *)f2fs_kvmalloc(sbi,
				sizeof(struct merge_extent) * fm_u->extent_count, GFP_KERNEL);
	if (!fm_ext) {
		ret = -ENOMEM;
		goto fail;
	}

	if (copy_from_user(fm_ext, (struct merge_extent __user *)fm_u->extents,
		sizeof(struct merge_extent) * fm_u->extent_count)) {
		ret = -EFAULT;
		goto fail;
	}

	for (i = 0; i < fm_u->extent_count; i++) {
		if (fm_ext[i].length == 0) {
			f2fs_warn(sbi, "f2fs_ioc_merge_user check ext length == 0!");
			ret = -EINVAL;
			goto fail;
		}

		ret = f2fs_insert_merge_extent(fmm, sbi, fm_node, &fm_ext[i]);
		if (ret) {
			goto fail;
		}
	}

fail:
	if (fm_ext)
		kvfree(fm_ext);

	return ret;
}

static int f2fs_ioc_start_file_merge(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct inode *inode_source;
	struct fi_merge_manage *fmm;
	struct file_list_node *fm_node;
	struct merge_file_user fm_u;
	int ret = 0;
	loff_t i_size;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	if  (!f2fs_appboost_enable(sbi))
		return -ENOTTY;

	if (!inode_trylock(inode))
		return -EAGAIN;

	if (!(filp->f_flags & __O_TMPFILE)) {
		f2fs_warn(sbi, "f2fs_ioc_start_file_merge check flags failed!");
		inode_unlock(inode);
		return -EINVAL;
	}

	if (!fi->i_boostfile) {
		fi->i_boostfile = f2fs_init_merge_manage(inode);
		if (!fi->i_boostfile) {
			f2fs_warn(sbi, "f2fs_ioc_start_file_merge init private failed!");
			inode_unlock(inode);
			return -ENOMEM;
		}
	}

	fmm = fi->i_boostfile;
	if (fmm->num >= BOOST_MAX_FILES) {
		f2fs_warn(sbi, "f2fs_ioc_start_file_merge num overflow!");
		ret = -EFAULT;
		goto fail;
	}

	if (copy_from_user(&fm_u, (struct merge_file_user __user *)arg,
		sizeof(struct merge_file_user))) {
		ret = -EFAULT;
		goto fail;
	}

	fm_node = f2fs_search_merge_file(fmm, fm_u.ino);
	if (!fm_node) {
		inode_source = f2fs_iget(sbi->sb, fm_u.ino);
		if (IS_ERR(inode_source)) {
			ret = PTR_ERR(inode_source);
			f2fs_warn(sbi, "f2fs_ioc_start_file_merge no found ino=%d", fm_u.ino);
			goto fail;
		}

		if (is_bad_inode(inode_source)) {
			iput(inode_source);
			ret = -EKEYEXPIRED;
			goto fail;
		}

		if (!inode_trylock(inode_source)) {
			iput(inode_source);
			ret = -EAGAIN;
			goto fail;
		}

		i_size = i_size_read(inode_source);
		if (DIV_ROUND_UP(i_size, PAGE_SIZE) > BOOSTFILE_MAX_BITMAP) {
			f2fs_warn(sbi, "f2fs_ioc_start_file_merge ino=%d, i_size=%lld", fm_u.ino, i_size);
			inode_unlock(inode_source);
			iput(inode_source);
			ret = -EFAULT;
			goto fail;
		}

		if (fm_u.mtime != timespec64_to_ns(&inode_source->i_mtime) ||
			fm_u.i_generation != inode_source->i_generation) {
			f2fs_warn(sbi, "f2fs_ioc_start_file_merge EKEYEXPIRED ino=%d", fm_u.ino);
			inode_unlock(inode_source);
			iput(inode_source);
			ret = -EKEYEXPIRED;
			goto fail;
		}
#ifdef CONFIG_F2FS_FS_DEDUP
		if (f2fs_inode_support_dedup(sbi, inode_source)
		    && !is_inode_flag_set(inode_source, FI_DATA_UN_MODIFY))
			set_inode_flag(inode_source, FI_DATA_UN_MODIFY);
#endif
		inode_unlock(inode_source);
		iput(inode_source);
		fm_node = (struct file_list_node *)f2fs_kmalloc(sbi,
						sizeof(struct file_list_node), GFP_KERNEL);
		if (!fm_node) {
			ret = -ENOMEM;
			goto fail;
		}
		fm_node->merge_file.ino = fm_u.ino;
		fm_node->merge_file.extent_count = 0;
		fm_node->merge_file.mtime = fm_u.mtime;
		fm_node->merge_file.i_generation = fm_u.i_generation;
		fm_node->bitmax = DIV_ROUND_UP(i_size, PAGE_SIZE);
		fm_node->bitmap = (unsigned long*)f2fs_kvzalloc(sbi,
						f2fs_bitmap_size(fm_node->bitmax), GFP_KERNEL);
		if (!fm_node->bitmap) {
			kfree(fm_node);
			fm_node = NULL;
			ret = -ENOMEM;
			goto fail;
		}

		INIT_LIST_HEAD(&(fm_node->ext_list));
		list_add_tail(&(fm_node->list), &(fmm->list));
		fmm->num++;
	} else {
		if (fm_node->merge_file.i_generation != fm_u.i_generation ||
			fm_node->merge_file.mtime  != fm_u.mtime) {
			ret = -EKEYEXPIRED;
			goto fail;
		}
	}

	if (fm_u.extent_count > f2fs_appboost_maxblocks(sbi)) {
		ret = -EKEYEXPIRED;
		goto fail;
	}

	ret = f2fs_insert_merge_file_user(fmm, sbi, fm_node, &fm_u);
	// if return EOVERFLOW, we support max blocks
	if (ret == -EOVERFLOW)
		ret = 0;
fail:
	inode_unlock(inode);
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
static void f2fs_file_read_pages(struct inode *inode)
{
	struct backing_dev_info *bdi;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	DEFINE_READAHEAD(ractl, NULL, NULL, inode->i_mapping, 0);
#else
	DEFINE_READAHEAD(ractl, NULL, inode->i_mapping, 0);
#endif
	unsigned long max_blocks = (inode->i_size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	unsigned long nr_to_read = 0;
	unsigned long index = 0;

	bdi = inode_to_bdi(inode);
	if (!bdi)
		return;

	while (1) {
		ractl._index = index;
		/* equal to POSIX_FADV_SEQUENTIAL */
		nr_to_read = min(2 * bdi->ra_pages, max_blocks - index + 1);
		page_cache_ra_unbounded(&ractl, nr_to_read, 0);

		index += nr_to_read;
		if (index >= max_blocks)
			return;
	}
}
#endif

static int merge_sync_file(struct f2fs_sb_info *sbi, struct file *file)
{
	int ret = 0;
	struct inode *inode = file->f_mapping->host;

	if (time_to_inject(sbi, FAULT_FSYNC_ERROR)) {
		ret = 0;
	} else {
		ret = f2fs_do_sync_file(file, 0, LLONG_MAX, 0, 0);
	}

	if (ret != 0) {
		f2fs_err(sbi, "f2fs_end_file_merge:failed to sync");
		return ret;
	}

	if (time_to_inject(sbi, FAULT_FLUSH_ERROR)) {
		ret = 0;
	} else {
		ret = f2fs_issue_flush(sbi, inode->i_ino);
	}

	if (ret != 0)
		f2fs_err(sbi, "f2fs_end_file_merge:failed to flush");

	return ret;
}

static void copy_summary_info_to_disk(struct merge_summary *summary,
					struct merge_summary_dinfo *summary_dinfo)
{
	if (!summary || !summary_dinfo)
		return;

	summary_dinfo->version = cpu_to_le32(summary->version);
	summary_dinfo->state = cpu_to_le32(summary->state);
	summary_dinfo->tail = cpu_to_le32(summary->tail);
	summary_dinfo->checksum = cpu_to_le32(summary->checksum);
	summary_dinfo->num = cpu_to_le32(summary->num);
}

static void copy_file_merge_info_to_disk(struct merge_file *info, struct merge_file_dinfo *dinfo)
{
	if (!info || !dinfo)
		return;

	dinfo->ino = cpu_to_le32(info->ino);
	dinfo->extent_count = cpu_to_le32(info->extent_count);
	dinfo->i_generation = cpu_to_le32(info->i_generation);
	dinfo->mtime = cpu_to_le64(info->mtime);
}

static void copy_extent_info_to_disk(struct merge_extent *extent,
					struct merge_extent_dinfo *extent_dinfo)
{
	if (!extent || !extent_dinfo)
		return;

	extent_dinfo->index = cpu_to_le32(extent->index);
	extent_dinfo->length = cpu_to_le32(extent->length);
	extent_dinfo->index_in_mfile = cpu_to_le32(extent->index_in_mfile);
}

static int end_file_merge(struct file *filp, unsigned long arg)
{
	struct merge_summary *summary = NULL;
	struct merge_summary_dinfo *summary_dinfo = NULL;
	struct fi_merge_manage *fmm;
	struct file_list_node *fm_node, *fm_tmp;
	struct extent_list_node *fm_ext_node, *fm_ext_tmp;
	struct merge_file *cur_merge_file;
	struct file_list_node **merge_files_lists = NULL;
	struct inode *inode = file_inode(filp);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct inode *inode_source;
	struct page *page = NULL;
	unsigned char *addr = NULL;
	unsigned long merged_blocks = 0;
	unsigned int max_blocks = f2fs_appboost_maxblocks(sbi);
	struct merge_extent_dinfo extent_dinfo;
	struct merge_file_dinfo file_dinfo;
	loff_t offset = 0;
	loff_t tail = 0;
	int ret, i, k, result;

	/* disk free space is not enough */
	if (has_not_enough_free_secs(sbi, 0, max_blocks >> sbi->log_blocks_per_seg))
		return -ENOSPC;

	if (!inode_trylock(inode))
		return -EAGAIN;

	if (!(filp->f_flags & __O_TMPFILE)) {
		f2fs_warn(sbi, "f2fs_ioc_end_file_merge check flags failed!");
		ret = -EINVAL;
		goto fail;
	}

	// when end and private is NULL, will return err
	if (!fi->i_boostfile) {
		f2fs_warn(sbi, "f2fs_ioc_end_file_merge: i_boostfile is null");
		ret = -EFAULT;
		goto fail;
	}

	fmm = (struct fi_merge_manage *)fi->i_boostfile;
	merge_files_lists = (struct file_list_node **)f2fs_kvmalloc(sbi,
				sizeof(struct file_list_node *) * fmm->num, GFP_KERNEL);
	if (!merge_files_lists) {
		f2fs_warn(sbi, "f2fs_ioc_end_file_merge: merge_files_lists is null");
		ret = -ENOMEM;
		goto fail;
	}

	// 1. fill summary.
	summary = f2fs_kzalloc(sbi, sizeof(struct merge_summary), GFP_KERNEL);
	if (!summary) {
		f2fs_warn(sbi, "f2fs_ioc_end_file_merge: summary is null");
		ret = -ENOMEM;
		goto fail;
	}

	summary_dinfo = f2fs_kzalloc(sbi, sizeof(struct merge_summary_dinfo), GFP_KERNEL);
	if (!summary_dinfo) {
		f2fs_warn(sbi, "f2fs_ioc_end_file_merge: summary_dinfo is null");
		ret = -ENOMEM;
		goto fail;
	}

	list_for_each_entry_safe(fm_node, fm_tmp, &fmm->list, list) {
		u64 size = fm_node->merge_file.extent_count *
				sizeof(struct merge_extent_dinfo) + sizeof(struct merge_file_dinfo);
		if (unlikely(size > INT_MAX)) {
			ret = -EINVAL;
			goto fail;
		}
		summary->fsize[summary->num] = size;
		summary_dinfo->fsize[summary->num] = cpu_to_le32(size);
		merge_files_lists[summary->num] = fm_node;
		summary->num++;
	}

	// calc the offset
	offset = sizeof(struct merge_summary);
	for (i = 0; i < fmm->num; i++) {
		offset += summary->fsize[i];
	}
	// align PAGE_SIZE
	offset = (offset + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	// write file
	for (i = 0; i < fmm->num; i++) {
		cur_merge_file = &(merge_files_lists[i]->merge_file);
		inode_source = f2fs_iget(sbi->sb, cur_merge_file->ino);
		if (IS_ERR(inode_source)) {
			f2fs_warn(sbi, "f2fs_ioc_end_file_merge NOFOUND ino=%d", cur_merge_file->ino);
			ret = PTR_ERR(inode_source);
			goto fail;
		}

		if (is_bad_inode(inode_source)) {
			iput(inode_source);
			ret = -EKEYEXPIRED;
			goto fail;
		}

		if (!inode_trylock(inode_source)) {
			iput(inode_source);
			ret = -EAGAIN;
			goto fail;
		}

		if (!S_ISREG(inode_source->i_mode)) {
			inode_unlock(inode_source);
			iput(inode_source);
			ret = -EKEYEXPIRED;
			goto fail;
		}
		if (cur_merge_file->mtime != timespec64_to_ns(&inode_source->i_mtime)) {
			f2fs_warn(sbi, "f2fs_ioc_end_file_merge mtime expired ino = %u, new_time = %llu, expired_time = %llu",
						cur_merge_file->ino, timespec64_to_ns(&inode_source->i_mtime), cur_merge_file->mtime);
			inode_unlock(inode_source);
			iput(inode_source);
			ret = -EKEYEXPIRED;
			goto fail;
		}

		if (cur_merge_file->i_generation != inode_source->i_generation) {
			f2fs_warn(sbi, "f2fs_ioc_end_file_merge i_generation has been changed ino = %u!",  cur_merge_file->ino);
			inode_unlock(inode_source);
			iput(inode_source);
			ret = -EKEYEXPIRED;
			goto fail;
		}

#ifdef CONFIG_F2FS_FS_DEDUP
		if (f2fs_inode_support_dedup(sbi, inode_source) &&
			!is_inode_flag_set(inode_source, FI_DATA_UN_MODIFY)) {
			f2fs_warn(sbi, "f2fs_ioc_end_file_merge FI_DATA_UN_MODIFY flag has been changed ino = %u!",  cur_merge_file->ino);
                        inode_unlock(inode_source);
                        iput(inode_source);
                        ret = -EKEYEXPIRED;
                        goto fail;
		}
#endif

		ret = fscrypt_require_key(inode_source);
		if (ret) {
			f2fs_warn(sbi, "f2fs_ioc_end_file_merge get file ino = %lu encrypt info failed\n", inode_source->i_ino);
			inode_unlock(inode_source);
			iput(inode_source);
			goto fail;
		}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
		f2fs_file_read_pages(inode_source);
#endif
		list_for_each_entry_safe(fm_ext_node, fm_ext_tmp, &merge_files_lists[i]->ext_list, list) {
			fm_ext_node->extent.index_in_mfile = offset >> PAGE_SHIFT;

			for (k = 0; k < fm_ext_node->extent.length; k++) {
				if (time_to_inject(sbi, FAULT_PAGE_ERROR)) {
					page = ERR_PTR(-EIO);
				} else {
#ifdef CONFIG_F2FS_FS_COMPRESSION
					if (f2fs_compressed_file(inode_source) &&
						f2fs_is_compressed_cluster(inode_source,
							fm_ext_node->extent.index + k)) {
						f2fs_warn(sbi, "f2fs_ioc_end_file_merge ino = %u,(%d, %d) is compressed\n",
								cur_merge_file->ino, fm_ext_node->extent.index, k);
						inode_unlock(inode_source);
						iput(inode_source);
						ret = -EKEYEXPIRED;
						goto fail;
					}
#endif
					page = f2fs_get_lock_data_page(inode_source, fm_ext_node->extent.index + k, false);
				}
				if (IS_ERR(page)) {
					f2fs_warn(sbi, "f2fs_find_data_page err (%d,%d) ino:%d",
								fm_ext_node->extent.index, k, cur_merge_file->ino);
					inode_unlock(inode_source);
					iput(inode_source);
					ret = -EIO;
					goto fail;
				}

				addr = kmap(page);
				if (f2fs_file_write(filp, offset, addr, PAGE_SIZE) != PAGE_SIZE) {
					kunmap(page);
					flush_dcache_page(page);
					f2fs_put_page(page, 1);
					inode_unlock(inode_source);
					iput(inode_source);
					ret = -EIO;
					goto fail;
				}
				kunmap(page);
				flush_dcache_page(page);
				offset += PAGE_SIZE;
				merged_blocks++;
				f2fs_put_page(page, 1);
				if (merged_blocks > max_blocks) {
					f2fs_warn(sbi, "f2fs_ioc_end_file_merge excess max_blocks %lu,%u!!!",
							merged_blocks, max_blocks);
					inode_unlock(inode_source);
					iput(inode_source);
					ret = -EFAULT;
					goto fail;
				}

				if (fatal_signal_pending(current)) {
					inode_unlock(inode_source);
					iput(inode_source);
					ret = -EINTR;
					goto fail;
				}

				if (!f2fs_appboost_enable(sbi)) {
					inode_unlock(inode_source);
					iput(inode_source);
					ret = -ENOTTY;
					goto fail;
				}
			}
		}
		/* invalidate clean page */
		invalidate_mapping_pages(inode_source->i_mapping, 0, -1);
		inode_unlock(inode_source);
		iput(inode_source);
	}

	if (unlikely(offset > INT_MAX)) {
		ret = -EKEYEXPIRED;
		goto fail;
	}

	tail = offset;

	// first write summary
	offset = 0;
	summary->version = F2FS_BOOSTFILE_VERSION;
	summary->state = BOOST_FILE_STATE_FINISH;
	summary->tail = tail;
	summary->checksum = 0;
	summary->checksum = f2fs_crc32(sbi, (void *)summary , sizeof(struct merge_summary));

	copy_summary_info_to_disk(summary, summary_dinfo);
	result = f2fs_file_write(filp, offset, (unsigned char *)summary_dinfo,
				 sizeof(struct merge_summary_dinfo));
	if (result != sizeof(struct merge_summary_dinfo)) {
		ret = -EIO;
		goto fail;
	}

	// write the file info
	offset = sizeof(struct merge_summary_dinfo);
	for (i = 0; i < fmm->num; i++) {
		copy_file_merge_info_to_disk(&(merge_files_lists[i]->merge_file), &file_dinfo);
		result = f2fs_file_write(filp, offset, (unsigned char *)&(file_dinfo),
					 sizeof(file_dinfo));
		if (result != sizeof(file_dinfo)) {
			ret = -EIO;
			goto fail;
		}
		offset += sizeof(file_dinfo);

		list_for_each_entry_safe(fm_ext_node, fm_ext_tmp, &merge_files_lists[i]->ext_list, list) {
			copy_extent_info_to_disk(&fm_ext_node->extent, &extent_dinfo);
			result = f2fs_file_write(filp, offset, (unsigned char *)&extent_dinfo, sizeof(extent_dinfo));
			if (result != sizeof(extent_dinfo)) {
				ret = -EIO;
				goto fail;
			}
			offset += sizeof(extent_dinfo);

			if (fatal_signal_pending(current)) {
				ret = -EINTR;
				goto fail;
			}

			if (!f2fs_appboost_enable(sbi)) {
				ret = -ENOTTY;
				goto fail;
			}
		}
	}

	ret = merge_sync_file(sbi, filp);
	if (ret)
		goto fail;

	offset = tail;
	if (time_to_inject(sbi, FAULT_WRITE_TAIL_ERROR)) {
		summary_dinfo->checksum = F2FS_BOOSTFILE_VERSION;
	}
	result = f2fs_file_write(filp, offset, (unsigned char *)summary_dinfo,
				 sizeof(struct merge_summary_dinfo));
	if (result != sizeof(struct merge_summary_dinfo)) {
		ret = -EIO;
		goto fail;
	}

#ifdef CONFIG_F2FS_FS_DEDUP
        if (f2fs_inode_support_dedup(sbi, inode)) {
                set_inode_flag(inode, FI_MERGED_FILE);
                set_inode_flag(inode, FI_DATA_UN_MODIFY);
        }
#endif
	ret = merge_sync_file(sbi, filp);
fail:
	f2fs_boostfile_free(inode);
        inode_unlock(inode);
	if (merge_files_lists)
		kvfree(merge_files_lists);
	if (summary)
		kfree(summary);
	if (summary_dinfo)
		kfree(summary_dinfo);

	return ret;
}

static int f2fs_ioc_end_file_merge(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	int ret;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	if  (!f2fs_appboost_enable(sbi))
		return -ENOTTY;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	ret = end_file_merge(filp, arg);

	mnt_drop_write_file(filp);

	return ret;
}

static inline bool appboost_should_abort(struct inode *inode,
					 unsigned long boost_start, unsigned int interval)
{
	if (time_after(jiffies, boost_start + interval))
		return true;

	if (atomic_read(&(F2FS_I(inode)->appboost_abort))) {
		atomic_set(&(F2FS_I(inode)->appboost_abort), 0);
		return true;
	}

	return false;
}

static int f2fs_ioc_abort_preload_file(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	if (!f2fs_appboost_enable(sbi))
		return -ENOTTY;

	atomic_set(&(F2FS_I(inode)->appboost_abort), 1);

	return 0;
}

static void copy_summary_info_from_disk(struct merge_summary *summary,
					struct merge_summary_dinfo *summary_dinfo)
{
	int i;

	if (!summary || !summary_dinfo)
		return;

	summary->num = le32_to_cpu(summary_dinfo->num);
	summary->version = le32_to_cpu(summary_dinfo->version);
	summary->state = le32_to_cpu(summary_dinfo->state);
	summary->tail = le32_to_cpu(summary_dinfo->tail);
	summary->checksum = le32_to_cpu(summary_dinfo->checksum);

	for (i = 0; i < min(summary->num, BOOST_MAX_FILES); i++)
		summary->fsize[i] = le32_to_cpu(summary_dinfo->fsize[i]);
}

static void copy_extent_info_from_disk(struct merge_extent *extent,
					struct merge_extent_dinfo *d_extent)
{
	if (!extent || !d_extent)
		return;

	extent->index = le32_to_cpu(d_extent->index);
	extent->length = le32_to_cpu(d_extent->length);
	extent->index_in_mfile = le32_to_cpu(d_extent->index_in_mfile);
}

static void copy_file_merge_info_from_disk(struct merge_file *info, struct merge_file_dinfo *dinfo)
{
	if (!info || !dinfo)
		return;

	info->ino = le32_to_cpu(dinfo->ino);
	info->extent_count = le32_to_cpu(dinfo->extent_count);
	info->i_generation = le32_to_cpu(dinfo->i_generation);
	info->mtime = le64_to_cpu(dinfo->mtime);
}

static int f2fs_ioc_preload_file(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct inode *inode_source = NULL;
	struct merge_summary *summary = NULL;
	struct merge_summary_dinfo *summary_dinfo = NULL;
	unsigned char *page_addr = NULL;
	struct page *page = NULL;
	unsigned char *buf = NULL;
	unsigned long boost_start = jiffies;
	/* arg indicate ms to run */
	unsigned long interval, interval_ms;
	loff_t pos = 0;
	loff_t tail = 0;
	long long pos_in = 0;
	unsigned long to_read = 0;
	int ret = 0;
	int i, j, k;
	int checksum = 0;

	if (!f2fs_appboost_enable(sbi))
		return -ENOTTY;

	if (get_user(interval_ms, (unsigned long __user *)arg))
		return -EFAULT;

	if (interval_ms > PRELOAD_MAX_TIME)
		interval = PRELOAD_MAX_TIME * HZ / 1000;
	else
		interval = interval_ms * HZ / 1000;

	if (!inode_trylock(inode))
		return -EAGAIN;

#ifdef CONFIG_F2FS_FS_DEDUP
	if (f2fs_inode_support_dedup(sbi, inode) &&
			(!is_inode_flag_set(inode, FI_MERGED_FILE) ||
			!is_inode_flag_set(inode, FI_DATA_UN_MODIFY))) {
		f2fs_err(sbi, "merged file has something wrong\n");
		ret = -EKEYEXPIRED;
		goto fail;
	}
#endif

	if (atomic_read(&(F2FS_I(inode)->appboost_abort))) {
		atomic_set(&(F2FS_I(inode)->appboost_abort), 0);
		ret = -EAGAIN;
		goto fail;
	}

	//check head summary
	summary = f2fs_kzalloc(sbi, sizeof(struct merge_summary), GFP_KERNEL);
	if (!summary) {
		f2fs_err(sbi, "f2fs_ioc_preload_file: summary is null\n");
		ret = -ENOMEM;
		goto fail;
	}

	summary_dinfo = f2fs_kzalloc(sbi, sizeof(struct merge_summary_dinfo), GFP_KERNEL);
	if (!summary_dinfo) {
		f2fs_err(sbi, "f2fs_ioc_preload_file: summary_dinfo is null\n");
		ret = -ENOMEM;
		goto fail;
	}

	to_read = sizeof(struct merge_summary_dinfo);
	if (f2fs_file_read(filp, 0, (unsigned char*)summary_dinfo, to_read) != to_read) {
		ret = -EIO;
		goto fail;
	}

	copy_summary_info_from_disk(summary, summary_dinfo);

	checksum = summary->checksum;
	summary->checksum = 0;
	if (!f2fs_crc_valid(sbi, checksum, summary, sizeof(struct merge_summary))) {
		ret = -EKEYEXPIRED;
		goto fail;
	}

	if (summary->version != F2FS_BOOSTFILE_VERSION) {
		f2fs_err(sbi, "f2fs_ioc_preload_file boost file version mismatch!\n");
		ret = -EKEYEXPIRED;
		goto fail;
	}

	if (summary->state != BOOST_FILE_STATE_FINISH) {
		f2fs_err(sbi, "f2fs_ioc_preload_file boost file not ready!\n");
		ret = -EKEYEXPIRED;
		goto fail;
	}

	//check tail summary
	tail = summary->tail;
	to_read = sizeof(struct merge_summary);
	if (f2fs_file_read(filp, tail, (unsigned char*)summary_dinfo, to_read) != to_read) {
		ret = -EIO;
		goto fail;
	}

	copy_summary_info_from_disk(summary, summary_dinfo);
	if (checksum != summary->checksum) {
		ret = -EKEYEXPIRED;
		goto fail;
	}

	summary->checksum = 0;
	if (!f2fs_crc_valid(sbi, checksum, summary, sizeof(struct merge_summary))) {
		ret = -EKEYEXPIRED;
		goto fail;
	}

	if (summary->num > BOOST_MAX_FILES) {
		ret = -EKEYEXPIRED;
		goto fail;
	}

	pos += sizeof(struct merge_summary);
	for (i = 0; i < summary->num; i++) {
		pos += summary->fsize[i];
	}

	buf = f2fs_kvmalloc(sbi, pos - sizeof(struct merge_summary), GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto fail;
	}

	to_read = pos - sizeof(struct merge_summary_dinfo);
	if (f2fs_file_read(filp, sizeof(struct merge_summary), buf, to_read) != to_read) {
		f2fs_err(sbi, "f2fs_ioc_preload_file read buf failed!\n");
                ret = -EIO;
		goto fail;
	}

	// align the pos
	pos = (pos + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	for (i = 0; i < summary->num; i++) {
		u64 real_size = 0;
		struct merge_file merge_file;
		struct merge_file_dinfo *merge_file_dinfo = (struct merge_file_dinfo *)(buf + pos_in);
		copy_file_merge_info_from_disk(&merge_file, merge_file_dinfo);

		real_size = sizeof(struct merge_file_dinfo) +
				   merge_file.extent_count * sizeof(struct merge_extent_dinfo);
		if (summary->fsize[i] != real_size) {
			ret = -EKEYEXPIRED;
			goto fail;
		}

		pos_in += summary->fsize[i];
		inode_source = f2fs_iget(sbi->sb, merge_file.ino);
		if (IS_ERR(inode_source)) {
			f2fs_err(sbi, "f2fs_ioc_preload_file no found!\n");
			ret = -EFAULT;
			goto fail;
		}

		if (is_bad_inode(inode_source)) {
			ret = -EKEYEXPIRED;
			goto fail_iput_source;
		}

		if (!S_ISREG(inode_source->i_mode)) {
			ret = -EKEYEXPIRED;
			goto fail_iput_source;
		}

		if (!inode_trylock(inode_source)) {
			ret = -EAGAIN;
			goto fail_iput_source;
		}

		if (merge_file.mtime != timespec64_to_ns(&inode_source->i_mtime)) {
			f2fs_warn(sbi, "f2fs_ioc_preload_file file_merge has been changed! ino = %u, source_time = %llu, merge_time = %llu\n",
								merge_file.ino, timespec64_to_ns(&inode_source->i_mtime), merge_file.mtime);
			ret = -EKEYEXPIRED;
			goto fail_unlock_source;
		}

		if (merge_file.i_generation != inode_source->i_generation) {
                        f2fs_warn(sbi, "f2fs_ioc_preload_file i_generation has been changed!");
			ret = -EKEYEXPIRED;
			goto fail_unlock_source;
		}

#ifdef CONFIG_F2FS_FS_DEDUP
		if (f2fs_inode_support_dedup(sbi, inode_source) &&
				!is_inode_flag_set(inode_source, FI_DATA_UN_MODIFY)) {
			f2fs_err(sbi, "source file has modified\n");
			ret = -EKEYEXPIRED;
			goto fail_unlock_source;
		}
#endif

		ret = fscrypt_require_key(inode_source);
		if (ret) {
			f2fs_warn(sbi, "f2fs_ioc_preload_file get file ino = %lu encrypt info failed\n", inode_source->i_ino);
			goto fail_unlock_source;
		}

		for (j = 0; j < merge_file.extent_count; j++) {
			struct merge_extent extent;
			struct merge_extent extent_next;
			copy_extent_info_from_disk(&extent, &(merge_file_dinfo->extents[j]));
			if (pos >> PAGE_SHIFT != extent.index_in_mfile) {
				f2fs_err(sbi, "f2fs_ioc_preload_file invalid index in merge file\n");
				ret = -EKEYEXPIRED;
				goto fail_unlock_source;
			}

			if (j < merge_file.extent_count - 1) {
				copy_extent_info_from_disk(&extent_next, &(merge_file_dinfo->extents[j + 1]));
				if (extent.index_in_mfile + extent.length != extent_next.index_in_mfile) {
					f2fs_err(sbi, "f2fs_ioc_preload_file invalid extent len in merge file\n");
					ret = -EKEYEXPIRED;
					goto fail_unlock_source;
				}
			}

			for (k = 0; k < extent.length; k++, pos += PAGE_SIZE) {
				if (time_to_inject(sbi, FAULT_PAGE_ERROR)) {
					page = NULL;
				} else {
					page = f2fs_pagecache_get_page(inode_source->i_mapping, extent.index + k,
								FGP_LOCK | FGP_CREAT, GFP_NOFS);
				}
				if (!page) {
					f2fs_warn(sbi, "f2fs_ioc_preload_file can not get page cache!\n");
					ret = -ENOMEM;
					goto fail_unlock_source;
				}

				if (PageUptodate(page)) {
					f2fs_put_page(page, 1);
					continue;
				}
				page_addr = kmap(page);
				if (f2fs_file_read(filp, pos, page_addr, PAGE_SIZE) != PAGE_SIZE) {
					kunmap(page);
					flush_dcache_page(page);
					f2fs_put_page(page, 1);
					ret = -EIO;
					goto fail_unlock_source;
				}
				kunmap(page);
				flush_dcache_page(page);
				SetPageUptodate(page);
				f2fs_put_page(page, 1);

				if (appboost_should_abort(inode, boost_start, interval)) {
					ret = -EINTR;
					f2fs_err(sbi, "f2fs_ioc_preload_file failed timeout!\n");
					goto fail_unlock_source;
				}

				if (fatal_signal_pending(current)) {
					ret = -EINTR;
					goto fail_unlock_source;
				}

				if (!f2fs_appboost_enable(sbi)) {
					ret = -ENOTTY;
					goto fail_unlock_source;
				}
			}
		}
		inode_unlock(inode_source);
		iput(inode_source);
	}
fail_unlock_source:
	if (ret)
		inode_unlock(inode_source);
fail_iput_source:
	if (ret)
		iput(inode_source);
fail:
	atomic_set(&(F2FS_I(inode)->appboost_abort), 0);
	/* invalidate boostfile clean page */
	invalidate_mapping_pages(inode->i_mapping, 0, -1);
	inode_unlock(inode);
	if (buf)
		kvfree(buf);
	if (summary)
		kfree(summary);
	if (summary_dinfo)
		kfree(summary_dinfo);
	return ret;
}
#endif

static bool extra_attr_fits_in_inode(struct inode *inode, int field)
{
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct f2fs_inode *ri;

	switch (field) {
	case F2FS_EXTRA_ATTR_TOTAL_SIZE:
	case F2FS_EXTRA_ATTR_ISIZE:
	case F2FS_EXTRA_ATTR_INLINE_XATTR_SIZE:
		return true;
	case F2FS_EXTRA_ATTR_PROJID:
		if (!F2FS_FITS_IN_INODE(ri, fi->i_extra_isize, i_projid))
			return false;
		return true;
	case F2FS_EXTRA_ATTR_INODE_CHKSUM:
		if (!F2FS_FITS_IN_INODE(ri, fi->i_extra_isize, i_inode_checksum))
			return false;
		return true;
	case F2FS_EXTRA_ATTR_CRTIME:
		if (!F2FS_FITS_IN_INODE(ri, fi->i_extra_isize, i_crtime))
			return false;
		return true;
	case F2FS_EXTRA_ATTR_COMPR_BLOCKS:
	case F2FS_EXTRA_ATTR_COMPR_OPTION:
		if (!F2FS_FITS_IN_INODE(ri, fi->i_extra_isize, i_compr_blocks))
			return false;
		return true;
	default:
		f2fs_bug_on(F2FS_I_SB(inode), 1);
		return false;
	}
}

static int f2fs_ioc_get_extra_attr(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_extra_attr attr;
	u32 chksum;
	int ret = 0;

	if (!f2fs_has_extra_attr(inode))
		return -EOPNOTSUPP;

	if (copy_from_user(&attr, (void __user *)arg, sizeof(attr)))
		return -EFAULT;

	if (attr.field >= F2FS_EXTRA_ATTR_MAX)
		return -EINVAL;

	if (!extra_attr_fits_in_inode(inode, attr.field))
		return -EOPNOTSUPP;

	switch (attr.field) {
	case F2FS_EXTRA_ATTR_TOTAL_SIZE:
		attr.attr = F2FS_TOTAL_EXTRA_ATTR_SIZE;
		break;
	case F2FS_EXTRA_ATTR_ISIZE:
		attr.attr = fi->i_extra_isize;
		break;
	case F2FS_EXTRA_ATTR_INLINE_XATTR_SIZE:
		if (!f2fs_has_inline_xattr(inode))
			return -EOPNOTSUPP;
		attr.attr = get_inline_xattr_addrs(inode);
		break;
	case F2FS_EXTRA_ATTR_PROJID:
		if (!f2fs_sb_has_project_quota(F2FS_I_SB(inode)))
			return -EOPNOTSUPP;
		attr.attr = from_kprojid(&init_user_ns, fi->i_projid);
		break;
	case F2FS_EXTRA_ATTR_INODE_CHKSUM:
		ret = f2fs_inode_chksum_get(sbi, inode, &chksum);
		if (ret)
			return ret;
		attr.attr = chksum;
		break;
	case F2FS_EXTRA_ATTR_CRTIME:
		if (!f2fs_sb_has_inode_crtime(sbi))
			return -EOPNOTSUPP;
		if (attr.attr_size == sizeof(struct timespec64)) {
			if (put_timespec64(&fi->i_crtime,
					(void __user *)(uintptr_t)attr.attr))
				return -EFAULT;
		} else if (attr.attr_size == sizeof(struct old_timespec32)) {
			if (put_old_timespec32(&fi->i_crtime,
					(void __user *)(uintptr_t)attr.attr))
				return -EFAULT;
		} else {
			return -EINVAL;
		}
		break;
	case F2FS_EXTRA_ATTR_COMPR_BLOCKS:
		if (attr.attr_size != sizeof(__u64))
			return -EINVAL;
		ret = f2fs_get_compress_blocks(inode, &attr.attr);
		break;
	case F2FS_EXTRA_ATTR_COMPR_OPTION:
		/* fix coverity error: Untrusted value as argument attr.attr_size*/
		if (attr.attr_size > sizeof(struct f2fs_comp_option_v2))
			return -EINVAL;
		ret = f2fs_get_compress_option_v2(filp, attr.attr,
						  &attr.attr_size);
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	if (copy_to_user((void __user *)arg, &attr, sizeof(attr)))
		return -EFAULT;

	return 0;
}

static int f2fs_ioc_set_extra_attr(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_extra_attr attr;
	struct page *ipage;
	void *inline_addr;
	int ret;

	if (!f2fs_has_extra_attr(inode))
		return -EOPNOTSUPP;

	if (copy_from_user(&attr, (void __user *)arg, sizeof(attr)))
		return -EFAULT;

	if (attr.field >= F2FS_EXTRA_ATTR_MAX)
		return -EINVAL;

	if (!extra_attr_fits_in_inode(inode, attr.field))
		return -EOPNOTSUPP;

	switch (attr.field) {
	case F2FS_EXTRA_ATTR_TOTAL_SIZE:
	case F2FS_EXTRA_ATTR_ISIZE:
	case F2FS_EXTRA_ATTR_PROJID:
	case F2FS_EXTRA_ATTR_INODE_CHKSUM:
	case F2FS_EXTRA_ATTR_CRTIME:
	case F2FS_EXTRA_ATTR_COMPR_BLOCKS:
		/* read only attribtues */
		return -EOPNOTSUPP;
	case F2FS_EXTRA_ATTR_INLINE_XATTR_SIZE:
		if (!f2fs_sb_has_flexible_inline_xattr(sbi) ||
		    !f2fs_has_inline_xattr(inode))
			return -EOPNOTSUPP;
		if (attr.attr < MIN_INLINE_XATTR_SIZE ||
		    attr.attr > MAX_INLINE_XATTR_SIZE)
			return -EINVAL;
		inode_lock(inode);
		f2fs_lock_op(sbi);
		f2fs_down_write(&F2FS_I(inode)->i_xattr_sem);
		if (i_size_read(inode) || F2FS_I(inode)->i_xattr_nid) {
			/*
			 * it is not allowed to set this field if the inode
			 * has data or xattr node
			 */
			ret = -EFBIG;
			goto xattr_out_unlock;
		}
		ipage = f2fs_get_node_page(sbi, inode->i_ino);
		if (IS_ERR(ipage)) {
			ret = PTR_ERR(ipage);
			goto xattr_out_unlock;
		}
		inline_addr = inline_xattr_addr(inode, ipage);
		if (!IS_XATTR_LAST_ENTRY(XATTR_FIRST_ENTRY(inline_addr))) {
			ret = -EFBIG;
		} else {
			struct f2fs_xattr_header *hdr;
			struct f2fs_xattr_entry *ent;

			F2FS_I(inode)->i_inline_xattr_size = (int)attr.attr;
			inline_addr = inline_xattr_addr(inode, ipage);
			hdr = XATTR_HDR(inline_addr);
			ent = XATTR_FIRST_ENTRY(inline_addr);
			hdr->h_magic = cpu_to_le32(F2FS_XATTR_MAGIC);
			hdr->h_refcount = cpu_to_le32(1);
			memset(ent, 0, attr.attr - sizeof(*hdr));
			set_page_dirty(ipage);
			ret = 0;
		}
		f2fs_put_page(ipage, 1);
xattr_out_unlock:
		f2fs_up_write(&F2FS_I(inode)->i_xattr_sem);
		f2fs_unlock_op(sbi);
		inode_unlock(inode);
		if (!ret)
			f2fs_balance_fs(sbi, true);
		break;
	case F2FS_EXTRA_ATTR_COMPR_OPTION:
		/* fix coverity error: Untrusted value as argument attr.attr_size*/
		if (attr.attr_size > sizeof(struct f2fs_comp_option_v2))
			return -EINVAL;
		ret = f2fs_set_compress_option_v2(filp, attr.attr,
						  &attr.attr_size);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

#ifdef CONFIG_F2FS_SEQZONE
static long f2fs_ioc_set_seqzone_file(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	int ret = 0;

	if (!f2fs_sb_has_seqzone(sbi)) {
		return -ENOTSUPP;
	}

	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	inode_lock(inode);
	if (!IS_ENCRYPTED(inode)) {
		ret = -EINVAL;
		goto out;
	}

	if (F2FS_HAS_BLOCKS(inode)) {
		ret = -EINVAL;
		goto out;
	}

	if (is_inode_flag_set(inode, FI_COMPRESSED_FILE)) {
		ret = -ENOTSUPP;
		goto out;
	}

	if (f2fs_inode_support_dedup(sbi, inode)) {
		set_inode_flag(inode, FI_SEQZONE);
		f2fs_mark_inode_dirty_sync(inode, true);
	}

out:
	inode_unlock(inode);
	mnt_drop_write_file(filp);
	return ret;
}
static long f2fs_ioc_get_seqzone_file(struct file *filp, unsigned long arg)
{
	struct inode *inode = file_inode(filp);

	int seqzone = f2fs_seqzone_file(inode) ? 1 : 0;
	if (copy_to_user((int __user*)arg, &seqzone, sizeof(seqzone))) {
		return -1;
	}
	return 0;
}
#endif

static long __f2fs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	switch (cmd) {
	case FS_IOC_GETVERSION:
		return f2fs_ioc_getversion(filp, arg);
	case F2FS_IOC_START_ATOMIC_WRITE:
		return f2fs_ioc_start_atomic_write(filp, false);
	case F2FS_IOC_START_ATOMIC_REPLACE:
		return f2fs_ioc_start_atomic_write(filp, true);
	case F2FS_IOC_COMMIT_ATOMIC_WRITE:
		return f2fs_ioc_commit_atomic_write(filp);
	case F2FS_IOC_ABORT_ATOMIC_WRITE:
		return f2fs_ioc_abort_atomic_write(filp);
	case F2FS_IOC_START_VOLATILE_WRITE:
	case F2FS_IOC_RELEASE_VOLATILE_WRITE:
		return -EOPNOTSUPP;
	case F2FS_IOC_SHUTDOWN:
		return f2fs_ioc_shutdown(filp, arg);
	case FITRIM:
		return f2fs_ioc_fitrim(filp, arg);
	case FS_IOC_SET_ENCRYPTION_POLICY:
		return f2fs_ioc_set_encryption_policy(filp, arg);
	case FS_IOC_GET_ENCRYPTION_POLICY:
		return f2fs_ioc_get_encryption_policy(filp, arg);
	case FS_IOC_GET_ENCRYPTION_PWSALT:
		return f2fs_ioc_get_encryption_pwsalt(filp, arg);
	case FS_IOC_GET_ENCRYPTION_POLICY_EX:
		return f2fs_ioc_get_encryption_policy_ex(filp, arg);
	case FS_IOC_ADD_ENCRYPTION_KEY:
		return f2fs_ioc_add_encryption_key(filp, arg);
	case FS_IOC_REMOVE_ENCRYPTION_KEY:
		return f2fs_ioc_remove_encryption_key(filp, arg);
	case FS_IOC_REMOVE_ENCRYPTION_KEY_ALL_USERS:
		return f2fs_ioc_remove_encryption_key_all_users(filp, arg);
	case FS_IOC_GET_ENCRYPTION_KEY_STATUS:
		return f2fs_ioc_get_encryption_key_status(filp, arg);
	case FS_IOC_GET_ENCRYPTION_NONCE:
		return f2fs_ioc_get_encryption_nonce(filp, arg);
	case F2FS_IOC_GARBAGE_COLLECT:
		return f2fs_ioc_gc(filp, arg);
	case F2FS_IOC_GARBAGE_COLLECT_RANGE:
		return f2fs_ioc_gc_range(filp, arg);
	case F2FS_IOC_WRITE_CHECKPOINT:
		return f2fs_ioc_write_checkpoint(filp);
	case F2FS_IOC_DEFRAGMENT:
		return f2fs_ioc_defragment(filp, arg);
	case F2FS_IOC_MOVE_RANGE:
		return f2fs_ioc_move_range(filp, arg);
	case F2FS_IOC_FLUSH_DEVICE:
		return f2fs_ioc_flush_device(filp, arg);
	case F2FS_IOC_GET_FEATURES:
		return f2fs_ioc_get_features(filp, arg);
	case F2FS_IOC_GET_PIN_FILE:
		return f2fs_ioc_get_pin_file(filp, arg);
	case F2FS_IOC_SET_PIN_FILE:
		return f2fs_ioc_set_pin_file(filp, arg);
	case F2FS_IOC_PRECACHE_EXTENTS:
		return f2fs_ioc_precache_extents(filp);
	case F2FS_IOC_RESIZE_FS:
		return f2fs_ioc_resize_fs(filp, arg);
	case FS_IOC_ENABLE_VERITY:
		return f2fs_ioc_enable_verity(filp, arg);
	case FS_IOC_MEASURE_VERITY:
		return f2fs_ioc_measure_verity(filp, arg);
	case FS_IOC_READ_VERITY_METADATA:
		return f2fs_ioc_read_verity_metadata(filp, arg);
	case FS_IOC_GETFSLABEL:
		return f2fs_ioc_getfslabel(filp, arg);
	case FS_IOC_SETFSLABEL:
		return f2fs_ioc_setfslabel(filp, arg);
	case F2FS_IOC_GET_COMPRESS_BLOCKS:
		return f2fs_ioc_get_compress_blocks(filp, arg);
	case F2FS_IOC_RELEASE_COMPRESS_BLOCKS:
		return f2fs_release_compress_blocks(filp, arg);
	case F2FS_IOC_RESERVE_COMPRESS_BLOCKS:
		return f2fs_ioc_reserve_compress_blocks(filp, arg);
	case F2FS_IOC_SEC_TRIM_FILE:
		return f2fs_sec_trim_file(filp, arg);
	case F2FS_IOC_GET_COMPRESS_OPTION:
		return f2fs_ioc_get_compress_option(filp, arg);
	case F2FS_IOC_SET_COMPRESS_OPTION:
		return f2fs_ioc_set_compress_option(filp, arg);
	case F2FS_IOC_DECOMPRESS_FILE:
		return f2fs_ioc_decompress_file(filp);
	case F2FS_IOC_COMPRESS_FILE:
		return f2fs_ioc_compress_file(filp);
#ifdef CONFIG_F2FS_APPBOOST
	case F2FS_IOC_START_MERGE_FILE:
		return f2fs_ioc_start_file_merge(filp, arg);
	case F2FS_IOC_END_MERGE_FILE:
		return f2fs_ioc_end_file_merge(filp, arg);
	case F2FS_IOC_PRELOAD_FILE:
		return f2fs_ioc_preload_file(filp, arg);
	case F2FS_IOC_ABORT_PRELOAD_FILE:
		return f2fs_ioc_abort_preload_file(filp, arg);
#endif
	case F2FS_IOC_GET_EXTRA_ATTR:
		return f2fs_ioc_get_extra_attr(filp, arg);
	case F2FS_IOC_SET_EXTRA_ATTR:
		return f2fs_ioc_set_extra_attr(filp, arg);
#ifdef CONFIG_F2FS_FS_DEDUP
	case F2FS_IOC_DEDUP_CREATE:
		ret = f2fs_ioc_create_layered_inode(filp, arg);
		break;
	case F2FS_IOC_DEDUP_FILE:
		ret = f2fs_ioc_dedup_file(filp, arg);
		break;
	case F2FS_IOC_DEDUP_REVOKE:
		ret = f2fs_ioc_dedup_revoke(filp, arg);
		break;
	case F2FS_IOC_CLONE_FILE:
		ret = f2fs_ioc_clone_file(filp, arg);
		break;
	case F2FS_IOC_MODIFY_CHECK:
		ret = f2fs_ioc_modify_check(filp, arg);
		break;
	case F2FS_IOC_DEDUP_PERM_CHECK:
		ret = f2fs_ioc_dedup_permission_check(filp, arg);
		break;
	case F2FS_IOC_DEDUP_GET_FILE_INFO:
		ret = f2fs_ioc_get_dedupd_file_info(filp, arg);
		break;
	case F2FS_IOC_DEDUP_GET_SYS_INFO:
		ret = f2fs_ioc_get_dedup_sysinfo(filp, arg);
		break;
	case F2FS_IOC_SNAPSHOT_CREATE:
		ret = f2fs_ioc_create_snapshot(filp, arg);
		break;
	case F2FS_IOC_SNAPSHOT_PREPARE:
		ret = f2fs_ioc_prepare_snapshot(filp, arg);
		break;
#endif
#ifdef CONFIG_F2FS_SEQZONE
	case F2FS_IOC_SET_SEQZONE_FILE:
		return f2fs_ioc_set_seqzone_file(filp, arg);
	case F2FS_IOC_GET_SEQZONE_FILE:
		return f2fs_ioc_get_seqzone_file(filp, arg);
#endif
	default:
		return -ENOTTY;
	}
	return ret;
}

long f2fs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	if (unlikely(f2fs_cp_error(F2FS_I_SB(file_inode(filp)))))
		return -EIO;
	if (!f2fs_is_checkpoint_ready(F2FS_I_SB(file_inode(filp))))
		return -ENOSPC;

	return __f2fs_ioctl(filp, cmd, arg);
}

/*
 * Return %true if the given read or write request should use direct I/O, or
 * %false if it should use buffered I/O.
 */
static bool f2fs_should_use_dio(struct inode *inode, struct kiocb *iocb,
				struct iov_iter *iter)
{
	unsigned int align;

	if (!(iocb->ki_flags & IOCB_DIRECT))
		return false;

	if (f2fs_force_buffered_io(inode, iov_iter_rw(iter)))
		return false;

#ifdef CONFIG_F2FS_FS_DEDUP
	if(is_inode_flag_set(inode, FI_SNAPSHOT_PREPARED))
		return false;
#endif

	/*
	 * Direct I/O not aligned to the disk's logical_block_size will be
	 * attempted, but will fail with -EINVAL.
	 *
	 * f2fs additionally requires that direct I/O be aligned to the
	 * filesystem block size, which is often a stricter requirement.
	 * However, f2fs traditionally falls back to buffered I/O on requests
	 * that are logical_block_size-aligned but not fs-block aligned.
	 *
	 * The below logic implements this behavior.
	 */
	align = iocb->ki_pos | iov_iter_alignment(iter);
	if (!IS_ALIGNED(align, i_blocksize(inode)) &&
	    IS_ALIGNED(align, bdev_logical_block_size(inode->i_sb->s_bdev)))
		return false;

	return true;
}

static int f2fs_dio_read_end_io(struct kiocb *iocb, ssize_t size, int error,
				unsigned int flags)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(file_inode(iocb->ki_filp));

	dec_page_count(sbi, F2FS_DIO_READ);
	if (error)
		return error;
	f2fs_update_iostat(sbi, NULL, APP_DIRECT_READ_IO, size);
	return 0;
}

static const struct iomap_dio_ops f2fs_iomap_dio_read_ops = {
	.end_io = f2fs_dio_read_end_io,
};

static ssize_t f2fs_dio_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	const loff_t pos = iocb->ki_pos;
	const size_t count = iov_iter_count(to);
	struct iomap_dio *dio;
	ssize_t ret;

	if (count == 0)
		return 0; /* skip atime update */

	trace_f2fs_direct_IO_enter(inode, iocb, count, READ);

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (!f2fs_down_read_trylock(&fi->i_gc_rwsem[READ])) {
			ret = -EAGAIN;
			goto out;
		}
	} else {
		f2fs_down_read(&fi->i_gc_rwsem[READ]);
	}

	/*
	 * We have to use __iomap_dio_rw() and iomap_dio_complete() instead of
	 * the higher-level function iomap_dio_rw() in order to ensure that the
	 * F2FS_DIO_READ counter will be decremented correctly in all cases.
	 */
	inc_page_count(sbi, F2FS_DIO_READ);
	dio = __iomap_dio_rw(iocb, to, &f2fs_iomap_ops,
			     &f2fs_iomap_dio_read_ops, 0, NULL, 0);
	if (IS_ERR_OR_NULL(dio)) {
		ret = PTR_ERR_OR_ZERO(dio);
		if (ret != -EIOCBQUEUED)
			dec_page_count(sbi, F2FS_DIO_READ);
	} else {
		ret = iomap_dio_complete(dio);
	}

	f2fs_up_read(&fi->i_gc_rwsem[READ]);

	file_accessed(file);
out:
	trace_f2fs_direct_IO_exit(inode, pos, count, READ, ret);
	return ret;
}

static void f2fs_trace_rw_file_path(struct kiocb *iocb, size_t count, int rw)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	char *buf, *path;

	buf = f2fs_kmalloc(F2FS_I_SB(inode), PATH_MAX, GFP_KERNEL);
	if (!buf)
		return;
#ifdef CONFIG_F2FS_APPBOOST
	buf = strcpy(buf, "/data");
	path = dentry_path_raw(file_dentry(iocb->ki_filp), buf + 5, PATH_MAX - 5);
#else
	path = dentry_path_raw(file_dentry(iocb->ki_filp), buf, PATH_MAX);
#endif
	if (IS_ERR(path))
		goto free_buf;
	if (rw == WRITE)
		trace_f2fs_datawrite_start(inode, iocb->ki_pos, count,
				current->pid, path, current->comm);
	else
		trace_f2fs_dataread_start(inode, iocb->ki_pos, count,
				current->pid, path, current->comm);
free_buf:
	f2fs_putname(buf);
}

static ssize_t f2fs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	const loff_t pos = iocb->ki_pos;
	ssize_t ret;

	if (!f2fs_is_compress_backend_ready(inode))
		return -EOPNOTSUPP;

	if (trace_f2fs_dataread_start_enabled())
		f2fs_trace_rw_file_path(iocb, iov_iter_count(to), READ);

	/* In LFS mode, if there is inflight dio, wait for its completion */
	if (f2fs_lfs_mode(F2FS_I_SB(inode)))
		inode_dio_wait(inode);

	if (f2fs_should_use_dio(inode, iocb, to)) {
		ret = f2fs_dio_read_iter(iocb, to);
	} else {
		struct backing_dev_info *bdi = inode_to_bdi(inode);
		struct file *file = iocb->ki_filp;

		if (!(file->f_mode & FMODE_RANDOM) &&
		    file->f_ra.ra_pages == bdi->ra_pages &&
		    f2fs_compressed_file(inode)) {
			file->f_ra.ra_pages = bdi->ra_pages * MIN_RA_MUL;
		}

		ret = filemap_read(iocb, to, 0);
		if (ret > 0)
			f2fs_update_iostat(F2FS_I_SB(inode), inode,
						APP_BUFFERED_READ_IO, ret);
	}
	if (trace_f2fs_dataread_end_enabled())
		trace_f2fs_dataread_end(inode, pos, ret);
	return ret;
}

static ssize_t f2fs_write_checks(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	ssize_t count;
	int err;

	if (IS_IMMUTABLE(inode))
		return -EPERM;

	if (is_inode_flag_set(inode, FI_COMPRESS_RELEASED)) {
#ifdef CONFIG_F2FS_FS_COMPRESSION
		err = f2fs_reserve_compress_blocks(inode, NULL);
		if (err < 0)
			return err;
#else
		return -EPERM;
#endif
	}

	count = generic_write_checks(iocb, from);
	if (count <= 0)
		return count;

	err = file_modified(file);
	if (err)
		return err;
	return count;
}

/*
 * Preallocate blocks for a write request, if it is possible and helpful to do
 * so.  Returns a positive number if blocks may have been preallocated, 0 if no
 * blocks were preallocated, or a negative errno value if something went
 * seriously wrong.  Also sets FI_PREALLOCATED_ALL on the inode if *all* the
 * requested blocks (not just some of them) have been allocated.
 */
static int f2fs_preallocate_blocks(struct kiocb *iocb, struct iov_iter *iter,
				   bool dio)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	const loff_t pos = iocb->ki_pos;
	const size_t count = iov_iter_count(iter);
	struct f2fs_map_blocks map = {};
	int flag;
	int ret;

	/* If it will be an out-of-place direct write, don't bother. */
#ifdef CONFIG_F2FS_SEQZONE
	if (dio && (f2fs_lfs_mode(sbi) || f2fs_seqzone_file(inode)))
#else
	if (dio && f2fs_lfs_mode(sbi))
#endif
		return 0;
	/*
	 * Don't preallocate holes aligned to DIO_SKIP_HOLES which turns into
	 * buffered IO, if DIO meets any holes.
	 */
	if (dio && i_size_read(inode) &&
		(F2FS_BYTES_TO_BLK(pos) < F2FS_BLK_ALIGN(i_size_read(inode))))
		return 0;

	/* No-wait I/O can't allocate blocks. */
	if (iocb->ki_flags & IOCB_NOWAIT)
		return 0;

	/* If it will be a short write, don't bother. */
	if (fault_in_iov_iter_readable(iter, count))
		return 0;

	if (f2fs_has_inline_data(inode)) {
		/* If the data will fit inline, don't bother. */
		if (pos + count <= MAX_INLINE_DATA(inode))
			return 0;
		ret = f2fs_convert_inline_inode(inode);
		if (ret)
			return ret;
	}

	/* Do not preallocate blocks that will be written partially in 4KB. */
	map.m_lblk = F2FS_BLK_ALIGN(pos);
	map.m_len = F2FS_BYTES_TO_BLK(pos + count);
	if (map.m_len > map.m_lblk)
		map.m_len -= map.m_lblk;
	else
		map.m_len = 0;
	map.m_may_create = true;
	if (dio) {
		map.m_seg_type = f2fs_rw_hint_to_seg_type(inode->i_write_hint);
		flag = F2FS_GET_BLOCK_PRE_DIO;
	} else {
		map.m_seg_type = NO_CHECK_TYPE;
		flag = F2FS_GET_BLOCK_PRE_AIO;
	}

	ret = f2fs_map_blocks(inode, &map, flag);
	/* -ENOSPC|-EDQUOT are fine to report the number of allocated blocks. */
	if (ret < 0 && !((ret == -ENOSPC || ret == -EDQUOT) && map.m_len > 0))
		return ret;
	if (ret == 0)
		set_inode_flag(inode, FI_PREALLOCATED_ALL);
	return map.m_len;
}

static ssize_t f2fs_buffered_write_iter(struct kiocb *iocb,
					struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	ssize_t ret;

	if (iocb->ki_flags & IOCB_NOWAIT)
		return -EOPNOTSUPP;

	current->backing_dev_info = inode_to_bdi(inode);
	ret = generic_perform_write(iocb, from);
	current->backing_dev_info = NULL;

	if (ret > 0) {
		iocb->ki_pos += ret;
		f2fs_update_iostat(F2FS_I_SB(inode), inode,
						APP_BUFFERED_IO, ret);
	}
	return ret;
}

static int f2fs_dio_write_end_io(struct kiocb *iocb, ssize_t size, int error,
				 unsigned int flags)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(file_inode(iocb->ki_filp));

	dec_page_count(sbi, F2FS_DIO_WRITE);
	if (error)
		return error;
	f2fs_update_iostat(sbi, NULL, APP_DIRECT_IO, size);
	return 0;
}

static const struct iomap_dio_ops f2fs_iomap_dio_write_ops = {
	.end_io = f2fs_dio_write_end_io,
};

static void f2fs_flush_buffered_write(struct address_space *mapping,
				      loff_t start_pos, loff_t end_pos)
{
	int ret;

	ret = filemap_write_and_wait_range(mapping, start_pos, end_pos);
	if (ret < 0)
		return;
	invalidate_mapping_pages(mapping,
				 start_pos >> PAGE_SHIFT,
				 end_pos >> PAGE_SHIFT);
}

static ssize_t f2fs_dio_write_iter(struct kiocb *iocb, struct iov_iter *from,
				   bool *may_need_sync)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	const bool do_opu = f2fs_lfs_mode(sbi);
	const loff_t pos = iocb->ki_pos;
	const ssize_t count = iov_iter_count(from);
	unsigned int dio_flags;
	struct iomap_dio *dio;
	ssize_t ret;

	trace_f2fs_direct_IO_enter(inode, iocb, count, WRITE);

	if (iocb->ki_flags & IOCB_NOWAIT) {
		/* f2fs_convert_inline_inode() and block allocation can block */
		if (f2fs_has_inline_data(inode) ||
		    !f2fs_overwrite_io(inode, pos, count)) {
			ret = -EAGAIN;
			goto out;
		}

		if (!f2fs_down_read_trylock(&fi->i_gc_rwsem[WRITE])) {
			ret = -EAGAIN;
			goto out;
		}
		if (do_opu && !f2fs_down_read_trylock(&fi->i_gc_rwsem[READ])) {
			f2fs_up_read(&fi->i_gc_rwsem[WRITE]);
			ret = -EAGAIN;
			goto out;
		}
	} else {
		ret = f2fs_convert_inline_inode(inode);
		if (ret)
			goto out;

		f2fs_down_read(&fi->i_gc_rwsem[WRITE]);
		if (do_opu)
			f2fs_down_read(&fi->i_gc_rwsem[READ]);
	}

	/*
	 * We have to use __iomap_dio_rw() and iomap_dio_complete() instead of
	 * the higher-level function iomap_dio_rw() in order to ensure that the
	 * F2FS_DIO_WRITE counter will be decremented correctly in all cases.
	 */
	inc_page_count(sbi, F2FS_DIO_WRITE);
	dio_flags = 0;
	if (pos + count > inode->i_size)
		dio_flags |= IOMAP_DIO_FORCE_WAIT;
	dio = __iomap_dio_rw(iocb, from, &f2fs_iomap_ops,
			     &f2fs_iomap_dio_write_ops, dio_flags, NULL, 0);
	if (IS_ERR_OR_NULL(dio)) {
		ret = PTR_ERR_OR_ZERO(dio);
		if (ret == -ENOTBLK)
			ret = 0;
		if (ret != -EIOCBQUEUED)
			dec_page_count(sbi, F2FS_DIO_WRITE);
	} else {
		ret = iomap_dio_complete(dio);
	}

	if (do_opu)
		f2fs_up_read(&fi->i_gc_rwsem[READ]);
	f2fs_up_read(&fi->i_gc_rwsem[WRITE]);

	if (ret < 0)
		goto out;
	if (pos + ret > inode->i_size)
		f2fs_i_size_write(inode, pos + ret);
	if (!do_opu)
		set_inode_flag(inode, FI_UPDATE_WRITE);

	if (iov_iter_count(from)) {
		ssize_t ret2;
		loff_t bufio_start_pos = iocb->ki_pos;

		/*
		 * The direct write was partial, so we need to fall back to a
		 * buffered write for the remainder.
		 */

		ret2 = f2fs_buffered_write_iter(iocb, from);
		if (iov_iter_count(from))
			f2fs_write_failed(inode, iocb->ki_pos);
		if (ret2 < 0)
			goto out;

		/*
		 * Ensure that the pagecache pages are written to disk and
		 * invalidated to preserve the expected O_DIRECT semantics.
		 */
		if (ret2 > 0) {
			loff_t bufio_end_pos = bufio_start_pos + ret2 - 1;

			ret += ret2;

			f2fs_flush_buffered_write(file->f_mapping,
						  bufio_start_pos,
						  bufio_end_pos);
		}
	} else {
		/* iomap_dio_rw() already handled the generic_write_sync(). */
		*may_need_sync = false;
	}
out:
	trace_f2fs_direct_IO_exit(inode, pos, count, WRITE, ret);
	return ret;
}

static ssize_t f2fs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	const loff_t orig_pos = iocb->ki_pos;
	const size_t orig_count = iov_iter_count(from);
	loff_t target_size;
	bool dio;
	bool may_need_sync = true;
	int preallocated;
	ssize_t ret;

	if (unlikely(f2fs_cp_error(F2FS_I_SB(inode)))) {
		ret = -EIO;
		goto out;
	}

	if (!f2fs_is_compress_backend_ready(inode)) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (!inode_trylock(inode)) {
			ret = -EAGAIN;
			goto out;
		}
	} else {
		inode_lock(inode);
	}

#ifdef CONFIG_F2FS_FS_COMPRESSION_FIXED_OUTPUT
	CLEAR_IFLAG_IF_SET(inode, F2FS_NOCOMP_FL);
#endif

#ifdef CONFIG_F2FS_FS_DEDUP
	if (!is_inode_flag_set(inode, FI_SNAPSHOT_PREPARED))
		mark_file_modified(inode);
	if (f2fs_is_outer_inode(inode)) {
		ret = f2fs_revoke_deduped_inode(inode, __func__);
		if (ret) {
			inode_unlock(inode);
			goto out;
		}
	}
#endif

	ret = f2fs_write_checks(iocb, from);
	if (ret <= 0)
		goto out_unlock;

	/* Determine whether we will do a direct write or a buffered write. */
	dio = f2fs_should_use_dio(inode, iocb, from);

	/* Possibly preallocate the blocks for the write. */
	target_size = iocb->ki_pos + iov_iter_count(from);
	preallocated = f2fs_preallocate_blocks(iocb, from, dio);
	if (preallocated < 0) {
		ret = preallocated;
	} else {
		if (trace_f2fs_datawrite_start_enabled())
			f2fs_trace_rw_file_path(iocb, orig_count, WRITE);

		/* Do the actual write. */
		ret = dio ?
			f2fs_dio_write_iter(iocb, from, &may_need_sync) :
			f2fs_buffered_write_iter(iocb, from);

		if (trace_f2fs_datawrite_end_enabled())
			trace_f2fs_datawrite_end(inode, orig_pos, ret);
	}

	/* Don't leave any preallocated blocks around past i_size. */
	if (preallocated && i_size_read(inode) < target_size) {
		f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
		filemap_invalidate_lock(inode->i_mapping);
		if (!f2fs_truncate(inode))
			file_dont_truncate(inode);
		filemap_invalidate_unlock(inode->i_mapping);
		f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
	} else {
		file_dont_truncate(inode);
	}

	clear_inode_flag(inode, FI_PREALLOCATED_ALL);
out_unlock:
	inode_unlock(inode);
out:
	trace_f2fs_file_write_iter(inode, orig_pos, orig_count, ret);

	if (ret > 0 && may_need_sync)
		ret = generic_write_sync(iocb, ret);

	/* If buffered IO was forced, flush and drop the data from
	 * the page cache to preserve O_DIRECT semantics
	 */
	if (ret > 0 && !dio && (iocb->ki_flags & IOCB_DIRECT))
		f2fs_flush_buffered_write(iocb->ki_filp->f_mapping,
					  orig_pos,
					  orig_pos + ret - 1);

	return ret;
}

static int f2fs_file_fadvise(struct file *filp, loff_t offset, loff_t len,
		int advice)
{
	struct address_space *mapping;
	struct backing_dev_info *bdi;
	struct inode *inode = file_inode(filp);
	int err;

	if (advice == POSIX_FADV_SEQUENTIAL) {
		if (S_ISFIFO(inode->i_mode))
			return -ESPIPE;

		mapping = filp->f_mapping;
		if (!mapping || len < 0)
			return -EINVAL;

		bdi = inode_to_bdi(mapping->host);
		filp->f_ra.ra_pages = bdi->ra_pages *
			F2FS_I_SB(inode)->seq_file_ra_mul;
		if (f2fs_compressed_file(inode) &&
		    filp->f_ra.ra_pages < (bdi->ra_pages * COMPR_RA_MUL))
			filp->f_ra.ra_pages = bdi->ra_pages * COMPR_RA_MUL;
		spin_lock(&filp->f_lock);
		filp->f_mode &= ~FMODE_RANDOM;
		spin_unlock(&filp->f_lock);
		return 0;
	}

	err = generic_fadvise(filp, offset, len, advice);
	if (!err && advice == POSIX_FADV_DONTNEED &&
		test_opt(F2FS_I_SB(inode), COMPRESS_CACHE) &&
		f2fs_compressed_file(inode))
		f2fs_invalidate_compress_pages(F2FS_I_SB(inode), inode->i_ino);

	return err;
}

#ifdef CONFIG_COMPAT
struct compat_f2fs_gc_range {
	u32 sync;
	compat_u64 start;
	compat_u64 len;
};
#define F2FS_IOC32_GARBAGE_COLLECT_RANGE	_IOW(F2FS_IOCTL_MAGIC, 11,\
						struct compat_f2fs_gc_range)

static int f2fs_compat_ioc_gc_range(struct file *file, unsigned long arg)
{
	struct compat_f2fs_gc_range __user *urange;
	struct f2fs_gc_range range;
	int err;

	urange = compat_ptr(arg);
	err = get_user(range.sync, &urange->sync);
	err |= get_user(range.start, &urange->start);
	err |= get_user(range.len, &urange->len);
	if (err)
		return -EFAULT;

	return __f2fs_ioc_gc_range(file, &range);
}

struct compat_f2fs_move_range {
	u32 dst_fd;
	compat_u64 pos_in;
	compat_u64 pos_out;
	compat_u64 len;
};
#define F2FS_IOC32_MOVE_RANGE		_IOWR(F2FS_IOCTL_MAGIC, 9,	\
					struct compat_f2fs_move_range)

static int f2fs_compat_ioc_move_range(struct file *file, unsigned long arg)
{
	struct compat_f2fs_move_range __user *urange;
	struct f2fs_move_range range;
	int err;

	urange = compat_ptr(arg);
	err = get_user(range.dst_fd, &urange->dst_fd);
	err |= get_user(range.pos_in, &urange->pos_in);
	err |= get_user(range.pos_out, &urange->pos_out);
	err |= get_user(range.len, &urange->len);
	if (err)
		return -EFAULT;

	return __f2fs_ioc_move_range(file, &range);
}

long f2fs_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (unlikely(f2fs_cp_error(F2FS_I_SB(file_inode(file)))))
		return -EIO;
	if (!f2fs_is_checkpoint_ready(F2FS_I_SB(file_inode(file))))
		return -ENOSPC;

	switch (cmd) {
	case FS_IOC32_GETVERSION:
		cmd = FS_IOC_GETVERSION;
		break;
	case F2FS_IOC32_GARBAGE_COLLECT_RANGE:
		return f2fs_compat_ioc_gc_range(file, arg);
	case F2FS_IOC32_MOVE_RANGE:
		return f2fs_compat_ioc_move_range(file, arg);
	case F2FS_IOC_START_ATOMIC_WRITE:
	case F2FS_IOC_START_ATOMIC_REPLACE:
	case F2FS_IOC_COMMIT_ATOMIC_WRITE:
	case F2FS_IOC_START_VOLATILE_WRITE:
	case F2FS_IOC_RELEASE_VOLATILE_WRITE:
	case F2FS_IOC_ABORT_ATOMIC_WRITE:
	case F2FS_IOC_SHUTDOWN:
	case FITRIM:
	case FS_IOC_SET_ENCRYPTION_POLICY:
	case FS_IOC_GET_ENCRYPTION_PWSALT:
	case FS_IOC_GET_ENCRYPTION_POLICY:
	case FS_IOC_GET_ENCRYPTION_POLICY_EX:
	case FS_IOC_ADD_ENCRYPTION_KEY:
	case FS_IOC_REMOVE_ENCRYPTION_KEY:
	case FS_IOC_REMOVE_ENCRYPTION_KEY_ALL_USERS:
	case FS_IOC_GET_ENCRYPTION_KEY_STATUS:
	case FS_IOC_GET_ENCRYPTION_NONCE:
	case F2FS_IOC_GARBAGE_COLLECT:
	case F2FS_IOC_WRITE_CHECKPOINT:
	case F2FS_IOC_DEFRAGMENT:
	case F2FS_IOC_FLUSH_DEVICE:
	case F2FS_IOC_GET_FEATURES:
	case F2FS_IOC_GET_PIN_FILE:
	case F2FS_IOC_SET_PIN_FILE:
	case F2FS_IOC_PRECACHE_EXTENTS:
	case F2FS_IOC_RESIZE_FS:
	case FS_IOC_ENABLE_VERITY:
	case FS_IOC_MEASURE_VERITY:
	case FS_IOC_READ_VERITY_METADATA:
	case FS_IOC_GETFSLABEL:
	case FS_IOC_SETFSLABEL:
	case F2FS_IOC_GET_COMPRESS_BLOCKS:
	case F2FS_IOC_RELEASE_COMPRESS_BLOCKS:
	case F2FS_IOC_RESERVE_COMPRESS_BLOCKS:
	case F2FS_IOC_SEC_TRIM_FILE:
	case F2FS_IOC_GET_COMPRESS_OPTION:
	case F2FS_IOC_SET_COMPRESS_OPTION:
	case F2FS_IOC_DECOMPRESS_FILE:
	case F2FS_IOC_COMPRESS_FILE:
#ifdef CONFIG_F2FS_APPBOOST
	case F2FS_IOC_START_MERGE_FILE:
	case F2FS_IOC_END_MERGE_FILE:
	case F2FS_IOC_PRELOAD_FILE:
	case F2FS_IOC_ABORT_PRELOAD_FILE:
#endif
#ifdef CONFIG_F2FS_FS_DEDUP
	case F2FS_IOC_DEDUP_CREATE:
	case F2FS_IOC_DEDUP_FILE:
	case F2FS_IOC_DEDUP_REVOKE:
	case F2FS_IOC_CLONE_FILE:
	case F2FS_IOC_MODIFY_CHECK:
	case F2FS_IOC_DEDUP_PERM_CHECK:
	case F2FS_IOC_DEDUP_GET_FILE_INFO:
	case F2FS_IOC_DEDUP_GET_SYS_INFO:
	case F2FS_IOC_SNAPSHOT_CREATE:
	case F2FS_IOC_SNAPSHOT_PREPARE:
#endif
	case F2FS_IOC_GET_EXTRA_ATTR:
	case F2FS_IOC_SET_EXTRA_ATTR:
#ifdef CONFIG_F2FS_SEQZONE
	case F2FS_IOC_SET_SEQZONE_FILE:
	case F2FS_IOC_GET_SEQZONE_FILE:
#endif
		break;
	default:
		return -ENOIOCTLCMD;
	}
	return __f2fs_ioctl(file, cmd, (unsigned long) compat_ptr(arg));
}
#endif

const struct file_operations f2fs_file_operations = {
	.llseek		= f2fs_llseek,
	.read_iter	= f2fs_file_read_iter,
	.write_iter	= f2fs_file_write_iter,
	.iopoll		= iocb_bio_iopoll,
	.open		= f2fs_file_open,
	.release	= f2fs_release_file,
	.mmap		= f2fs_file_mmap,
	.flush		= f2fs_file_flush,
	.fsync		= f2fs_sync_file,
	.fallocate	= f2fs_fallocate,
	.unlocked_ioctl	= f2fs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= f2fs_compat_ioctl,
#endif
	.splice_read	= generic_file_splice_read,
	.splice_write	= iter_file_splice_write,
	.fadvise	= f2fs_file_fadvise,
};
