/*
   unicorn-fops.c - V4L2 driver for unicorn

   Copyright (c) 2010 Aldebaran robotics
   joseph pinkasfeld joseph.pinkasfeld@gmail.com
   Ludovic SMAL <lsmal@aldebaran-robotics.com>
   Corentin Le Molgat <clemolgat@aldebaran-robotics.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
   */

#include "unicorn-fops.h"

#include "unicorn.h"
#include "unicorn-video.h"
#include "unicorn-mmap.h"
#include "unicorn-vbuff.h"

static int video_open(struct file *file)
{
  struct unicorn_dev *dev = video_drvdata(file);
  struct unicorn_fh *fh = dev->vidq[video_devdata(file)->index].fh;
  int i;
  file->private_data = fh;

  dprintk_video(1, dev->name, "open device %d ...\n", fh->channel);

  mutex_lock(&dev->mutex);
  fh->client_counter++;

  if (fh->client_counter > 1) {
    /* Buffer and sensor registers already initialized */
    mutex_unlock(&dev->mutex);
    dprintk_video(1, dev->name, "open device %d DONE\n", fh->channel);
    return 0;
  }

  dev->global_register->video[fh->channel].ctrl &= ~VIDEO_CONTROL_INPUT_SEL_MASK;
  dev->global_register->video[fh->channel].ctrl |= (fh->input) << VIDEO_CONTROL_INPUT_SEL_POS;
  dev->pixel_formats[fh->channel] = V4L2_PIX_FMT_YUYV;
  dev->fps_limit[fh->channel] = 0; // No fps limit set

  v4l2_prio_open(&dev->prio, &fh->prio);

  dprintk_video(1, dev->name,  "%s() device %d dma init...\n", __func__, fh->channel);
  videobuf_queue_dma_contig_init(&fh->vidq, &unicorn_video_qops,
      &dev->pci->dev, &dev->slock,
      V4L2_BUF_TYPE_VIDEO_CAPTURE,
      V4L2_FIELD_NONE,
      sizeof(struct unicorn_buffer), fh, NULL);
  dprintk_video(1, dev->name, "%s() device %d dma init DONE\n", __func__, fh->channel);

  // need to start streaming once dma is running
  dprintk_video(1, dev->name,  "%s() power up device %d...\n", __func__, fh->channel);
  for (i=0; i<max_subdev_per_video_bus; i++) {
    if(dev->sensor[fh->input][i] !=NULL)
      v4l2_subdev_call(dev->sensor[fh->input][i], core, s_power, 1);
  }

  mutex_unlock(&dev->mutex);

  dprintk_video(1, dev->name, "open device %d DONE\n", fh->channel);
  return 0;
}

static int video_release(struct file *file)
{
  struct unicorn_fh *fh = file->private_data;
  struct unicorn_dev *dev = fh->dev;
  int i;

  dprintk_video(1, dev->name,  "release device %d ...\n", fh->channel);

  mutex_lock(&dev->mutex);
  fh->client_counter--;

  if (fh->client_counter > 0) {
    /* A client is still using the device, don't free memory */
    mutex_unlock(&dev->mutex);
    dprintk_video(1, dev->name,  "release device %d DONE\n", fh->channel);
    return 0;
  }

  dev->global_register->video[fh->channel].ctrl &= ~VIDEO_CONTROL_ENABLE;
  dev->pcie_dma->dma[fh->channel].ctrl |= DMA_CONTROL_RESET;

  /* stop video capture */
  dprintk_video(1, dev->name,  "%s() stop video capture for device %d...\n", __func__, fh->channel);
  if (fh->vidq.streaming)
    videobuf_queue_cancel(&fh->vidq);

  if (fh->vidq.read_buf) {
    fh->vidq.ops->buf_release(&fh->vidq, fh->vidq.read_buf);
    kfree(fh->vidq.read_buf);
  }
  dprintk_video(1, dev->name, "mmap free videobuf queue of device %d ...\n", fh->channel);
  videobuf_mmap_free(&fh->vidq);
  dprintk_video(1, dev->name, "mmap free videobuf queue of device %d DONE\n", fh->channel);

  v4l2_prio_close(&dev->prio, fh->prio);
  file->private_data = NULL;

  dprintk_video(1, dev->name,  "%s() power down device %d...\n", __func__, fh->channel);
  for (i=0; i<max_subdev_per_video_bus; i++) {
    if(dev->sensor[fh->input][i] !=NULL)
      v4l2_subdev_call(dev->sensor[fh->input][i], core, s_power, 0);
  }

  mutex_unlock(&dev->mutex);

  dprintk_video(1, dev->name,  "release device %d DONE\n", fh->channel);
  return 0;
}

static ssize_t video_read(struct file *file, char __user * data, size_t count,
    loff_t * ppos)
{
  struct unicorn_fh *fh = file->private_data;
  struct unicorn_dev *dev = fh->dev;
  dprintk_video(1, dev->name, "%s()\n", __func__);

  switch (fh->type) {
    case V4L2_BUF_TYPE_VIDEO_CAPTURE:
      mutex_lock(&dev->mutex);
      if (fh->vidq.streaming) {
        mutex_unlock(&dev->mutex);
        return -EBUSY;
      }
      mutex_unlock(&dev->mutex);
      return videobuf_read_one(&fh->vidq, data, count, ppos,
          file->f_flags & O_NONBLOCK);

    default:
      BUG();
      return 0;
  }
}

static unsigned int video_poll(struct file *file,
    struct poll_table_struct *wait)
{
  struct unicorn_fh *fh = file->private_data;
  struct unicorn_buffer *buf;
  struct unicorn_dev *dev = fh->dev;
  dprintk_video(1, dev->name, "%s()\n", __func__);

  mutex_lock(&dev->mutex);
  if (fh->vidq.streaming){
    mutex_unlock(&dev->mutex);
    /* streaming capture */
    if (list_empty(&fh->vidq.stream))
      return POLLERR;
    buf = list_entry(fh->vidq.stream.next,
        struct unicorn_buffer, vb.stream);
  } else {
    mutex_unlock(&dev->mutex);
    /* read() capture */
    buf = (struct unicorn_buffer *)fh->vidq.read_buf;
    if (NULL == buf)
      return POLLERR;
  }
  poll_wait(file, &buf->vb.done, wait);
  if (buf->vb.state == VIDEOBUF_DONE || buf->vb.state == VIDEOBUF_ERROR) {

    return POLLIN | POLLRDNORM;
  }

  return 0;
}

int video_mmap(struct file *file, struct vm_area_struct *vma)
{
  int ret=0;

  struct unicorn_fh *fh = file->private_data;
  struct videobuf_queue *q = &fh->vidq;
  unsigned int index=0;
  unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

  // Search first available index to allocate
  for (index = 0; index < VIDEO_MAX_FRAME; ++index) {
    if(!q->bufs[index])
      continue;
    if (V4L2_MEMORY_MMAP != q->bufs[index]->memory)
      continue;
    if (q->bufs[index]->boff == offset)
      break;
  }
  if (VIDEO_MAX_FRAME == index) {
    dprintk_video(1, "unicorn-mmap", "Too much buffer mmapped for device %d\n", fh->channel);
    return -EINVAL;
  }

  dprintk_video(1, "unicorn-mmap", "mmap buffer %d for devide %d ...\n", index, fh->channel);
  ret = video_mmap_mapper_alloc(fh, index, vma);
  dprintk_video(1, "unicorn-mmap", "mmap buffer %d for device %d DONE\n", index, fh->channel);

  return ret;
}

const struct v4l2_file_operations video_fops = {
  .owner = THIS_MODULE,
  .open = video_open,     /* done */
  .release = video_release, /* done */
  .read = video_read,  /* done */
  .poll = video_poll,  /* done */
  .mmap = video_mmap, /* done */
  .unlocked_ioctl = video_ioctl2, /* not found */
};


