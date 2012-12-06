#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/module.h>

static DEFINE_IDR(test_idr);

int init_module(void)
{
	int ret, forty95, forty96;
	void *addr;

again1:
	if (!idr_pre_get(&test_idr, GFP_KERNEL))
		return -ENOMEM;

	ret = idr_get_new_above(&test_idr, (void *)4095, 4095, &forty95);
	if (ret) {
		if (ret == -EAGAIN)
			goto again1;
		return ret;
	}

	if (forty95 != 4095)
		printk(KERN_ERR "hmmm, forty95=%d (should be 4095)\n", forty95);

again2:
	if (!idr_pre_get(&test_idr, GFP_KERNEL))
		return -ENOMEM;

	ret = idr_get_new_above(&test_idr, (void *)4096, 4095, &forty96);
	if (ret) {
		if (ret == -EAGAIN)
			goto again2;
		return ret;
	}

	addr = idr_find(&test_idr, forty95);
	if ((int)addr != forty95)
		printk(KERN_ERR "hmmm, after find forty95=%d addr=%d\n", forty95, (int)addr);

	addr = idr_find(&test_idr, forty96);
	if ((int)addr != forty96)
		printk(KERN_ERR "hmmm, after find forty96=%d addr=%d\n", forty96, (int)addr);

	addr = idr_find(&test_idr, 0);
	if ((int)addr)
		printk(KERN_ERR "found an entry at id=0 for addr=%d\n", (int)addr);

	idr_remove(&test_idr, forty95);
	idr_remove(&test_idr, forty96);

	return 0;
}

void cleanup_module(void)
{
}

MODULE_AUTHOR("Eric Paris <eparis@redhat.com>");
MODULE_DESCRIPTION("Simple idr test");
MODULE_LICENSE("GPL");
