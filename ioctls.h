// This software is licensed under either the MIT or the GPLv2 licenses below.
//
// ----------------------------------------------------------------------------
// MIT license:
//
// Copyright 2023 Peralex Electronics(Pty) Ltd
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the “Software”), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
// ----------------------------------------------------------------------------
// GPLv2 license:
//
// Copyright(C) 2023 Peralex Electronics(Pty) Ltd
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; version 2 of the License.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 59 Temple Place, Suite 330, Boston, MA 02111 - 1307 USA

#ifndef __SURGE_DEV_IOCTLS_H__
#define __SURGE_DEV_IOCTLS_H__

#ifdef __SURGE_DEV_IOCTLS_DRIVER_DEFS__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#define SURGE_DEV_IOCTL_MAGIC 'P'
#define IOCTL_CMD_READ_REG32 0
#define IOCTL_CMD_WRITE_REG32 1
#define IOCTL_CMD_DMA_TRANSFER 2
#define IOCTL_CMD_READ_DRIVER_VERSION 3

struct cReg32Access
{
	unsigned m_uRegAddress;
	unsigned m_uValue;
};

struct cDmaAccess
{
	void *m_pvUserDataBuffer;
	unsigned m_uSize;
	unsigned m_uDMAReadDir;
	unsigned m_uTimeout_ms;
};

#define CTL_READ_REG32 _IOWR(SURGE_DEV_IOCTL_MAGIC, IOCTL_CMD_READ_REG32, struct cReg32Access *)
#define CTL_WRITE_REG32 _IOW(SURGE_DEV_IOCTL_MAGIC, IOCTL_CMD_WRITE_REG32, struct cReg32Access *)
#define CTL_DMA_TRANSFER _IOWR(SURGE_DEV_IOCTL_MAGIC, IOCTL_CMD_DMA_TRANSFER, struct cDmaAccess *)
#define CTL_READ_DRIVER_VERSION _IOR(SURGE_DEV_IOCTL_MAGIC, IOCTL_CMD_READ_DRIVER_VERSION, unsigned *)

#endif
