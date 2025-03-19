// SPDX-License-Identifier: GPL-2.0
/*
 * Parallel page copy routine.
 */

#include <linux/module.h>
#include <linux/printk.h>
#include <linux/init.h>
#include <linux/sysctl.h>
#include <linux/sysfs.h>
#include <linux/highmem.h>
#include <linux/padata.h>
#include <linux/slab.h>
#include <linux/migrate.h>
#include <linux/migrate_offc.h>

/* A sensible limit to prevent creating an excessive number of threads. */
#define MAX_NUM_COPY_THREADS 64

unsigned int limit_mt_num = 4;
static int offloading_enabled;

static int copy_page_lists_mt(struct list_head *dst_folios,
		struct list_head *src_folios, unsigned int nr_items);

static DEFINE_MUTEX(migratecfg_mutex);

/* CPU Multithreaded Batch Migrator */
struct migrator cpu_migrator = {
	.name = "CPU_MT_COPY\0",
	.migrate_offload_copy = copy_page_lists_mt,
	.owner = THIS_MODULE,
};

struct copy_item {
	char *to;
	char *from;
	unsigned long chunk_size;
};

struct copy_page_info {
	int ret;
	unsigned long num_items;
	struct copy_item item_list[] __counted_by(num_items);
};

static unsigned long copy_page_routine(char *vto, char *vfrom,
	unsigned long chunk_size)
{
	return copy_mc_to_kernel(vto, vfrom, chunk_size);
}

static void copy_page_work_thread(unsigned long start, unsigned long end, void *arg)
{
	unsigned long i, j;
	struct copy_page_info **work_item = (struct copy_page_info**)arg;

	for (i = start; i < end; i++)
		for (j = 0; j < work_item[i]->num_items; j++)
			work_item[i]->ret |= !!copy_page_routine(
				work_item[i]->item_list[j].to,
				work_item[i]->item_list[j].from,
				work_item[i]->item_list[j].chunk_size);
}

int copy_page_lists_mt(struct list_head *dst_folios,
		struct list_head *src_folios, unsigned int nr_items)
{
	struct copy_page_info *work_items[MAX_NUM_COPY_THREADS] = {};
	unsigned int total_mt_num = limit_mt_num;
	struct folio *src, *src2, *dst, *dst2;
	struct padata_mt_job job = {
		.thread_fn = copy_page_work_thread,
		.fn_arg = &work_items,
		.start = 0,
		.align = 1,
		.min_chunk = 1,
		.numa_aware = true,
	};
	int max_items_per_thread;
	int item_idx;
	int err = 0;
	int cpu;
	int i;

	if (IS_ENABLED(CONFIG_HIGHMEM))
		return -ENOTSUPP;

	if (total_mt_num > MAX_NUM_COPY_THREADS)
		total_mt_num = MAX_NUM_COPY_THREADS;

	/*
	 * If nr_items < total_mt_num, each thread gets part of each page.
	 * If there are more threads than pages, split each page across multiple
	 * threads.
	 */
	if (nr_items < total_mt_num)
		max_items_per_thread = nr_items;
	else
		max_items_per_thread = (nr_items / total_mt_num) +
				((nr_items % total_mt_num) ? 1 : 0);


	for (cpu = 0; cpu < total_mt_num; ++cpu) {
		work_items[cpu] = kzalloc(struct_size(work_items[cpu],
						      item_list, max_items_per_thread),
					  GFP_NOWAIT);
		if (!work_items[cpu]) {
			err = -ENOMEM;
			goto free_work_items;
		}
	}

	job.max_threads = total_mt_num;
	job.size = total_mt_num;

	if (nr_items < total_mt_num) {
		for (cpu = 0; cpu < total_mt_num; ++cpu)
			work_items[cpu]->num_items = max_items_per_thread;

		item_idx = 0;
		dst = list_first_entry(dst_folios, struct folio, lru);
		dst2 = list_next_entry(dst, lru);
		list_for_each_entry_safe(src, src2, src_folios, lru) {
			unsigned long chunk_size = PAGE_SIZE * folio_nr_pages(src) / total_mt_num;
			char *vfrom = page_address(&src->page);
			char *vto = page_address(&dst->page);

			VM_WARN_ON(PAGE_SIZE * folio_nr_pages(src) % total_mt_num);
			VM_WARN_ON(folio_nr_pages(dst) != folio_nr_pages(src));

			for (cpu = 0; cpu < total_mt_num; ++cpu) {
				work_items[cpu]->item_list[item_idx].to =
					vto + chunk_size * cpu;
				work_items[cpu]->item_list[item_idx].from =
					vfrom + chunk_size * cpu;
				work_items[cpu]->item_list[item_idx].chunk_size =
					chunk_size;
			}

			item_idx++;
			dst = dst2;
			dst2 = list_next_entry(dst, lru);
		}
	} else {
		int num_xfer_per_thread = nr_items / total_mt_num;
		int per_cpu_item_idx;

		for (cpu = 0; cpu < total_mt_num; ++cpu) {
			work_items[cpu]->num_items = num_xfer_per_thread +
					(cpu < (nr_items % total_mt_num));
		}

		cpu = 0;
		per_cpu_item_idx = 0;
		item_idx = 0;
		dst = list_first_entry(dst_folios, struct folio, lru);
		dst2 = list_next_entry(dst, lru);
		list_for_each_entry_safe(src, src2, src_folios, lru) {
			work_items[cpu]->item_list[per_cpu_item_idx].to =
				page_address(&dst->page);
			work_items[cpu]->item_list[per_cpu_item_idx].from =
				page_address(&src->page);
			work_items[cpu]->item_list[per_cpu_item_idx].chunk_size =
				PAGE_SIZE * folio_nr_pages(src);

			VM_WARN_ON(folio_nr_pages(dst) !=
				   folio_nr_pages(src));

			per_cpu_item_idx++;
			item_idx++;
			dst = dst2;
			dst2 = list_next_entry(dst, lru);

			if (per_cpu_item_idx == work_items[cpu]->num_items) {
				per_cpu_item_idx = 0;
				cpu++;
			}
		}
		if (item_idx != nr_items)
			pr_warn("%s: only %d out of %d pages are transferred\n",
				__func__, item_idx - 1, nr_items);

	}

	padata_do_multithreaded(&job);

	for (i = 0; i < total_mt_num; ++i) {
		/* retry if any copy fails */
		if (work_items[i]->ret)
			err = -EAGAIN;
	}

free_work_items:
	for (cpu = 0; cpu < total_mt_num; ++cpu)
		kfree(work_items[cpu]);

	return err;
}

static ssize_t offloading_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", offloading_enabled);
}

static ssize_t offloading_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	mutex_lock(&migratecfg_mutex);

	if (enable == offloading_enabled) {
		pr_err("migration offloading is already %s\n",
			 enable ? "ON" : "OFF");
		goto out;
	}

	if (enable) {
		start_offloading(&cpu_migrator);
		offloading_enabled = true;
		pr_info("migration offloading is now ON\n");
	} else {
		stop_offloading();
		offloading_enabled = false;
		pr_info("migration offloading is now OFF\n");
	}
out:
	mutex_unlock(&migratecfg_mutex);
	return count;
}

static ssize_t threads_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", limit_mt_num);
}

static ssize_t threads_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;

	if (val < 1 || val > MAX_NUM_COPY_THREADS)
		return -EINVAL;

	mutex_lock(&migratecfg_mutex);
	limit_mt_num = val;
	mutex_unlock(&migratecfg_mutex);

	return count;
}

static struct kobj_attribute offloading_attr = __ATTR_RW(offloading);
static struct kobj_attribute threads_attr = __ATTR_RW(threads);

static struct attribute *cpu_mt_attrs[] = {
	&offloading_attr.attr,
	&threads_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(cpu_mt);

static struct kobject *cpu_mt_kobj;

static int __init cpu_mt_module_init(void)
{
	int ret;

	cpu_mt_kobj = kobject_create_and_add("cpu_mt_copy", kernel_kobj);
	if (!cpu_mt_kobj)
		return -ENOMEM;

	ret = sysfs_create_groups(cpu_mt_kobj, cpu_mt_groups);
	if (ret) {
		kobject_put(cpu_mt_kobj);
		return ret;
	}

	pr_info("CPU_MT Migrator initialized\n");
	return 0;
}

static void __exit cpu_mt_module_exit(void)
{
	/* Stop the MT offloading to unload the module */
	mutex_lock(&migratecfg_mutex);
	if (offloading_enabled) {
		stop_offloading();
		offloading_enabled = false;
	}
	mutex_unlock(&migratecfg_mutex);

	sysfs_remove_groups(cpu_mt_kobj, cpu_mt_groups);
	kobject_put(cpu_mt_kobj);

	pr_info("CPU MT Migrator unloaded\n");
}

module_init(cpu_mt_module_init);
module_exit(cpu_mt_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zi Yan");
MODULE_DESCRIPTION("CPU Multithreaded Batch Page Migrator");
