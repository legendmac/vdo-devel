/*
 * FOR INTERNAL USE ONLY, DO NOT DISTRIBUTE!!!!
 *
 * A moderately heavily edited version of the kernel's kobject.c implementing
 * the functions we use with our library and something of our style, but
 * retaining (most of) the kerneldoc, naming, and as much code structure as
 * possible.
 *
 * $Id$
 */

#include <linux/device-mapper.h>
#include <linux/kobject.h>

#include "memory-alloc.h"

#include "status-codes.h"

static struct device *the_fake_device = NULL;
static struct kobj_type fake_device_type;

/**********************************************************************/
static void release_fake_device(struct kobject *kobj)
{
	UDS_FREE(kobj);
	the_fake_device = NULL;
}

/**********************************************************************/
struct device *disk_to_dev(void *disk __attribute__((unused)))
{
	if (the_fake_device != NULL) {
		return the_fake_device;
	}

	int result = UDS_ALLOCATE(1, struct device, __func__, &the_fake_device);
	if (result != VDO_SUCCESS) {
		return NULL;
	}

	fake_device_type.release = release_fake_device;
	kobject_init(&the_fake_device->kobj, &fake_device_type);
	result = kobject_add(&the_fake_device->kobj,
			     kernel_kobj,
			     "%s",
			     "fake device");
	if (result != VDO_SUCCESS) {
		UDS_FREE(the_fake_device);
		the_fake_device = NULL;
	}

	return the_fake_device;
}