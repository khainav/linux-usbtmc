/**
 * drivers/usb/class/usbtmc.c - USB Test & Measurement class driver
 *
 * Copyright (C) 2007 Stefan Kopp, Gechingen, Germany
 * Copyright (C) 2008 Novell, Inc.
 * Copyright (C) 2008 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (C) 2015 Dave Penkler <dpenkler@gmail.com>
 * Copyright (C) 2018 Guido Kiener <guido@kiener-muenchen.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * The GNU General Public License is available at
 * http://www.gnu.org/copyleft/gpl.html.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
//#define DEBUG
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/usb.h>
#include "tmc.h"

#define USBTMC_VERSION "1.2"

#define USBTMC_HEADER_SIZE	12
#define USBTMC_MINOR_BASE	176

/*
 * Size of driver internal IO buffer. Must be multiple of 4 and at least as
 * large as wMaxPacketSize (which is usually 512 bytes).
 */
#define USBTMC_SIZE_IOBUFFER	2048

/* Minimum USB timeout (in milliseconds) */
#define USBTMC_MIN_TIMEOUT	500
/* Default USB timeout (in milliseconds) */
#define USBTMC_TIMEOUT		5000

static unsigned int io_buffer_size = USBTMC_SIZE_IOBUFFER;
module_param(io_buffer_size, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(io_buffer_size, "Size of bulk IO buffer in bytes");

static unsigned int usb_timeout = USBTMC_TIMEOUT;
module_param(usb_timeout, uint,  S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(usb_timeout, "USB timeout in milliseconds");

/*
 * Maximum number of read cycles to empty bulk in endpoint during CLEAR and
 * ABORT_BULK_IN requests. Ends the loop if (for whatever reason) a short
 * packet is never read.
 */
#define USBTMC_MAX_READS_TO_CLEAR_BULK_IN	100

static const struct usb_device_id usbtmc_devices[] = {
	{ USB_INTERFACE_INFO(USB_CLASS_APP_SPEC, 3, 0), },
	{ USB_INTERFACE_INFO(USB_CLASS_APP_SPEC, 3, 1), },
	{ 0, } /* terminating entry */
};
MODULE_DEVICE_TABLE(usb, usbtmc_devices);

/*
 * This structure is the capabilities for the device
 * See section 4.2.1.8 of the USBTMC specification,
 * and section 4.2.2 of the USBTMC usb488 subclass
 * specification for details.
 */
struct usbtmc_dev_capabilities {
	__u8 interface_capabilities;
	__u8 device_capabilities;
	__u8 usb488_interface_capabilities;
	__u8 usb488_device_capabilities;
};

/* This structure holds private data for each USBTMC device. One copy is
 * allocated for each USBTMC device in the driver's probe function.
 */
struct usbtmc_device_data {
	const struct usb_device_id *id;
	struct usb_device *usb_dev;
	struct usb_interface *intf;
	struct list_head file_list;

	unsigned int bulk_in;
	unsigned int bulk_out;

	u8 bTag;
	u8 bTag_last_write;	/* needed for abort */
	u8 bTag_last_read;	/* needed for abort */

	/* data for interrupt in endpoint handling */
	u8             bNotify1;
	u8             bNotify2;
	u16            ifnum;
	u8             iin_bTag;
	u8            *iin_buffer;
	atomic_t       iin_data_valid;
	unsigned int   iin_ep;
	int            iin_ep_present;
	int            iin_interval;
	struct urb    *iin_urb;
	u16            iin_wMaxPacketSize;

	/* coalesced usb488_caps from usbtmc_dev_capabilities */
	__u8 usb488_caps;

	/* request timeout */
	u32 timeout;
	/* attributes from the USB TMC spec for this device */
	/* They are used as default values for file_data */
	u8 TermChar;
	bool TermCharEnabled;
	bool auto_abort;
	bool eom_val;

	bool zombie; /* fd of disconnected device */

	struct usbtmc_dev_capabilities	capabilities;
	struct kref kref;
	struct mutex io_mutex;	/* only one i/o function running at a time */
	wait_queue_head_t waitq;
	struct fasync_struct *fasync;
	spinlock_t dev_lock; /* lock for file_list */
};
#define to_usbtmc_data(d) container_of(d, struct usbtmc_device_data, kref)

/*
 * This structure holds private data for each USBTMC file handle.
 */
struct usbtmc_file_data {
	struct usbtmc_device_data *data;
	struct list_head file_elem;

	u8             srq_byte;
	atomic_t       srq_asserted;

	/* These values are initialized with default values from device_data */
	u8             TermChar;
	bool           TermCharEnabled;
	bool           auto_abort;
};

/* Forward declarations */
static struct usb_driver usbtmc_driver;

static void usbtmc_delete(struct kref *kref)
{
	struct usbtmc_device_data *data = to_usbtmc_data(kref);

	pr_debug("%s - called\n", __func__);
	usb_put_dev(data->usb_dev);
	kfree(data);
}

static int usbtmc_open(struct inode *inode, struct file *filp)
{
	struct usb_interface *intf;
	struct usbtmc_device_data *data;
	struct usbtmc_file_data *file_data;

	intf = usb_find_interface(&usbtmc_driver, iminor(inode));
	if (!intf) {
		pr_err("can not find device for minor %d", iminor(inode));
		return -ENODEV;
	}

	file_data = kzalloc(sizeof(*file_data), GFP_KERNEL);
	if (!file_data)
		return -ENOMEM;

	pr_debug("%s - called\n", __func__);

	data = usb_get_intfdata(intf);
	/* Protect reference to data from file structure until release */
	kref_get(&data->kref);

	mutex_lock(&data->io_mutex);
	file_data->data = data;

	/* copy default values from device settings */
	file_data->TermChar = data->TermChar;
	file_data->TermCharEnabled = data->TermCharEnabled;
	file_data->auto_abort = data->auto_abort;

	INIT_LIST_HEAD(&file_data->file_elem);
	spin_lock_irq(&data->dev_lock);
	list_add_tail(&file_data->file_elem, &data->file_list);
	spin_unlock_irq(&data->dev_lock);
	mutex_unlock(&data->io_mutex);

	/* Store pointer in file structure's private data field */
	filp->private_data = file_data;

	return 0;
}

static int usbtmc_release(struct inode *inode, struct file *file)
{
	struct usbtmc_file_data *file_data = file->private_data;

	pr_debug("%s - called\n", __func__);

	/* prevent IO _AND_ usbtmc_interrupt */
	mutex_lock(&file_data->data->io_mutex);
	spin_lock_irq(&file_data->data->dev_lock);

	list_del(&file_data->file_elem);

	spin_unlock_irq(&file_data->data->dev_lock);
	mutex_unlock(&file_data->data->io_mutex);

	kref_put(&file_data->data->kref, usbtmc_delete);
	file_data->data = NULL;
	kfree(file_data);
	return 0;
}

static int usbtmc_ioctl_abort_bulk_in(struct usbtmc_device_data *data)
{
	u8 *buffer;
	struct device *dev;
	int rv;
	int n;
	int actual;
	struct usb_host_interface *current_setting;
	int max_size;

	dev = &data->intf->dev;
	buffer = kmalloc(io_buffer_size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	rv = usb_control_msg(data->usb_dev,
			     usb_rcvctrlpipe(data->usb_dev, 0),
			     USBTMC_REQUEST_INITIATE_ABORT_BULK_IN,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT,
			     data->bTag_last_read, data->bulk_in,
			     buffer, 2, data->timeout);

	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}

	dev_dbg(dev, "INITIATE_ABORT_BULK_IN returned %x\n", buffer[0]);

	if (buffer[0] == USBTMC_STATUS_FAILED) {
		rv = 0;
		goto exit;
	}

	if (buffer[0] != USBTMC_STATUS_SUCCESS) {
		dev_err(dev, "INITIATE_ABORT_BULK_IN returned %x\n",
			buffer[0]);
		rv = -EPERM;
		goto exit;
	}

	max_size = 0;
	current_setting = data->intf->cur_altsetting;
	for (n = 0; n < current_setting->desc.bNumEndpoints; n++)
		if (current_setting->endpoint[n].desc.bEndpointAddress ==
			data->bulk_in)
			max_size = usb_endpoint_maxp(&current_setting->endpoint[n].desc);

	if (max_size == 0) {
		dev_err(dev, "Couldn't get wMaxPacketSize\n");
		rv = -EPERM;
		goto exit;
	}

	dev_dbg(&data->intf->dev, "wMaxPacketSize is %d\n", max_size);

	n = 0;

	do {
		dev_dbg(dev, "Reading from bulk in EP\n");

		rv = usb_bulk_msg(data->usb_dev,
				  usb_rcvbulkpipe(data->usb_dev,
						  data->bulk_in),
				  buffer, io_buffer_size,
				  &actual, data->timeout);

		n++;

		if (rv < 0) {
			dev_err(dev, "usb_bulk_msg returned %d\n", rv);
			goto exit;
		}
	} while ((actual == max_size) &&
		 (n < USBTMC_MAX_READS_TO_CLEAR_BULK_IN));

	if (actual == max_size) {
		dev_err(dev, "Couldn't clear device buffer within %d cycles\n",
			USBTMC_MAX_READS_TO_CLEAR_BULK_IN);
		rv = -EPERM;
		goto exit;
	}

	n = 0;

usbtmc_abort_bulk_in_status:
	rv = usb_control_msg(data->usb_dev,
			     usb_rcvctrlpipe(data->usb_dev, 0),
			     USBTMC_REQUEST_CHECK_ABORT_BULK_IN_STATUS,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT,
			     0, data->bulk_in, buffer, 0x08,
			     data->timeout);

	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}

	dev_dbg(dev, "INITIATE_ABORT_BULK_IN returned %x\n", buffer[0]);

	if (buffer[0] == USBTMC_STATUS_SUCCESS) {
		rv = 0;
		goto exit;
	}

	if (buffer[0] != USBTMC_STATUS_PENDING) {
		dev_err(dev, "INITIATE_ABORT_BULK_IN returned %x\n", buffer[0]);
		rv = -EPERM;
		goto exit;
	}

	if (buffer[1] == 1)
		do {
			dev_dbg(dev, "Reading from bulk in EP\n");

			rv = usb_bulk_msg(data->usb_dev,
					  usb_rcvbulkpipe(data->usb_dev,
							  data->bulk_in),
					  buffer, io_buffer_size,
					  &actual, data->timeout);

			n++;

			if (rv < 0) {
				dev_err(dev, "usb_bulk_msg returned %d\n", rv);
				goto exit;
			}
		} while ((actual == max_size) &&
			 (n < USBTMC_MAX_READS_TO_CLEAR_BULK_IN));

	if (actual == max_size) {
		dev_err(dev, "Couldn't clear device buffer within %d cycles\n",
			USBTMC_MAX_READS_TO_CLEAR_BULK_IN);
		rv = -EPERM;
		goto exit;
	}

	goto usbtmc_abort_bulk_in_status;

exit:
	kfree(buffer);
	return rv;

}

static int usbtmc_ioctl_abort_bulk_out(struct usbtmc_device_data *data)
{
	struct device *dev;
	u8 *buffer;
	int rv;
	int n;

	dev = &data->intf->dev;

	buffer = kmalloc(8, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	rv = usb_control_msg(data->usb_dev,
			     usb_rcvctrlpipe(data->usb_dev, 0),
			     USBTMC_REQUEST_INITIATE_ABORT_BULK_OUT,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT,
			     data->bTag_last_write, data->bulk_out,
			     buffer, 2, data->timeout);

	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}

	dev_dbg(dev, "INITIATE_ABORT_BULK_OUT returned %x\n", buffer[0]);

	if (buffer[0] != USBTMC_STATUS_SUCCESS) {
		dev_err(dev, "INITIATE_ABORT_BULK_OUT returned %x\n",
			buffer[0]);
		rv = -EPERM;
		goto exit;
	}

	n = 0;

usbtmc_abort_bulk_out_check_status:
	rv = usb_control_msg(data->usb_dev,
			     usb_rcvctrlpipe(data->usb_dev, 0),
			     USBTMC_REQUEST_CHECK_ABORT_BULK_OUT_STATUS,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT,
			     0, data->bulk_out, buffer, 0x08,
			     data->timeout);
	n++;
	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}

	dev_dbg(dev, "CHECK_ABORT_BULK_OUT returned %x\n", buffer[0]);

	if (buffer[0] == USBTMC_STATUS_SUCCESS)
		goto usbtmc_abort_bulk_out_clear_halt;

	if ((buffer[0] == USBTMC_STATUS_PENDING) &&
	    (n < USBTMC_MAX_READS_TO_CLEAR_BULK_IN))
		goto usbtmc_abort_bulk_out_check_status;

	rv = -EPERM;
	goto exit;

usbtmc_abort_bulk_out_clear_halt:
	rv = usb_clear_halt(data->usb_dev,
			    usb_sndbulkpipe(data->usb_dev, data->bulk_out));

	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}
	rv = 0;

exit:
	kfree(buffer);
	return rv;
}

static int usbtmc488_ioctl_read_stb(struct usbtmc_file_data *file_data,
				void __user *arg)
{
	struct usbtmc_device_data *data = file_data->data;
	struct device *dev = &data->intf->dev;
	int srq_asserted = 0;
	u8 *buffer;
	u8 tag;
	__u8 stb;
	int rv;

	dev_dbg(dev, "Enter ioctl_read_stb iin_ep_present: %d\n",
		data->iin_ep_present);

	spin_lock_irq(&data->dev_lock);
	srq_asserted = atomic_xchg(&file_data->srq_asserted, srq_asserted);
	if (srq_asserted) {
		/* a STB with SRQ is already received */
		stb = file_data->srq_byte;
		spin_unlock_irq(&data->dev_lock);
		rv = put_user(stb, (__u8 __user *)arg);
		dev_dbg(dev, "stb:0x%02x with srq received %d\n",
			(unsigned int)stb, rv);
		if (rv)
			return -EFAULT;
		return rv;
	}
	spin_unlock_irq(&data->dev_lock);

	buffer = kmalloc(8, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	atomic_set(&data->iin_data_valid, 0);

	rv = usb_control_msg(data->usb_dev,
			usb_rcvctrlpipe(data->usb_dev, 0),
			USBTMC488_REQUEST_READ_STATUS_BYTE,
			USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			data->iin_bTag,
			data->ifnum,
			buffer, 0x03, data->timeout);
	if (rv < 0) {
		dev_err(dev, "stb usb_control_msg returned %d\n", rv);
		goto exit;
	}

	if (buffer[0] != USBTMC_STATUS_SUCCESS) {
		dev_err(dev, "control status returned %x\n", buffer[0]);
		rv = -EIO;
		goto exit;
	}

	if (data->iin_ep_present) {
		rv = wait_event_interruptible_timeout(
			data->waitq,
			atomic_read(&data->iin_data_valid) != 0,
			data->timeout);
		if (rv < 0) {
			dev_dbg(dev, "wait interrupted %d\n", rv);
			goto exit;
		}

		if (rv == 0) {
			dev_dbg(dev, "wait timed out\n");
			rv = -ETIMEDOUT;
			goto exit;
		}

		tag = data->bNotify1 & 0x7f;
		if (tag != data->iin_bTag) {
			dev_err(dev, "expected bTag %x got %x\n",
				data->iin_bTag, tag);
		}

		stb = data->bNotify2;
	} else {
		stb = buffer[2];
	}

	if (put_user(stb, (__u8 __user *)arg))
		rv = -EFAULT;
	else
		rv = 0;
	dev_dbg(dev, "stb:0x%02x received %d\n", (unsigned int)stb, rv);

 exit:
	/* bump interrupt bTag */
	data->iin_bTag += 1;
	if (data->iin_bTag > 127)
		/* 1 is for SRQ see USBTMC-USB488 subclass spec section 4.3.1 */
		data->iin_bTag = 2;

	kfree(buffer);
	return rv;
}

static int usbtmc488_ioctl_simple(struct usbtmc_device_data *data,
				void __user *arg, unsigned int cmd)
{
	struct device *dev = &data->intf->dev;
	__u8 val;
	u8 *buffer;
	u16 wValue;
	int rv;

	if (!(data->usb488_caps & USBTMC488_CAPABILITY_SIMPLE))
		return -EINVAL;

	buffer = kmalloc(8, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	if (cmd == USBTMC488_REQUEST_REN_CONTROL) {
		rv = copy_from_user(&val, arg, sizeof(val));
		if (rv) {
			rv = -EFAULT;
			goto exit;
		}
		wValue = val ? 1 : 0;
	} else {
		wValue = 0;
	}

	rv = usb_control_msg(data->usb_dev,
			usb_rcvctrlpipe(data->usb_dev, 0),
			cmd,
			USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			wValue,
			data->ifnum,
			buffer, 0x01, data->timeout);
	if (rv < 0) {
		dev_err(dev, "simple usb_control_msg failed %d\n", rv);
		goto exit;
	} else if (rv != 1) {
		dev_warn(dev, "simple usb_control_msg returned %d\n", rv);
		rv = -EIO;
		goto exit;
	}

	if (buffer[0] != USBTMC_STATUS_SUCCESS) {
		dev_err(dev, "simple control status returned %x\n", buffer[0]);
		rv = -EIO;
		goto exit;
	}
	rv = 0;

 exit:
	kfree(buffer);
	return rv;
}

/*
 * Sends a TRIGGER Bulk-OUT command message
 * See the USBTMC-USB488 specification, Table 2.
 *
 * Also updates bTag_last_write.
 */
static int usbtmc488_ioctl_trigger(struct usbtmc_device_data *data)
{
	int retval;
	u8 *buffer;
	int actual;

	buffer = kzalloc(USBTMC_HEADER_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	buffer[0] = 128;
	buffer[1] = data->bTag;
	buffer[2] = ~data->bTag;

	retval = usb_bulk_msg(data->usb_dev,
			      usb_sndbulkpipe(data->usb_dev,
					      data->bulk_out),
			      buffer, USBTMC_HEADER_SIZE, &actual, data->timeout);

	/* Store bTag (in case we need to abort) */
	data->bTag_last_write = data->bTag;

	/* Increment bTag -- and increment again if zero */
	data->bTag++;
	if (!data->bTag)
		data->bTag++;

	kfree(buffer);
	if (retval < 0) {
		dev_err(&data->intf->dev, "%s returned %d\n",
			__func__, retval);
		return retval;
	}

	return retval;
}


/*
 * Sends a REQUEST_DEV_DEP_MSG_IN message on the Bulk-IN endpoint.
 * @transfer_size: number of bytes to request from the device.
 *
 * See the USBTMC specification, Table 4.
 *
 * Also updates bTag_last_write.
 */
static int send_request_dev_dep_msg_in(struct usbtmc_file_data *file_data,
				       size_t transfer_size)
{
	struct usbtmc_device_data *data = file_data->data;
	int retval;
	u8 *buffer;
	int actual;

	buffer = kmalloc(USBTMC_HEADER_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	/* Setup IO buffer for REQUEST_DEV_DEP_MSG_IN message
	 * Refer to class specs for details
	 */
	buffer[0] = 2;
	buffer[1] = data->bTag;
	buffer[2] = ~data->bTag;
	buffer[3] = 0; /* Reserved */
	buffer[4] = transfer_size >> 0;
	buffer[5] = transfer_size >> 8;
	buffer[6] = transfer_size >> 16;
	buffer[7] = transfer_size >> 24;
	buffer[8] = file_data->TermCharEnabled * 2;
	/* Use term character? */
	buffer[9] = file_data->TermChar;
	buffer[10] = 0; /* Reserved */
	buffer[11] = 0; /* Reserved */

	/* Send bulk URB */
	retval = usb_bulk_msg(data->usb_dev,
			      usb_sndbulkpipe(data->usb_dev,
					      data->bulk_out),
			      buffer, USBTMC_HEADER_SIZE, &actual, data->timeout);

	/* Store bTag (in case we need to abort) */
	data->bTag_last_write = data->bTag;

	/* Increment bTag -- and increment again if zero */
	data->bTag++;
	if (!data->bTag)
		data->bTag++;

	kfree(buffer);
	if (retval < 0)
		dev_err(&data->intf->dev, "%s returned %d\n",
			__func__, retval);

	return retval;
}

static ssize_t usbtmc_read(struct file *filp, char __user *buf,
			   size_t count, loff_t *f_pos)
{
	struct usbtmc_file_data *file_data;
	struct usbtmc_device_data *data;
	struct device *dev;
	u32 n_characters;
	u8 *buffer;
	int actual;
	size_t done;
	size_t remaining;
	int retval;
	size_t this_part;

	/* Get pointer to private data structure */
	file_data = filp->private_data;
	data = file_data->data;
	dev = &data->intf->dev;

	buffer = kmalloc(io_buffer_size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	mutex_lock(&data->io_mutex);
	if (data->zombie) {
		retval = -ENODEV;
		goto exit;
	}

	dev_dbg(dev, "usb_bulk_msg_in: count(%zu)\n", count);

	retval = send_request_dev_dep_msg_in(file_data, count);

	if (retval < 0) {
		if (data->auto_abort)
			usbtmc_ioctl_abort_bulk_out(data);
		goto exit;
	}

	/* Loop until we have fetched everything we requested */
	remaining = count;
	this_part = remaining;
	done = 0;

	while (remaining > 0) {
	        /* Send bulk URB */
		retval = usb_bulk_msg(data->usb_dev,
				      usb_rcvbulkpipe(data->usb_dev,
						      data->bulk_in),
				      buffer, io_buffer_size, &actual,
				      data->timeout);

	dev_dbg(dev, "%s: bulk_msg retval(%u), actual(%d)\n",
		__func__, retval, actual);

		/* Store bTag (in case we need to abort) */
		data->bTag_last_read = data->bTag;

		if (retval < 0) {
			dev_dbg(dev, "Unable to read data, error %d\n", retval);
			if (data->auto_abort)
				usbtmc_ioctl_abort_bulk_in(data);
			goto exit;
		}

		/* Parse header in first packet */
		if (done == 0) {
			/* Sanity checks for the header */
			if (actual < USBTMC_HEADER_SIZE) {
				dev_err(dev, "Device sent too small first packet: %u < %u\n", actual, USBTMC_HEADER_SIZE);
				if (data->auto_abort)
					usbtmc_ioctl_abort_bulk_in(data);
				goto exit;
			}

			if (buffer[0] != 2) {
				dev_err(dev, "Device sent reply with wrong MsgID: %u != 2\n", buffer[0]);
				if (data->auto_abort)
					usbtmc_ioctl_abort_bulk_in(data);
				goto exit;
			}

			if (buffer[1] != data->bTag_last_write) {
				dev_err(dev, "Device sent reply with wrong bTag: %u != %u\n", buffer[1], data->bTag_last_write);
				if (data->auto_abort)
					usbtmc_ioctl_abort_bulk_in(data);
				goto exit;
			}

			/* How many characters did the instrument send? */
			n_characters = buffer[4] +
				       (buffer[5] << 8) +
				       (buffer[6] << 16) +
				       (buffer[7] << 24);

			if (n_characters > this_part) {
				dev_err(dev, "Device wants to return more data than requested: %u > %zu\n", n_characters, count);
				if (data->auto_abort)
					usbtmc_ioctl_abort_bulk_in(data);
				goto exit;
			}

			/* Remove the USBTMC header */
			actual -= USBTMC_HEADER_SIZE;

			/* Check if the message is smaller than requested */
			if (remaining > n_characters)
				remaining = n_characters;
			/* Remove padding if it exists */
			if (actual > remaining)
				actual = remaining;

			dev_dbg(dev, "Bulk-IN header: N_characters(%u), bTransAttr(%u)\n", n_characters, buffer[8]);

			remaining -= actual;

			/* Terminate if end-of-message bit received from device */
			if ((buffer[8] & 0x01) && (actual >= n_characters))
				remaining = 0;

			dev_dbg(dev, "Bulk-IN header: remaining(%zu), buf(%p), buffer(%p) done(%zu)\n", remaining,buf,buffer,done);


			/* Copy buffer to user space */
			if (copy_to_user(buf + done, &buffer[USBTMC_HEADER_SIZE], actual)) {
				/* There must have been an addressing problem */
				retval = -EFAULT;
				goto exit;
			}
			done += actual;
		}
		else  {
			if (actual > remaining)
				actual = remaining;

			remaining -= actual;

			dev_dbg(dev, "Bulk-IN header cont: actual(%u), done(%zu), remaining(%zu), buf(%p), buffer(%p)\n", actual, done, remaining,buf,buffer);

			/* Copy buffer to user space */
			if (copy_to_user(buf + done, buffer, actual)) {
				/* There must have been an addressing problem */
				retval = -EFAULT;
				goto exit;
			}
			done += actual;
		}
	}

	/* Update file position value */
	*f_pos = *f_pos + done;
	retval = done;

exit:
	mutex_unlock(&data->io_mutex);
	kfree(buffer);
	return retval;
}

static ssize_t usbtmc_write(struct file *filp, const char __user *buf,
			    size_t count, loff_t *f_pos)
{
	struct usbtmc_file_data *file_data;
	struct usbtmc_device_data *data;
	u8 *buffer;
	int retval;
	int actual;
	unsigned long int n_bytes;
	int remaining;
	int done;
	int this_part;

	file_data = filp->private_data;
	data = file_data->data;

	buffer = kmalloc(io_buffer_size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	mutex_lock(&data->io_mutex);
	if (data->zombie) {
		retval = -ENODEV;
		goto exit;
	}

	remaining = count;
	done = 0;

	while (remaining > 0) {
		if (remaining > io_buffer_size - USBTMC_HEADER_SIZE) {
			this_part = io_buffer_size - USBTMC_HEADER_SIZE;
			buffer[8] = 0;
		} else {
			this_part = remaining;
			buffer[8] = data->eom_val;
		}

		/* Setup IO buffer for DEV_DEP_MSG_OUT message */
		buffer[0] = 1;
		buffer[1] = data->bTag;
		buffer[2] = ~data->bTag;
		buffer[3] = 0; /* Reserved */
		buffer[4] = this_part >> 0;
		buffer[5] = this_part >> 8;
		buffer[6] = this_part >> 16;
		buffer[7] = this_part >> 24;
		/* buffer[8] is set above... */
		buffer[9] = 0; /* Reserved */
		buffer[10] = 0; /* Reserved */
		buffer[11] = 0; /* Reserved */

		if (copy_from_user(&buffer[USBTMC_HEADER_SIZE], buf + done, this_part)) {
			retval = -EFAULT;
			goto exit;
		}

		n_bytes = roundup(USBTMC_HEADER_SIZE + this_part, 4);
		memset(buffer + USBTMC_HEADER_SIZE + this_part, 0, n_bytes - (USBTMC_HEADER_SIZE + this_part));

		do {
			retval = usb_bulk_msg(data->usb_dev,
					      usb_sndbulkpipe(data->usb_dev,
							      data->bulk_out),
					      buffer, n_bytes,
					      &actual, data->timeout);
			if (retval != 0)
				break;
			n_bytes -= actual;
		} while (n_bytes);

		data->bTag_last_write = data->bTag;
		data->bTag++;

		if (!data->bTag)
			data->bTag++;

		if (retval < 0) {
			dev_err(&data->intf->dev,
				"Unable to send data, error %d\n", retval);
			if (data->auto_abort)
				usbtmc_ioctl_abort_bulk_out(data);
			goto exit;
		}

		remaining -= this_part;
		done += this_part;
	}

	retval = count;
exit:
	mutex_unlock(&data->io_mutex);
	kfree(buffer);
	return retval;
}

static int usbtmc_ioctl_clear(struct usbtmc_device_data *data)
{
	struct usb_host_interface *current_setting;
	struct usb_endpoint_descriptor *desc;
	struct device *dev;
	u8 *buffer;
	int rv;
	int n;
	int actual = 0;
	int max_size;

	dev = &data->intf->dev;

	dev_dbg(dev, "Sending INITIATE_CLEAR request\n");

	buffer = kmalloc(io_buffer_size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	rv = usb_control_msg(data->usb_dev,
			     usb_rcvctrlpipe(data->usb_dev, 0),
			     USBTMC_REQUEST_INITIATE_CLEAR,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			     0, 0, buffer, 1, data->timeout);
	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}

	dev_dbg(dev, "INITIATE_CLEAR returned %x\n", buffer[0]);

	if (buffer[0] != USBTMC_STATUS_SUCCESS) {
		dev_err(dev, "INITIATE_CLEAR returned %x\n", buffer[0]);
		rv = -EPERM;
		goto exit;
	}

	max_size = 0;
	current_setting = data->intf->cur_altsetting;
	for (n = 0; n < current_setting->desc.bNumEndpoints; n++) {
		desc = &current_setting->endpoint[n].desc;
		if (desc->bEndpointAddress == data->bulk_in)
			max_size = usb_endpoint_maxp(desc);
	}

	if (max_size == 0) {
		dev_err(dev, "Couldn't get wMaxPacketSize\n");
		rv = -EPERM;
		goto exit;
	}

	dev_dbg(dev, "wMaxPacketSize is %d\n", max_size);

	n = 0;

usbtmc_clear_check_status:

	dev_dbg(dev, "Sending CHECK_CLEAR_STATUS request\n");

	rv = usb_control_msg(data->usb_dev,
			     usb_rcvctrlpipe(data->usb_dev, 0),
			     USBTMC_REQUEST_CHECK_CLEAR_STATUS,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			     0, 0, buffer, 2, data->timeout);
	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}

	dev_dbg(dev, "CHECK_CLEAR_STATUS returned %x\n", buffer[0]);

	if (buffer[0] == USBTMC_STATUS_SUCCESS)
		goto usbtmc_clear_bulk_out_halt;

	if (buffer[0] != USBTMC_STATUS_PENDING) {
		dev_err(dev, "CHECK_CLEAR_STATUS returned %x\n", buffer[0]);
		rv = -EPERM;
		goto exit;
	}

	if (buffer[1] == 1)
		do {
			dev_dbg(dev, "Reading from bulk in EP\n");

			rv = usb_bulk_msg(data->usb_dev,
					  usb_rcvbulkpipe(data->usb_dev,
							  data->bulk_in),
					  buffer, io_buffer_size,
					  &actual, data->timeout);
			n++;

			if (rv < 0) {
				dev_err(dev, "usb_control_msg returned %d\n",
					rv);
				goto exit;
			}
		} while ((actual == max_size) &&
			  (n < USBTMC_MAX_READS_TO_CLEAR_BULK_IN));

	if (actual == max_size) {
		dev_err(dev, "Couldn't clear device buffer within %d cycles\n",
			USBTMC_MAX_READS_TO_CLEAR_BULK_IN);
		rv = -EPERM;
		goto exit;
	}

	goto usbtmc_clear_check_status;

usbtmc_clear_bulk_out_halt:

	rv = usb_clear_halt(data->usb_dev,
			    usb_sndbulkpipe(data->usb_dev, data->bulk_out));
	if (rv < 0) {
		dev_err(dev, "usb_clear_halt returned %d\n", rv);
		goto exit;
	}
	rv = 0;

exit:
	kfree(buffer);
	return rv;
}

static int usbtmc_ioctl_clear_out_halt(struct usbtmc_device_data *data)
{
	int rv;

	rv = usb_clear_halt(data->usb_dev,
			    usb_sndbulkpipe(data->usb_dev, data->bulk_out));

	if (rv < 0) {
		dev_err(&data->usb_dev->dev, "usb_control_msg returned %d\n",
			rv);
		return rv;
	}
	return 0;
}

static int usbtmc_ioctl_clear_in_halt(struct usbtmc_device_data *data)
{
	int rv;

	rv = usb_clear_halt(data->usb_dev,
			    usb_rcvbulkpipe(data->usb_dev, data->bulk_in));

	if (rv < 0) {
		dev_err(&data->usb_dev->dev, "usb_control_msg returned %d\n",
			rv);
		return rv;
	}
	return 0;
}

static int get_capabilities(struct usbtmc_device_data *data)
{
	struct device *dev = &data->usb_dev->dev;
	char *buffer;
	int rv = 0;

	buffer = kmalloc(0x18, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	rv = usb_control_msg(data->usb_dev, usb_rcvctrlpipe(data->usb_dev, 0),
			     USBTMC_REQUEST_GET_CAPABILITIES,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			     0, 0, buffer, 0x18, data->timeout);
	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto err_out;
	}

	dev_dbg(dev, "GET_CAPABILITIES returned %x\n", buffer[0]);
	if (buffer[0] != USBTMC_STATUS_SUCCESS) {
		dev_err(dev, "GET_CAPABILITIES returned %x\n", buffer[0]);
		rv = -EPERM;
		goto err_out;
	}
	dev_dbg(dev, "Interface capabilities are %x\n", buffer[4]);
	dev_dbg(dev, "Device capabilities are %x\n", buffer[5]);
	dev_dbg(dev, "USB488 interface capabilities are %x\n", buffer[14]);
	dev_dbg(dev, "USB488 device capabilities are %x\n", buffer[15]);

	data->capabilities.interface_capabilities = buffer[4];
	data->capabilities.device_capabilities = buffer[5];
	data->capabilities.usb488_interface_capabilities = buffer[14];
	data->capabilities.usb488_device_capabilities = buffer[15];
	data->usb488_caps = (buffer[14] & 0x07) | ((buffer[15] & 0x0f) << 4);
	rv = 0;

err_out:
	kfree(buffer);
	return rv;
}

#define capability_attribute(name)					\
static ssize_t name##_show(struct device *dev,				\
			   struct device_attribute *attr, char *buf)	\
{									\
	struct usb_interface *intf = to_usb_interface(dev);		\
	struct usbtmc_device_data *data = usb_get_intfdata(intf);	\
									\
	return sprintf(buf, "%d\n", data->capabilities.name);		\
}									\
static DEVICE_ATTR_RO(name)

capability_attribute(interface_capabilities);
capability_attribute(device_capabilities);
capability_attribute(usb488_interface_capabilities);
capability_attribute(usb488_device_capabilities);

static struct attribute *capability_attrs[] = {
	&dev_attr_interface_capabilities.attr,
	&dev_attr_device_capabilities.attr,
	&dev_attr_usb488_interface_capabilities.attr,
	&dev_attr_usb488_device_capabilities.attr,
	NULL,
};

static const struct attribute_group capability_attr_grp = {
	.attrs = capability_attrs,
};

static ssize_t TermChar_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct usbtmc_device_data *data = usb_get_intfdata(intf);

	return sprintf(buf, "%c\n", data->TermChar);
}

static ssize_t TermChar_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct usbtmc_device_data *data = usb_get_intfdata(intf);

	if (count < 1)
		return -EINVAL;
	data->TermChar = buf[0];
	return count;
}
static DEVICE_ATTR_RW(TermChar);

#define data_attribute(name)						\
static ssize_t name##_show(struct device *dev,				\
			   struct device_attribute *attr, char *buf)	\
{									\
	struct usb_interface *intf = to_usb_interface(dev);		\
	struct usbtmc_device_data *data = usb_get_intfdata(intf);	\
									\
	return sprintf(buf, "%d\n", data->name);			\
}									\
static ssize_t name##_store(struct device *dev,				\
			    struct device_attribute *attr,		\
			    const char *buf, size_t count)		\
{									\
	struct usb_interface *intf = to_usb_interface(dev);		\
	struct usbtmc_device_data *data = usb_get_intfdata(intf);	\
	ssize_t result;							\
	unsigned int val;						\
									\
	result = sscanf(buf, "%u\n", &val);				\
	if (result != 1)						\
		result = -EINVAL;					\
	data->name = val;						\
	if (result < 0)							\
		return result;						\
	else								\
		return count;						\
}									\
static DEVICE_ATTR_RW(name)

data_attribute(TermCharEnabled);
data_attribute(auto_abort);

static struct attribute *data_attrs[] = {
	&dev_attr_TermChar.attr,
	&dev_attr_TermCharEnabled.attr,
	&dev_attr_auto_abort.attr,
	NULL,
};

static const struct attribute_group data_attr_grp = {
	.attrs = data_attrs,
};

/*
 * Flash activity indicator on device
 */
static int usbtmc_ioctl_indicator_pulse(struct usbtmc_device_data *data)
{
	struct device *dev;
	u8 *buffer;
	int rv;

	dev = &data->intf->dev;

	buffer = kmalloc(2, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	rv = usb_control_msg(data->usb_dev,
			     usb_rcvctrlpipe(data->usb_dev, 0),
			     USBTMC_REQUEST_INDICATOR_PULSE,
			     USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			     0, 0, buffer, 0x01, data->timeout);

	if (rv < 0) {
		dev_err(dev, "usb_control_msg returned %d\n", rv);
		goto exit;
	}

	dev_dbg(dev, "INDICATOR_PULSE returned %x\n", buffer[0]);

	if (buffer[0] != USBTMC_STATUS_SUCCESS) {
		dev_err(dev, "INDICATOR_PULSE returned %x\n", buffer[0]);
		rv = -EPERM;
		goto exit;
	}
	rv = 0;

exit:
	kfree(buffer);
	return rv;
}

static int usbtmc_ioctl_request(struct usbtmc_device_data *data, void __user *arg)
{
	struct device *dev = &data->intf->dev;
	struct usbtmc_ctrlrequest request;
	u8 *buffer = NULL;
	int rv;
	unsigned long res;

	res = copy_from_user(&request, arg, sizeof(struct usbtmc_ctrlrequest));
	if (res) {
		return -EFAULT;
	}

	buffer = kmalloc(request.req.wLength, GFP_KERNEL); // TODO: if wLength == 0
	if (!buffer)
		return -ENOMEM;

	if ((request.req.bRequestType & USB_DIR_IN) == 0) {
		res = copy_from_user(buffer, request.data, request.req.wLength);
		if (res) {
			rv = -EFAULT;
			goto exit;
		}
	}

	rv = usb_control_msg(data->usb_dev,
			usb_rcvctrlpipe(data->usb_dev, 0),
			request.req.bRequest,
			request.req.bRequestType,
			request.req.wValue,
			request.req.wIndex,
			buffer, request.req.wLength, data->timeout);

	if (rv < 0) {
		dev_err(dev, "generic usb_control_msg failed %d\n", rv);
		goto exit;
	}
	if ((request.req.bRequestType & USB_DIR_IN)) {
		if (rv > request.req.wLength ) {
			dev_warn(dev, "generic usb_control_msg returned too much data: %d\n", rv);
			rv = request.req.wLength;
		}

		res = copy_to_user(request.data, buffer, rv );
		if (res) {
			rv = -EFAULT;
		}
	}

 exit:
	kfree(buffer);
	return rv;
}

/*
 * Get the usb timeout value
 */
static int usbtmc_ioctl_get_timeout(struct usbtmc_device_data *data,
				void __user *arg)
{
	u32 timeout;

	timeout = data->timeout;

        if (copy_to_user(arg, &timeout, sizeof(timeout)))
                return -EFAULT;

	return 0;
}

/*
 * Set the usb timeout value
 */
static int usbtmc_ioctl_set_timeout(struct usbtmc_device_data *data,
				void __user *arg)
{
	u32 timeout;

	if (copy_from_user(&timeout, arg, sizeof(timeout)))
		return -EFAULT;

	if (timeout < USBTMC_MIN_TIMEOUT)
		return -EINVAL;

	data->timeout = timeout;

	return 0;
}

/*
 * enables/disables sending EOM on write
 */
static int usbtmc_ioctl_eom_enable(struct usbtmc_device_data *data,
				void __user *arg)
{
	__u8 eom_enable;

	if (copy_from_user(&eom_enable, arg, sizeof(eom_enable)))
		return -EFAULT;

	if (eom_enable > 1)
		return -EINVAL;

	data->eom_val = eom_enable;

	return 0;
}

/*
 * Configure TermChar and TermCharEnable
 */
static int usbtmc_ioctl_config_termc(struct usbtmc_device_data *data,
				void __user *arg)
{
	struct usbtmc_termchar termc;

	if (copy_from_user(&termc, arg, sizeof(termc)))
		return -EFAULT;

	if ((termc.term_char_enabled > 1) ||
		(termc.term_char_enabled &&
			!(data->capabilities.device_capabilities & 1)))
		return -EINVAL;

	data->TermChar = termc.term_char;
	data->TermCharEnabled = termc.term_char_enabled;

	return 0;
}

static long usbtmc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct usbtmc_file_data *file_data = file->private_data;
	struct usbtmc_device_data *data = file_data->data;
	int retval = -EBADRQC;

	mutex_lock(&data->io_mutex);
	if (data->zombie) {
		retval = -ENODEV;
		goto skip_io_on_zombie;
	}

	switch (cmd) {
	case USBTMC_IOCTL_CLEAR_OUT_HALT:
		retval = usbtmc_ioctl_clear_out_halt(data);
		break;

	case USBTMC_IOCTL_CLEAR_IN_HALT:
		retval = usbtmc_ioctl_clear_in_halt(data);
		break;

	case USBTMC_IOCTL_INDICATOR_PULSE:
		retval = usbtmc_ioctl_indicator_pulse(data);
		break;

	case USBTMC_IOCTL_CLEAR:
		retval = usbtmc_ioctl_clear(data);
		break;

	case USBTMC_IOCTL_ABORT_BULK_OUT:
		retval = usbtmc_ioctl_abort_bulk_out(data);
		break;

	case USBTMC_IOCTL_ABORT_BULK_IN:
		retval = usbtmc_ioctl_abort_bulk_in(data);
		break;

	case USBTMC_IOCTL_CTRL_REQUEST:
		retval = usbtmc_ioctl_request(data, (void __user *)arg);
		break;

	case USBTMC_IOCTL_GET_TIMEOUT:
		retval = usbtmc_ioctl_get_timeout(data, (void __user *)arg);
		break;

	case USBTMC_IOCTL_SET_TIMEOUT:
		retval = usbtmc_ioctl_set_timeout(data, (void __user *)arg);
		break;

	case USBTMC_IOCTL_EOM_ENABLE:
		retval = usbtmc_ioctl_eom_enable(data, (void __user *)arg);
		break;

	case USBTMC_IOCTL_CONFIG_TERMCHAR:
		retval = usbtmc_ioctl_config_termc(data, (void __user *)arg);
		break;

	case USBTMC488_IOCTL_GET_CAPS:
		retval = copy_to_user((void __user *)arg,
				&data->usb488_caps,
				sizeof(data->usb488_caps));
		if (retval)
			retval = -EFAULT;
		break;

	case USBTMC488_IOCTL_READ_STB:
		retval = usbtmc488_ioctl_read_stb(file_data,
						  (void __user *)arg);
		break;

	case USBTMC488_IOCTL_REN_CONTROL:
		retval = usbtmc488_ioctl_simple(data, (void __user *)arg,
						USBTMC488_REQUEST_REN_CONTROL);
		break;

	case USBTMC488_IOCTL_GOTO_LOCAL:
		retval = usbtmc488_ioctl_simple(data, (void __user *)arg,
						USBTMC488_REQUEST_GOTO_LOCAL);
		break;

	case USBTMC488_IOCTL_LOCAL_LOCKOUT:
		retval = usbtmc488_ioctl_simple(data, (void __user *)arg,
						USBTMC488_REQUEST_LOCAL_LOCKOUT);
		break;

	case USBTMC488_IOCTL_TRIGGER:
		retval = usbtmc488_ioctl_trigger(data);
		break;
	}

skip_io_on_zombie:
	mutex_unlock(&data->io_mutex);
	return retval;
}

static int usbtmc_fasync(int fd, struct file *file, int on)
{
	struct usbtmc_file_data *file_data = file->private_data;

	return fasync_helper(fd, file, on, &file_data->data->fasync);
}

static __poll_t usbtmc_poll(struct file *file, poll_table *wait)
{
	struct usbtmc_file_data *file_data = file->private_data;
	struct usbtmc_device_data *data = file_data->data;
	__poll_t mask;

	mutex_lock(&data->io_mutex);

	if (data->zombie) {
		mask = POLLHUP | POLLERR;
		goto no_poll;
	}

	poll_wait(file, &data->waitq, wait);

	mask = (atomic_read(&file_data->srq_asserted)) ? POLLPRI : 0;

no_poll:
	mutex_unlock(&data->io_mutex);
	return mask;
}

static const struct file_operations fops = {
	.owner		= THIS_MODULE,
	.read		= usbtmc_read,
	.write		= usbtmc_write,
	.open		= usbtmc_open,
	.release	= usbtmc_release,
	.unlocked_ioctl	= usbtmc_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= usbtmc_ioctl,
#endif
	.fasync         = usbtmc_fasync,
	.poll           = usbtmc_poll,
	.llseek		= default_llseek,
};

static struct usb_class_driver usbtmc_class = {
	.name =		"usbtmc%d",
	.fops =		&fops,
	.minor_base =	USBTMC_MINOR_BASE,
};

static void usbtmc_interrupt(struct urb *urb)
{
	struct usbtmc_device_data *data = urb->context;
	struct device *dev = &data->intf->dev;
	int status = urb->status;
	int rv;

	dev_dbg(&data->intf->dev, "int status: %d len %d\n",
		status, urb->actual_length);

	switch (status) {
	case 0: /* SUCCESS */
		/* check for valid STB notification */
		if (data->iin_buffer[0] > 0x81) {
			data->bNotify1 = data->iin_buffer[0];
			data->bNotify2 = data->iin_buffer[1];
			atomic_set(&data->iin_data_valid, 1);
			wake_up_interruptible(&data->waitq);
			goto exit;
		}
		/* check for SRQ notification */
		if (data->iin_buffer[0] == 0x81) {
			struct list_head *elem;

			if (data->fasync)
				kill_fasync(&data->fasync,
					SIGIO, POLL_PRI);

			spin_lock(&data->dev_lock);
			list_for_each(elem, &data->file_list) {
				struct usbtmc_file_data *file_data;

				file_data = list_entry(elem,
						struct usbtmc_file_data,
						file_elem);
				file_data->srq_byte = data->iin_buffer[1];
				atomic_set(&file_data->srq_asserted, 1);
			}
			spin_unlock(&data->dev_lock);

			dev_dbg(dev, "srq received bTag %x stb %x\n",
				(unsigned int)data->iin_buffer[0],
				(unsigned int)data->iin_buffer[1]);
			wake_up_interruptible_all(&data->waitq);
			goto exit;
		}
		dev_warn(dev, "invalid notification: %x\n",
			data->iin_buffer[0]);
		break;
	case -EOVERFLOW:
		dev_err(dev, "overflow with length %d, actual length is %d\n",
			data->iin_wMaxPacketSize, urb->actual_length);
		/* fall through */
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
	case -EILSEQ:
	case -ETIME:
	case -EPIPE:
		/* urb terminated, clean up */
		dev_dbg(dev, "urb terminated, status: %d\n", status);
		return;
	default:
		dev_err(dev, "unknown status received: %d\n", status);
	}
exit:
	rv = usb_submit_urb(urb, GFP_ATOMIC);
	if (rv)
		dev_err(dev, "usb_submit_urb failed: %d\n", rv);
}

static void usbtmc_free_int(struct usbtmc_device_data *data)
{
	if (!data->iin_ep_present || !data->iin_urb)
		return;
	usb_kill_urb(data->iin_urb);
	kfree(data->iin_buffer);
	usb_free_urb(data->iin_urb);
	data->iin_urb = NULL;
	kref_put(&data->kref, usbtmc_delete);
}

static int usbtmc_probe(struct usb_interface *intf,
			const struct usb_device_id *id)
{
	struct usbtmc_device_data *data;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	int n;
	int retcode;

	dev_dbg(&intf->dev, "%s called\n", __func__);

	pr_info("Experimental driver version %s loaded\n", USBTMC_VERSION);
	if (io_buffer_size < 512)
		io_buffer_size = 512;
	io_buffer_size = io_buffer_size - (io_buffer_size % 4);
	if (usb_timeout < USBTMC_MIN_TIMEOUT)
		usb_timeout = USBTMC_MIN_TIMEOUT;
	pr_info("Params: io_buffer_size = %d, usb_timeout = %d\n",
		io_buffer_size, usb_timeout);

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->intf = intf;
	data->id = id;
	data->usb_dev = usb_get_dev(interface_to_usbdev(intf));
	usb_set_intfdata(intf, data);
	kref_init(&data->kref);
	mutex_init(&data->io_mutex);
	init_waitqueue_head(&data->waitq);
	atomic_set(&data->iin_data_valid, 0);
	INIT_LIST_HEAD(&data->file_list);
	spin_lock_init(&data->dev_lock);

	data->zombie = 0;

	/* Initialize USBTMC bTag and other fields */
	data->bTag	= 1;
	data->TermCharEnabled = 0;
	data->TermChar = '\n';
	data->timeout  = usb_timeout;
	data->eom_val  = 1;
	/*  2 <= bTag <= 127   USBTMC-USB488 subclass specification 4.3.1 */
	data->iin_bTag = 2;

	/* USBTMC devices have only one setting, so use that */
	iface_desc = data->intf->cur_altsetting;
	data->ifnum = iface_desc->desc.bInterfaceNumber;

	/* Find bulk in endpoint */
	for (n = 0; n < iface_desc->desc.bNumEndpoints; n++) {
		endpoint = &iface_desc->endpoint[n].desc;

		if (usb_endpoint_is_bulk_in(endpoint)) {
			data->bulk_in = endpoint->bEndpointAddress;
			dev_dbg(&intf->dev, "Found bulk in endpoint at %u\n",
				data->bulk_in);
			break;
		}
	}

	/* Find bulk out endpoint */
	for (n = 0; n < iface_desc->desc.bNumEndpoints; n++) {
		endpoint = &iface_desc->endpoint[n].desc;

		if (usb_endpoint_is_bulk_out(endpoint)) {
			data->bulk_out = endpoint->bEndpointAddress;
			dev_dbg(&intf->dev, "Found Bulk out endpoint at %u\n",
				data->bulk_out);
			break;
		}
	}
	/* Find int endpoint */
	for (n = 0; n < iface_desc->desc.bNumEndpoints; n++) {
		endpoint = &iface_desc->endpoint[n].desc;

		if (usb_endpoint_is_int_in(endpoint)) {
			data->iin_ep_present = 1;
			data->iin_ep = endpoint->bEndpointAddress;
			data->iin_wMaxPacketSize = usb_endpoint_maxp(endpoint);
			data->iin_interval = endpoint->bInterval;
			dev_dbg(&intf->dev, "Found Int in endpoint at %u\n",
				data->iin_ep);
			break;
		}
	}

	retcode = get_capabilities(data);
	if (retcode)
		dev_err(&intf->dev, "can't read capabilities\n");
	else
		retcode = sysfs_create_group(&intf->dev.kobj,
					     &capability_attr_grp);

	if (data->iin_ep_present) {
		/* allocate int urb */
		data->iin_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!data->iin_urb) {
			retcode = -ENOMEM;
			goto error_register;
		}

		/* Protect interrupt in endpoint data until iin_urb is freed */
		kref_get(&data->kref);

		/* allocate buffer for interrupt in */
		data->iin_buffer = kmalloc(data->iin_wMaxPacketSize,
					GFP_KERNEL);
		if (!data->iin_buffer) {
			retcode = -ENOMEM;
			goto error_register;
		}

		/* fill interrupt urb */
		usb_fill_int_urb(data->iin_urb, data->usb_dev,
				usb_rcvintpipe(data->usb_dev, data->iin_ep),
				data->iin_buffer, data->iin_wMaxPacketSize,
				usbtmc_interrupt,
				data, data->iin_interval);

		retcode = usb_submit_urb(data->iin_urb, GFP_KERNEL);
		if (retcode) {
			dev_err(&intf->dev, "Failed to submit iin_urb\n");
			goto error_register;
		}
	}

	retcode = sysfs_create_group(&intf->dev.kobj, &data_attr_grp);

	retcode = usb_register_dev(intf, &usbtmc_class);
	if (retcode) {
		dev_err(&intf->dev, "Not able to get a minor (base %u, slice default): %d\n",
			USBTMC_MINOR_BASE,
			retcode);
		goto error_register;
	}
	dev_dbg(&intf->dev, "Using minor number %d\n", intf->minor);

	return 0;

error_register:
	sysfs_remove_group(&intf->dev.kobj, &capability_attr_grp);
	sysfs_remove_group(&intf->dev.kobj, &data_attr_grp);
	usbtmc_free_int(data);
	kref_put(&data->kref, usbtmc_delete);
	return retcode;
}

static void usbtmc_disconnect(struct usb_interface *intf)
{
	struct usbtmc_device_data *data = usb_get_intfdata(intf);

	dev_dbg(&intf->dev, "%s - called\n", __func__);

	usb_deregister_dev(intf, &usbtmc_class);
	sysfs_remove_group(&intf->dev.kobj, &capability_attr_grp);
	sysfs_remove_group(&intf->dev.kobj, &data_attr_grp);
	mutex_lock(&data->io_mutex);
	data->zombie = 1;
	wake_up_interruptible_all(&data->waitq);
	mutex_unlock(&data->io_mutex);
	usbtmc_free_int(data);
	kref_put(&data->kref, usbtmc_delete);

	pr_info("Experimental driver version %s unloaded", USBTMC_VERSION);
}

static int usbtmc_suspend(struct usb_interface *intf, pm_message_t message)
{
	/* this driver does not have pending URBs */
	return 0;
}

static int usbtmc_resume(struct usb_interface *intf)
{
	return 0;
}

static struct usb_driver usbtmc_driver = {
	.name		= "usbtmc",
	.id_table	= usbtmc_devices,
	.probe		= usbtmc_probe,
	.disconnect	= usbtmc_disconnect,
	.suspend	= usbtmc_suspend,
	.resume		= usbtmc_resume,
};

module_usb_driver(usbtmc_driver);

MODULE_LICENSE("GPL");
