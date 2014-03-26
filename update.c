/**
    Network Driver for Beckhoff CCAT communication controller
    Copyright (C) 2014  Beckhoff Automation GmbH
    Author: Patrick Bruenn <p.bruenn@beckhoff.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include "ccat.h"
#include "print.h"
#include "update.h"

#define CCAT_DATA_IN_4 0x038
#define CCAT_DATA_IN_N 0x7F0
#define CCAT_DATA_BLOCK_SIZE (size_t)((CCAT_DATA_IN_N - CCAT_DATA_IN_4)/8)
#define CCAT_WRITE_BLOCK_SIZE 128
#define CCAT_FLASH_SIZE (size_t)0xE0000

/**     FUNCTION_NAME            CMD,  CLOCKS          */
#define CCAT_BULK_ERASE          0xE3, 8
#define CCAT_GET_PROM_ID         0xD5, 40
#define CCAT_READ_FLASH          0xC0, 32
#define CCAT_READ_STATUS         0xA0, 16
#define CCAT_WRITE_ENABLE        0x60, 8
#define CCAT_WRITE_FLASH         0x40, 32

/* from http://graphics.stanford.edu/~seander/bithacks.html#ReverseByteWith32Bits */
#define SWAP_BITS(B) \
	((((B) * 0x0802LU & 0x22110LU) | ((B) * 0x8020LU & 0x88440LU)) * 0x10101LU >> 16)

struct update_buffer {
	struct ccat_update *update;
	char data[CCAT_FLASH_SIZE];
	size_t size;
};

static void ccat_wait_status_cleared(void __iomem *const ioaddr);
static int ccat_read_flash(void __iomem *const ioaddr, char __user *buf, uint32_t len, loff_t *off);
static void ccat_write_flash(const struct update_buffer *const buf);
static void ccat_update_cmd(void __iomem *const ioaddr, uint8_t cmd, uint16_t clocks);
static void ccat_update_destroy(struct kref *ref);

static inline void wait_until_busy_reset(void __iomem *const ioaddr)
{
	wmb();
	/* wait until busy flag was reset */
	while(ioread8(ioaddr + 1)) {
		schedule();
	}
}

static int ccat_update_open(struct inode *const i, struct file *const f)
{
	struct ccat_update *update = container_of(i->i_cdev, struct ccat_update, cdev);
	struct update_buffer *buf;
	kref_get(&update->refcount);
	if(atomic_read(&update->refcount.refcount) > 2) {
		kref_put(&update->refcount, ccat_update_destroy);
		return -EBUSY;
	}

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if(!buf) {
		kref_put(&update->refcount, ccat_update_destroy);
		return -ENOMEM;
	}

	buf->update = update;
	f->private_data = buf;
	return 0;
}

static int ccat_update_release(struct inode *const i, struct file *const f)
{
	const struct update_buffer *const buf = f->private_data;
	struct ccat_update *const update = buf->update;
	if(buf->size > 0 ) {
		ccat_update_cmd(update->ioaddr, CCAT_WRITE_ENABLE);
		ccat_update_cmd(update->ioaddr, CCAT_BULK_ERASE);
		ccat_wait_status_cleared(update->ioaddr);
		ccat_write_flash(buf);
	}
	kfree(f->private_data);
	kref_put(&update->refcount, ccat_update_destroy);
	return 0;
}

/**
 * ccat_update_read() - Read CCAT configuration data from flash
 * //TODO
 * A maximum of CCAT_DATA_BLOCK_SIZE bytes are read. To read the full
 * CCAT flash repeat calling this function until it returns 0, which
 * indicates EOF.
 */
static int ccat_update_read(struct file *const f, char __user *buf, size_t len, loff_t *off)
{
	struct update_buffer *update = f->private_data;
	if(!buf || !off) {
		return -EINVAL;
	}
	if(*off >= CCAT_FLASH_SIZE) {
		return 0;
	}
	if(*off + len >= CCAT_FLASH_SIZE) {
		len = CCAT_FLASH_SIZE - *off;
	}
	return ccat_read_flash(update->update->ioaddr, buf, len, off);
}

static int ccat_update_write(struct file *const f, const char __user *buf, size_t len, loff_t *off)
{
	struct update_buffer *const update = f->private_data;
	if(*off + len > sizeof(update->data))
		return 0;

	copy_from_user(update->data + *off, buf, len);
	*off += len;
	update->size = *off;
	return len;
}

static struct file_operations update_ops = {
	.owner = THIS_MODULE,
	.open = ccat_update_open,
	.release = ccat_update_release,
	.read = ccat_update_read,
	.write = ccat_update_write,
};

static inline void __ccat_update_cmd(void __iomem *const ioaddr, uint8_t cmd, uint16_t clocks)
{
	iowrite8((0xff00 & clocks) >> 8, ioaddr);
	iowrite8(0x00ff & clocks, ioaddr + 0x8);
	iowrite8(cmd, ioaddr + 0x10);
}

static inline void ccat_update_cmd(void __iomem *const ioaddr, uint8_t cmd, uint16_t clocks)
{
	__ccat_update_cmd(ioaddr, cmd, clocks);
	wmb();
	iowrite8(0xff, ioaddr + 0x7f8);
	wait_until_busy_reset(ioaddr);
}

static inline void ccat_update_cmd_addr(void __iomem *const ioaddr, uint8_t cmd, uint16_t clocks, uint32_t addr)
{
	const uint8_t addr_0 = SWAP_BITS(addr & 0xff);
	const uint8_t addr_1 = SWAP_BITS((addr & 0xff00) >> 8);
	const uint8_t addr_2 = SWAP_BITS((addr & 0xff0000) >> 16);
	__ccat_update_cmd(ioaddr, cmd, clocks);
	iowrite8(addr_2, ioaddr + 0x18);
	iowrite8(addr_1, ioaddr + 0x20);
	iowrite8(addr_0, ioaddr + 0x28);
	wmb();
	iowrite8(0xff, ioaddr + 0x7f8);
	wait_until_busy_reset(ioaddr);
}

/**
 * ccat_get_prom_id() - Read CCAT PROM ID
 * @update: CCAT Update function object
 */
static uint8_t ccat_get_prom_id(void __iomem *const ioaddr)
{
	ccat_update_cmd(ioaddr, CCAT_GET_PROM_ID);
	return ioread8(ioaddr + 0x38);
}

static uint8_t ccat_get_status(void __iomem *const ioaddr)
{
	ccat_update_cmd(ioaddr, CCAT_READ_STATUS);
	return ioread8(ioaddr + 0x20);
}

static void ccat_wait_status_cleared(void __iomem *const ioaddr)
{
	uint8_t status;
	do {
		status = ccat_get_status(ioaddr);
	} while (status & (1 << 7));
}

/**
 * ccat_read_flash() - Read bytes from CCAT flash
 * @update: CCAT Update function object
 * @len: number of bytes to read
 */
static int ccat_read_flash_block(void __iomem *const ioaddr, const uint32_t addr, const uint16_t len, char __user *const buf)
{
	uint16_t i;
	const uint16_t clocks = 8 * len;
	ccat_update_cmd_addr(ioaddr, CCAT_READ_FLASH + clocks, addr);
	for(i = 0; i < len; i++) {
		put_user(ioread8(ioaddr + CCAT_DATA_IN_4 + 8*i), buf + i);
	}
	return len;
}

static int ccat_read_flash(void __iomem *const ioaddr, char __user *buf, uint32_t len, loff_t* off)
{
	const loff_t start = *off;
	while(len > CCAT_DATA_BLOCK_SIZE) {
		*off += ccat_read_flash_block(ioaddr, *off, CCAT_DATA_BLOCK_SIZE, buf);
		buf += CCAT_DATA_BLOCK_SIZE;
		len -= CCAT_DATA_BLOCK_SIZE;
	}
	*off += ccat_read_flash_block(ioaddr, *off, len, buf);
	return *off - start;
}

#include <linux/delay.h>
static int ccat_write_flash_block(void __iomem *const ioaddr, const uint32_t addr, const uint16_t len, const char *const buf)
{
	uint16_t i;
	const uint16_t clocks = 8 * len;
	const uint8_t addr_0 = SWAP_BITS(addr & 0xff);
	const uint8_t addr_1 = SWAP_BITS((addr & 0xff00) >> 8);
	const uint8_t addr_2 = SWAP_BITS((addr & 0xff0000) >> 16);
	__ccat_update_cmd(ioaddr, CCAT_WRITE_FLASH + clocks);
	iowrite8(addr_2, ioaddr + 0x18);
	iowrite8(addr_1, ioaddr + 0x20);
	iowrite8(addr_0, ioaddr + 0x28);
	pr_info("write(%p, 0x%x, %u, %p)\n", ioaddr, addr, len, buf);
	pr_info("%02x %02x %02x %02x %02x %02x %02x %02x\n", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
	for(i = 0; i < len; i++) {
		iowrite8(buf[i], ioaddr + 0x030 + 8*i);
	}
	wmb();
	iowrite8(0xff, ioaddr + 0x7f8);
	wait_until_busy_reset(ioaddr);
	wmb();
	//msleep(100);
	ccat_wait_status_cleared(ioaddr);
	return len;
}

static void ccat_write_flash(const struct update_buffer *const update)
{
	const char *buf = update->data;
	uint32_t off = 0;
	size_t len = update->size;
	while(len > CCAT_WRITE_BLOCK_SIZE) {
		ccat_update_cmd(update->update->ioaddr, CCAT_WRITE_ENABLE);
		ccat_write_flash_block(update->update->ioaddr, off, (uint16_t)CCAT_WRITE_BLOCK_SIZE, buf);
		off += CCAT_WRITE_BLOCK_SIZE;
		buf += CCAT_WRITE_BLOCK_SIZE;
		len -= CCAT_WRITE_BLOCK_SIZE;
	}
	ccat_update_cmd(update->update->ioaddr, CCAT_WRITE_ENABLE);
	ccat_write_flash_block(update->update->ioaddr, off, (uint16_t)len, buf);
}

struct ccat_update *ccat_update_init(const struct ccat_device *const ccatdev,
				    void __iomem * const addr)
{
	struct ccat_update *const update = kzalloc(sizeof(*update), GFP_KERNEL);
	if(!update) {
		return NULL;
	}
	kref_init(&update->refcount);
	update->ioaddr = ccatdev->bar[0].ioaddr + ioread32(addr + 0x8);
	memcpy_fromio(&update->info, addr, sizeof(update->info));
	print_update_info(&update->info);
	pr_info("     PROM ID is:   0x%x\n", ccat_get_prom_id(update->ioaddr));

	if(0x00 != update->info.nRevision) {
		pr_warn("CCAT Update rev. %d not supported\n", update->info.nRevision);
		goto cleanup;
	}

	if(alloc_chrdev_region(&update->dev, 0, 1, DRV_NAME)) {
		pr_warn("alloc_chrdev_region() failed\n");
		goto cleanup;
	}

	update->class = class_create(THIS_MODULE, "ccat_update");
	if(NULL == update->class) {
		pr_warn("Create device class failed\n");
		goto cleanup;
	}

	if(NULL == device_create(update->class, NULL, update->dev, NULL, "ccat_update")) {
		pr_warn("device_create() failed\n");
		goto cleanup;
	}

	cdev_init(&update->cdev, &update_ops);
	update->cdev.owner = THIS_MODULE;
	update->cdev.ops = &update_ops;
	if(cdev_add(&update->cdev, update->dev, 1)) {
		pr_warn("add update device failed\n");
		goto cleanup;
	}
	return update;
cleanup:
	kref_put(&update->refcount, ccat_update_destroy);
	return NULL;
}

static void ccat_update_destroy(struct kref *ref)
{
	struct ccat_update *update = container_of(ref, struct ccat_update, refcount);
	cdev_del(&update->cdev);
	device_destroy(update->class, update->dev);
	class_destroy(update->class);
	unregister_chrdev_region(update->dev, 1);
	kfree(update);
	pr_info("%s(): done\n", __FUNCTION__);
}

void ccat_update_remove(struct ccat_update *update)
{
	kref_put(&update->refcount, ccat_update_destroy);
	pr_info("%s(): done\n", __FUNCTION__);
}