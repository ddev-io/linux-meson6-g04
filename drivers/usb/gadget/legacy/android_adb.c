/*
 * Legacy Android ADB USB gadget for early Android recovery userspace.
 *
 * Provides the pre-FunctionFS ABI used by Android 4.1-era adbd:
 *   /dev/android_adb        - bulk transport
 *   /dev/android_adb_enable - lifetime gate for the USB pull-up
 *
 * Based on the original Android f_adb driver by Mike Lockwood, adapted
 * to the Linux 3.19 composite gadget API and made self-contained.
 */

#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/usb/ch9.h>
#include <linux/usb/composite.h>
#include <linux/usb/gadget.h>
#include <linux/wait.h>

#define ADB_BULK_BUFFER_SIZE	4096
#define ADB_TX_REQ_MAX		4

struct adb_dev {
	struct usb_function function;
	struct usb_composite_dev *cdev;
	struct usb_ep *ep_in;
	struct usb_ep *ep_out;
	struct usb_request *rx_req;
	struct list_head tx_idle;
	spinlock_t lock;
	wait_queue_head_t read_wq;
	wait_queue_head_t write_wq;
	atomic_t online;
	atomic_t error;
	atomic_t read_excl;
	atomic_t write_excl;
	atomic_t open_excl;
	atomic_t enable_excl;
	bool rx_done;
	bool activated;
};

static struct adb_dev *adb_device_data;
static DEFINE_MUTEX(adb_enable_lock);

static struct usb_interface_descriptor adb_interface_desc = {
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
	.bInterfaceSubClass = 0x42,
	.bInterfaceProtocol = 0x01,
};

static struct usb_endpoint_descriptor fs_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor fs_out_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_OUT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor hs_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = cpu_to_le16(512),
};

static struct usb_endpoint_descriptor hs_out_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_OUT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = cpu_to_le16(512),
};

static struct usb_descriptor_header *fs_adb_descs[] = {
	(struct usb_descriptor_header *)&adb_interface_desc,
	(struct usb_descriptor_header *)&fs_in_desc,
	(struct usb_descriptor_header *)&fs_out_desc,
	NULL,
};

static struct usb_descriptor_header *hs_adb_descs[] = {
	(struct usb_descriptor_header *)&adb_interface_desc,
	(struct usb_descriptor_header *)&hs_in_desc,
	(struct usb_descriptor_header *)&hs_out_desc,
	NULL,
};

static struct usb_request *adb_request_alloc(struct usb_ep *ep)
{
	struct usb_request *req;

	req = usb_ep_alloc_request(ep, GFP_KERNEL);
	if (!req)
		return NULL;
	req->buf = kmalloc(ADB_BULK_BUFFER_SIZE, GFP_KERNEL);
	if (!req->buf) {
		usb_ep_free_request(ep, req);
		return NULL;
	}
	INIT_LIST_HEAD(&req->list);
	return req;
}

static void adb_request_free(struct usb_ep *ep, struct usb_request *req)
{
	if (!req)
		return;
	kfree(req->buf);
	usb_ep_free_request(ep, req);
}

static int adb_exclusive_lock(atomic_t *excl)
{
	if (atomic_inc_return(excl) == 1)
		return 0;
	atomic_dec(excl);
	return -EBUSY;
}

static void adb_exclusive_unlock(atomic_t *excl)
{
	atomic_dec(excl);
}

static void adb_req_put(struct adb_dev *dev, struct usb_request *req)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	list_add_tail(&req->list, &dev->tx_idle);
	spin_unlock_irqrestore(&dev->lock, flags);
}

static struct usb_request *adb_req_get(struct adb_dev *dev)
{
	struct usb_request *req = NULL;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	if (!list_empty(&dev->tx_idle)) {
		req = list_first_entry(&dev->tx_idle, struct usb_request, list);
		list_del_init(&req->list);
	}
	spin_unlock_irqrestore(&dev->lock, flags);
	return req;
}

static void adb_complete_in(struct usb_ep *ep, struct usb_request *req)
{
	struct adb_dev *dev = req->context;

	if (req->status)
		atomic_set(&dev->error, 1);
	adb_req_put(dev, req);
	wake_up(&dev->write_wq);
	wake_up(&dev->read_wq);
}

static void adb_complete_out(struct usb_ep *ep, struct usb_request *req)
{
	struct adb_dev *dev = req->context;

	if (req->status && req->status != -ECONNRESET)
		atomic_set(&dev->error, 1);
	dev->rx_done = true;
	wake_up(&dev->read_wq);
	wake_up(&dev->write_wq);
}

static ssize_t adb_read(struct file *file, char __user *buf,
			 size_t count, loff_t *pos)
{
	struct adb_dev *dev = file->private_data;
	struct usb_request *req = dev->rx_req;
	int ret;
	ssize_t result;

	if (!count || count > ADB_BULK_BUFFER_SIZE)
		return -EINVAL;
	ret = adb_exclusive_lock(&dev->read_excl);
	if (ret)
		return ret;

	ret = wait_event_interruptible(dev->read_wq,
		atomic_read(&dev->online) || atomic_read(&dev->error));
	if (ret) {
		result = ret;
		goto out;
	}
	if (atomic_read(&dev->error)) {
		result = -EIO;
		goto out;
	}

retry:
	dev->rx_done = false;
	req->length = count;
	ret = usb_ep_queue(dev->ep_out, req, GFP_ATOMIC);
	if (ret) {
		atomic_set(&dev->error, 1);
		result = -EIO;
		goto out;
	}

	ret = wait_event_interruptible(dev->read_wq,
		dev->rx_done || atomic_read(&dev->error));
	if (ret) {
		usb_ep_dequeue(dev->ep_out, req);
		result = ret;
		goto out;
	}
	if (atomic_read(&dev->error)) {
		result = -EIO;
		goto out;
	}
	if (!req->actual)
		goto retry;

	result = min_t(size_t, req->actual, count);
	if (copy_to_user(buf, req->buf, result))
		result = -EFAULT;

out:
	adb_exclusive_unlock(&dev->read_excl);
	return result;
}

static ssize_t adb_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *pos)
{
	struct adb_dev *dev = file->private_data;
	struct usb_request *req = NULL;
	size_t left = count;
	int ret;
	ssize_t result = count;

	if (!count)
		return -EINVAL;
	ret = adb_exclusive_lock(&dev->write_excl);
	if (ret)
		return ret;

	while (left) {
		if (atomic_read(&dev->error)) {
			result = -EIO;
			break;
		}
		ret = wait_event_interruptible(dev->write_wq,
			(req = adb_req_get(dev)) || atomic_read(&dev->error));
		if (ret) {
			result = ret;
			break;
		}
		if (!req) {
			result = -EIO;
			break;
		}
		req->length = min_t(size_t, left, ADB_BULK_BUFFER_SIZE);
		if (copy_from_user(req->buf, buf, req->length)) {
			result = -EFAULT;
			break;
		}
		ret = usb_ep_queue(dev->ep_in, req, GFP_ATOMIC);
		if (ret) {
			atomic_set(&dev->error, 1);
			result = -EIO;
			break;
		}
		buf += req->length;
		left -= req->length;
		req = NULL;
	}
	if (req)
		adb_req_put(dev, req);
	adb_exclusive_unlock(&dev->write_excl);
	return result;
}

static int adb_open(struct inode *inode, struct file *file)
{
	struct adb_dev *dev = adb_device_data;
	int ret;

	if (!dev)
		return -ENODEV;
	ret = adb_exclusive_lock(&dev->open_excl);
	if (ret)
		return ret;
	file->private_data = dev;
	atomic_set(&dev->error, 0);
	return 0;
}

static int adb_release(struct inode *inode, struct file *file)
{
	struct adb_dev *dev = file->private_data;

	adb_exclusive_unlock(&dev->open_excl);
	return 0;
}

static const struct file_operations adb_fops = {
	.owner = THIS_MODULE,
	.read = adb_read,
	.write = adb_write,
	.open = adb_open,
	.release = adb_release,
	.llseek = no_llseek,
};

static struct miscdevice adb_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "android_adb",
	.fops = &adb_fops,
};

static int adb_enable_open(struct inode *inode, struct file *file)
{
	struct adb_dev *dev = adb_device_data;
	int ret;

	if (!dev || !dev->function.config)
		return -ENODEV;
	ret = adb_exclusive_lock(&dev->enable_excl);
	if (ret)
		return ret;

	mutex_lock(&adb_enable_lock);
	ret = usb_function_activate(&dev->function);
	if (!ret)
		dev->activated = true;
	mutex_unlock(&adb_enable_lock);
	if (ret)
		adb_exclusive_unlock(&dev->enable_excl);
	return ret;
}

static int adb_enable_release(struct inode *inode, struct file *file)
{
	struct adb_dev *dev = adb_device_data;

	if (!dev)
		return 0;
	mutex_lock(&adb_enable_lock);
	if (dev->activated) {
		usb_function_deactivate(&dev->function);
		dev->activated = false;
	}
	mutex_unlock(&adb_enable_lock);
	adb_exclusive_unlock(&dev->enable_excl);
	return 0;
}

static const struct file_operations adb_enable_fops = {
	.owner = THIS_MODULE,
	.open = adb_enable_open,
	.release = adb_enable_release,
	.llseek = no_llseek,
};

static struct miscdevice adb_enable_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "android_adb_enable",
	.fops = &adb_enable_fops,
};

static int adb_function_bind(struct usb_configuration *c,
			     struct usb_function *f)
{
	struct adb_dev *dev = container_of(f, struct adb_dev, function);
	struct usb_composite_dev *cdev = c->cdev;
	struct usb_request *req;
	int id;
	int i;
	int ret;

	dev->cdev = cdev;
	id = usb_interface_id(c, f);
	if (id < 0)
		return id;
	adb_interface_desc.bInterfaceNumber = id;

	dev->ep_in = usb_ep_autoconfig(cdev->gadget, &fs_in_desc);
	dev->ep_out = usb_ep_autoconfig(cdev->gadget, &fs_out_desc);
	if (!dev->ep_in || !dev->ep_out)
		return -ENODEV;
	dev->ep_in->driver_data = dev;
	dev->ep_out->driver_data = dev;

	hs_in_desc.bEndpointAddress = fs_in_desc.bEndpointAddress;
	hs_out_desc.bEndpointAddress = fs_out_desc.bEndpointAddress;

	dev->rx_req = adb_request_alloc(dev->ep_out);
	if (!dev->rx_req)
		return -ENOMEM;
	dev->rx_req->complete = adb_complete_out;
	dev->rx_req->context = dev;

	for (i = 0; i < ADB_TX_REQ_MAX; i++) {
		req = adb_request_alloc(dev->ep_in);
		if (!req) {
			ret = -ENOMEM;
			goto fail;
		}
		req->complete = adb_complete_in;
		req->context = dev;
		adb_req_put(dev, req);
	}

	ret = usb_assign_descriptors(f, fs_adb_descs, hs_adb_descs, NULL);
	if (ret)
		goto fail;

	ret = usb_function_deactivate(f);
	if (ret) {
		usb_free_all_descriptors(f);
		goto fail;
	}
	return 0;

fail:
	adb_request_free(dev->ep_out, dev->rx_req);
	dev->rx_req = NULL;
	while ((req = adb_req_get(dev)))
		adb_request_free(dev->ep_in, req);
	return ret;
}

static void adb_function_unbind(struct usb_configuration *c,
				struct usb_function *f)
{
	struct adb_dev *dev = container_of(f, struct adb_dev, function);
	struct usb_request *req;

	atomic_set(&dev->online, 0);
	atomic_set(&dev->error, 1);
	wake_up(&dev->read_wq);
	wake_up(&dev->write_wq);
	usb_free_all_descriptors(f);
	adb_request_free(dev->ep_out, dev->rx_req);
	dev->rx_req = NULL;
	while ((req = adb_req_get(dev)))
		adb_request_free(dev->ep_in, req);
}

static int adb_function_set_alt(struct usb_function *f,
				unsigned intf, unsigned alt)
{
	struct adb_dev *dev = container_of(f, struct adb_dev, function);
	struct usb_composite_dev *cdev = f->config->cdev;
	int ret;

	ret = config_ep_by_speed(cdev->gadget, f, dev->ep_in);
	if (ret)
		return ret;
	ret = usb_ep_enable(dev->ep_in);
	if (ret)
		return ret;

	ret = config_ep_by_speed(cdev->gadget, f, dev->ep_out);
	if (ret)
		goto disable_in;
	ret = usb_ep_enable(dev->ep_out);
	if (ret)
		goto disable_in;

	atomic_set(&dev->error, 0);
	atomic_set(&dev->online, 1);
	wake_up(&dev->read_wq);
	return 0;

disable_in:
	usb_ep_disable(dev->ep_in);
	return ret;
}

static void adb_function_disable(struct usb_function *f)
{
	struct adb_dev *dev = container_of(f, struct adb_dev, function);

	atomic_set(&dev->online, 0);
	atomic_set(&dev->error, 1);
	usb_ep_disable(dev->ep_in);
	usb_ep_disable(dev->ep_out);
	wake_up(&dev->read_wq);
	wake_up(&dev->write_wq);
}

static struct usb_device_descriptor adb_device_desc = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = cpu_to_le16(0x0200),
	.bDeviceClass = USB_CLASS_PER_INTERFACE,
	.idVendor = cpu_to_le16(0x18d1),
	.idProduct = cpu_to_le16(0x4e11),
	.bcdDevice = cpu_to_le16(0x0100),
	.bNumConfigurations = 1,
};

static struct usb_string adb_strings[] = {
	[USB_GADGET_MANUFACTURER_IDX].s = "Amlogic",
	[USB_GADGET_PRODUCT_IDX].s = "G04 Recovery ADB",
	[USB_GADGET_SERIAL_IDX].s = "G04",
	{ },
};

static struct usb_gadget_strings adb_stringtab = {
	.language = 0x0409,
	.strings = adb_strings,
};

static struct usb_gadget_strings *adb_dev_strings[] = {
	&adb_stringtab,
	NULL,
};

static struct usb_configuration adb_config = {
	.label = "adb",
	.bConfigurationValue = 1,
	.bmAttributes = USB_CONFIG_ATT_ONE,
	.MaxPower = 500,
};

static int adb_do_config(struct usb_configuration *c)
{
	return usb_add_function(c, &adb_device_data->function);
}

static int adb_composite_bind(struct usb_composite_dev *cdev)
{
	struct adb_dev *dev;
	int ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	spin_lock_init(&dev->lock);
	INIT_LIST_HEAD(&dev->tx_idle);
	init_waitqueue_head(&dev->read_wq);
	init_waitqueue_head(&dev->write_wq);
	dev->function.name = "adb";
	dev->function.bind = adb_function_bind;
	dev->function.unbind = adb_function_unbind;
	dev->function.set_alt = adb_function_set_alt;
	dev->function.disable = adb_function_disable;
	adb_device_data = dev;

	ret = misc_register(&adb_miscdev);
	if (ret)
		goto free_dev;
	ret = misc_register(&adb_enable_miscdev);
	if (ret)
		goto unregister_adb;

	ret = usb_string_ids_tab(cdev, adb_strings);
	if (ret < 0)
		goto unregister_enable;
	adb_device_desc.iManufacturer = adb_strings[USB_GADGET_MANUFACTURER_IDX].id;
	adb_device_desc.iProduct = adb_strings[USB_GADGET_PRODUCT_IDX].id;
	adb_device_desc.iSerialNumber = adb_strings[USB_GADGET_SERIAL_IDX].id;

	ret = usb_add_config(cdev, &adb_config, adb_do_config);
	if (ret)
		goto unregister_enable;

	dev_info(&cdev->gadget->dev,
		 "legacy Android ADB ready; waiting for /dev/android_adb_enable\n");
	return 0;

unregister_enable:
	misc_deregister(&adb_enable_miscdev);
unregister_adb:
	misc_deregister(&adb_miscdev);
free_dev:
	adb_device_data = NULL;
	kfree(dev);
	return ret;
}

static int adb_composite_unbind(struct usb_composite_dev *cdev)
{
	misc_deregister(&adb_enable_miscdev);
	misc_deregister(&adb_miscdev);
	kfree(adb_device_data);
	adb_device_data = NULL;
	return 0;
}

static struct usb_composite_driver adb_driver = {
	.name = "g_android_adb",
	.dev = &adb_device_desc,
	.strings = adb_dev_strings,
	.max_speed = USB_SPEED_HIGH,
	.bind = adb_composite_bind,
	.unbind = adb_composite_unbind,
};

module_usb_composite_driver(adb_driver);

MODULE_DESCRIPTION("Legacy Android ADB USB gadget");
MODULE_AUTHOR("Google, adapted for Meson6 G04");
MODULE_LICENSE("GPL v2");
