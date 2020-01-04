
#include <stdio.h>
#include <sys/ioctl.h>

#include <uuid/uuid.h>

#include "ccan/darray/darray.h"

#include "linux/sort.h"

#include "libbcachefs/bcachefs_ioctl.h"
#include "libbcachefs/opts.h"

#include "cmds.h"
#include "libbcachefs.h"

static void print_dev_usage(struct bchfs_handle fs,
			    struct dev_name *d,
			    enum units units)
{
	struct bch_ioctl_dev_usage u = bchu_dev_usage(fs, d->idx);
	u64 available = u.nr_buckets;
	unsigned i;

	printf("\n");
	printf_pad(20, "%s (device %u):", d->label ?: "(no label)", d->idx);
	printf("%24s%12s\n", d->dev ?: "(device not found)", bch2_dev_state[u.state]);

	printf("%-20s%12s%12s%12s\n",
	       "", "data", "buckets", "fragmented");

	for (i = BCH_DATA_SB; i < BCH_DATA_NR; i++) {
		u64 frag = max((s64) u.buckets[i] * u.bucket_size -
			       (s64) u.sectors[i], 0LL);

		printf_pad(20, "  %s:", bch2_data_types[i]);
		printf("%12s%12llu%12s\n",
		       pr_units(u.sectors[i], units),
		       u.buckets[i],
		       pr_units(frag, units));

		if (i != BCH_DATA_CACHED)
			available -= u.buckets[i];
	}

	printf_pad(20, "  available:");
	printf("%12s%12llu\n",
	       pr_units(available * u.bucket_size, units),
	       available);

	printf_pad(20, "  capacity:");
	printf("%12s%12llu\n",
	       pr_units(u.nr_buckets * u.bucket_size, units),
	       u.nr_buckets);
}

static int dev_by_label_cmp(const void *_l, const void *_r)
{
	const struct dev_name *l = _l, *r = _r;

	return  (l->label && r->label
		 ? strcmp(l->label, r->label) : 0) ?:
		(l->dev && r->dev
		 ? strcmp(l->dev, r->dev) : 0) ?:
		cmp_int(l->idx, r->idx);
}

static void print_fs_usage(const char *path, enum units units)
{
	unsigned i;
	char uuid[40];

	struct bchfs_handle fs = bcache_fs_open(path);

	struct dev_name *dev;
	dev_names dev_names = bchu_fs_get_devices(fs);

	struct bch_ioctl_fs_usage *u = bchu_fs_usage(fs);

	uuid_unparse(fs.uuid.b, uuid);
	printf("Filesystem %s:\n", uuid);

	printf("%-20s%12s\n", "Size:", pr_units(u->capacity, units));
	printf("%-20s%12s\n", "Used:", pr_units(u->used, units));

	printf("%-20s%12s\n", "Online reserved:", pr_units(u->online_reserved, units));

	printf("\n");
	printf("%-16s%-16s%s\n", "Data type", "Required/total", "Devices");

	for (i = 0; i < BCH_REPLICAS_MAX; i++) {
		if (!u->persistent_reserved[i])
			continue;

		printf_pad(16, "%s: ", "reserved");
		printf_pad(16, "%u/%u ", 1, i);
		printf_pad(32, "[] ");
		printf("%s\n", pr_units(u->persistent_reserved[i], units));
	}

	struct bch_replicas_usage *r;

	for (r = u->replicas;
	     r != (void *) u->replicas + u->replica_entries_bytes;
	     r = replicas_usage_next(r)) {
		BUG_ON((void *) r > (void *) u->replicas + u->replica_entries_bytes);

		if (!r->sectors)
			continue;

		char devs[4096], *d = devs;
		*d++ = '[';

		for (i = 0; i < r->r.nr_devs; i++) {
			unsigned dev_idx = r->r.devs[i];
			if (i)
				*d++ = ' ';

			darray_foreach(dev, dev_names)
				if (dev->idx == dev_idx)
					goto found;
			d = NULL;
found:
			d += dev && dev->dev
				? sprintf(d, "%s", dev->dev)
				: sprintf(d, "%u", dev_idx);
		}
		*d++ = ']';
		*d++ = '\0';

		printf_pad(16, "%s: ", bch2_data_types[r->r.data_type]);
		printf_pad(16, "%u/%u ", r->r.nr_required, r->r.nr_devs);
		printf_pad(32, "%s ", devs);
		printf(" %s\n", pr_units(r->sectors, units));
	}

	free(u);

	sort(&darray_item(dev_names, 0), darray_size(dev_names),
	     sizeof(darray_item(dev_names, 0)), dev_by_label_cmp, NULL);

	darray_foreach(dev, dev_names)
		print_dev_usage(fs, dev, units);

	darray_foreach(dev, dev_names) {
		free(dev->dev);
		free(dev->label);
	}
	darray_free(dev_names);

	bcache_fs_close(fs);
}

int cmd_fs_usage(int argc, char *argv[])
{
	enum units units = BYTES;
	char *fs;
	int opt;

	while ((opt = getopt(argc, argv, "h")) != -1)
		switch (opt) {
		case 'h':
			units = HUMAN_READABLE;
			break;
		}
	args_shift(optind);

	if (!argc) {
		print_fs_usage(".", units);
	} else {
		while ((fs = arg_pop()))
			print_fs_usage(fs, units);
	}

	return 0;
}
