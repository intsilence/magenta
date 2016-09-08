// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include <hexdump/hexdump.h>
#include <hw/inout.h>

#include "autolock.h"
#include "trace.h"
#include "virtio_priv.h"

#define LOCAL_TRACE 0

namespace virtio {

static mx_status_t virtio_device_release(mx_device_t* dev) {
    Device* d = Device::MXDeviceToObj(dev);

    LTRACEF("mx_device_t %p, virtio::Device %p\n", dev, d);

    assert(0);

    return NO_ERROR;
}

Device::Device(mx_driver_t* driver, mx_device_t* bus_device)
    : driver_(driver), bus_device_(bus_device) {
    LTRACE_ENTRY;

    mtx_init(&lock_, mtx_plain);

    // set up some common device ops
    device_ops_.release = &virtio_device_release;
}

Device::~Device() {
    if (pci_config_handle_)
        mx_handle_close(pci_config_handle_);

    LTRACE_ENTRY;

    // TODO: close pci protocol and other handles

    if (irq_handle_ > 0)
        mx_handle_close(irq_handle_);
    if (bar0_mmio_handle_ > 0)
        mx_handle_close(bar0_mmio_handle_);
    if (bar4_mmio_handle_ > 0)
        mx_handle_close(bar4_mmio_handle_);

    mtx_destroy(&lock_);
}

mx_status_t Device::Bind(pci_protocol_t* pci,
                         mx_handle_t pci_config_handle, const pci_config_t* pci_config) {
    LTRACE_ENTRY;

    AutoLock lock(lock_);

    // save off handles to things
    pci_ = pci;
    pci_config_handle_ = pci_config_handle;
    pci_config_ = pci_config;

    // detect if we're transitional or not
    if (pci_config_->device_id < 0x1040) {
        trans_ = true;
    }

    // claim the pci device
    mx_status_t r;
    r = pci->claim_device(bus_device_);
    if (r < 0)
        return r;

    // try to set up our IRQ mode
    if (pci->set_irq_mode(bus_device_, MX_PCIE_IRQ_MODE_MSI, 1)) {
        if (pci->set_irq_mode(bus_device_, MX_PCIE_IRQ_MODE_LEGACY, 1)) {
            LTRACEF("failed to set irq mode\n");
            return -1;
        } else {
            LTRACEF("using legacy irq mode\n");
        }
    }
    irq_handle_ = pci->map_interrupt(bus_device_, 0);
    if (irq_handle_ < 0) {
        TRACEF("failed to map irq\n");
        return -1;
    }

    LTRACEF("irq handle %u\n", irq_handle_);

    if (trans_) {
        LTRACEF("transitional\n");
        // transitional devices have a single PIO window at BAR0
        //
        // look at BAR0, which should be a PIO memory window
        bar0_pio_base_ = pci_config->base_addresses[0];
        LTRACEF("BAR0 address %#x\n", bar0_pio_base_);
        if ((bar0_pio_base_ & 0x1) == 0) {
            TRACEF("bar 0 does not appear to be PIO (address %#x, aborting\n", bar0_pio_base_);
            return -1;
        }

        bar0_pio_base_ &= ~1;
        if (bar0_pio_base_ > 0xffff) {
            bar0_pio_base_ = 0;

            // this may be a PIO mapped as mmio (non x86 host)
            // map in the mmio space
            // XXX this seems to be broken right now
            uint64_t sz;
            bar0_mmio_handle_ = pci->map_mmio(bus_device_, 0, MX_CACHE_POLICY_UNCACHED_DEVICE, (void**)&bar0_mmio_base_, &sz);
            if (bar0_mmio_handle_ < NO_ERROR) {
                TRACEF("cannot map io %d\n", bar0_mmio_handle_);
                return bar0_mmio_handle_;
            }

            LTRACEF("bar0_mmio_base_ %p, sz %#llx\n", bar0_mmio_base_, sz);
        } else {
            // this is probably PIO
            r = mx_mmap_device_io(get_root_resource(), bar0_pio_base_, bar0_size_);
            if (r != NO_ERROR) {
                TRACEF("failed to access PIO range %#x, length %#xw\n", bar0_pio_base_, bar0_size_);
                return r;
            }
        }
    } else {
        // non transitional
        LTRACEF("non transitional\n");

        // TODO: read this from the capabilities list
        //
        // from lspci:
        // Region 1: Memory at febd1000 (32-bit, non-prefetchable) [size=4K]
        // Region 4: Memory at fd000000 (64-bit, prefetchable) [size=8M]
        // Capabilities: [98] MSI-X: Enable+ Count=3 Masked-
        //     Vector table: BAR=1 offset=00000000
        //     PBA: BAR=1 offset=00000800
        // Capabilities: [84] Vendor Specific Information: VirtIO: <unknown>
        //     BAR=0 offset=00000000 size=00000000
        // Capabilities: [70] Vendor Specific Information: VirtIO: Notify
        //     BAR=4 offset=00003000 size=00400000 multiplier=00001000
        // Capabilities: [60] Vendor Specific Information: VirtIO: DeviceCfg
        //     BAR=4 offset=00002000 size=00001000
        // Capabilities: [50] Vendor Specific Information: VirtIO: ISR
        //     BAR=4 offset=00001000 size=00001000
        // Capabilities: [40] Vendor Specific Information: VirtIO: CommonCfg
        //     BAR=4 offset=00000000 size=00001000

        // map bar 4
        uint64_t sz;
        bar4_mmio_handle_ = pci->map_mmio(bus_device_, 4, MX_CACHE_POLICY_UNCACHED_DEVICE, (void**)&bar4_mmio_base_, &sz);
        if (bar4_mmio_handle_ < NO_ERROR) {
            TRACEF("cannot map io %d\n", bar4_mmio_handle_);
            return bar4_mmio_handle_;
        }
        LTRACEF("bar4_mmio_base_ %p, sz %#llx\n", bar4_mmio_base_, sz);

        // set up the mmio registers
        mmio_regs_.common_config = (volatile virtio_pci_common_cfg *)((uintptr_t)bar4_mmio_base_ + 0x0);
        mmio_regs_.isr_status = (volatile uint32_t *)((uintptr_t)bar4_mmio_base_ + 0x1000);
        mmio_regs_.device_config = (volatile void *)((uintptr_t)bar4_mmio_base_ + 0x2000);
        mmio_regs_.notify_base = (volatile uint16_t *)((uintptr_t)bar4_mmio_base_ + 0x3000);
        mmio_regs_.notify_mul = 0x1000;
    }

    // enable bus mastering
    if ((r = pci->enable_bus_master(bus_device_, true)) < 0) {
        TRACEF("cannot enable bus master %d\n", r);
        return -1;
    }

    LTRACE_EXIT;

    return NO_ERROR;
}

void Device::IrqWorker() {
    LTRACEF("started\n");
    for (;;) {
        mx_status_t r;
        if ((r = mx_pci_interrupt_wait(irq_handle_)) < 0) {
            printf("virtio: irq wait failed? %d\n", r);
            break;
        }

        uint32_t irq_status;
        if (trans_) {
            irq_status = inp((bar0_pio_base_ + VIRTIO_PCI_ISR_STATUS) & 0xffff);
        } else {
            irq_status = *mmio_regs_.isr_status;
        }

        LTRACEF("irq_status %#x\n", irq_status);

        if (irq_status == 0)
            continue;

        // grab the mutex for the duration of the irq handlers
        AutoLock lock(lock_);

        if (irq_status & 0x1) { /* used ring update */
            IrqRingUpdate();

#if 0
            /* cycle through all the active rings */
            for (uint r = 0; r < MAX_VIRTIO_RINGS; r++) {
                if ((dev->active_rings_bitmap & (1<<r)) == 0)
                    continue;

                struct vring *ring = &dev->ring[r];
                LTRACEF("ring %u: used flags 0x%hhx idx 0x%hhx last_used %u\n",
                        r, ring->used->flags, ring->used->idx, ring->last_used);

                uint cur_idx = ring->used->idx;
                for (uint i = ring->last_used; i != (cur_idx & ring->num_mask); i = (i + 1) & ring->num_mask) {
                    LTRACEF("looking at idx %u\n", i);

                    // process chain
                    struct vring_used_elem *used_elem = &ring->used->ring[i];
                    LTRACEF("id %u, len %u\n", used_elem->id, used_elem->len);

                    DEBUG_ASSERT(dev->irq_driver_callback);
                    ret |= dev->irq_driver_callback(dev, r, used_elem);

                    ring->last_used = (ring->last_used + 1) & ring->num_mask;
                }
            }
#endif
        }
        if (irq_status & 0x2) { /* config change */
            IrqConfigChange();
        }
    }
}

int Device::IrqThreadEntry(void* arg) {
    Device* d = static_cast<Device*>(arg);

    d->IrqWorker();

    return 0;
}

void Device::StartIrqThread() {
    thrd_create_with_name(&irq_thread_, IrqThreadEntry, this, "virtio-thread");
    thrd_detach(irq_thread_);
}

uint8_t Device::ReadConfigBar(uint16_t offset) {
    if (!trans_) {
        assert(0);
        return 0;
    }

    if (bar0_pio_base_) {
        uint16_t port = (bar0_pio_base_ + offset) & 0xffff;
        //LTRACEF("port %#x\n", port);
        return inp(port);
    } else {
        // XXX implement
        assert(0);
        return 0;
    }
}

void Device::WriteConfigBar(uint16_t offset, uint8_t val) {
    if (!trans_) {
        assert(0);
        return;
    }

    if (bar0_pio_base_) {
        uint16_t port = (bar0_pio_base_ + offset) & 0xffff;
        //LTRACEF("port %#x\n", port);
        outp(port, val);
    } else {
        // XXX implement
        assert(0);
    }
}

mx_status_t Device::CopyDeviceConfig(void* _buf, size_t len) {
    if (!trans_) {
        assert(0);
        return 0;
    }

    // XXX handle MSI vs noMSI
    size_t offset = VIRTIO_PCI_CONFIG_OFFSET_NOMSI;

    uint8_t* buf = (uint8_t*)_buf;
    for (size_t i = 0; i < len; i++) {
        if (bar0_pio_base_) {
            buf[i] = ReadConfigBar((offset + i) & 0xffff);
        } else {
            // XXX implement
            assert(0);
        }
    }

    return NO_ERROR;
}

void Device::SetRing(uint16_t index, uint16_t count, mx_paddr_t pa_desc, mx_paddr_t pa_avail, mx_paddr_t pa_used) {
    LTRACEF("index %u, count %u, pa_desc %#lx, pa_avail %#lx, pa_used %#lx\n", index, count, pa_desc, pa_avail, pa_used);

    if (trans_) {
        if (bar0_pio_base_) {
            outpw((bar0_pio_base_ + VIRTIO_PCI_QUEUE_SELECT) & 0xffff, index);
            outpw((bar0_pio_base_ + VIRTIO_PCI_QUEUE_SIZE) & 0xffff, count);
            outpd((bar0_pio_base_ + VIRTIO_PCI_QUEUE_PFN) & 0xffff, (uint32_t)(pa_desc / PAGE_SIZE));
        } else {
            // XXX implement
            assert(0);
        }
    } else {
        mmio_regs_.common_config->queue_select = index;
        mmio_regs_.common_config->queue_size = count;
        mmio_regs_.common_config->queue_desc = pa_desc;
        mmio_regs_.common_config->queue_avail = pa_avail;
        mmio_regs_.common_config->queue_used = pa_used;
        mmio_regs_.common_config->queue_enable = 1;
    }
}

void Device::RingKick(uint16_t ring_index) {
    LTRACEF("index %u\n", ring_index);
    if (trans_) {
        if (bar0_pio_base_) {
            outpw((bar0_pio_base_ + VIRTIO_PCI_QUEUE_NOTIFY) & 0xffff, ring_index);
        } else {
            // XXX implement
            assert(0);
        }
    } else {
        volatile uint16_t* notify = mmio_regs_.notify_base + ring_index * mmio_regs_.notify_mul / sizeof(uint16_t);
        LTRACEF("notify address %p\n", notify);
        *notify = ring_index;
    }
}

void Device::Reset() {
    if (trans_) {
        WriteConfigBar(VIRTIO_PCI_DEVICE_STATUS, 0);
    } else {
        mmio_regs_.common_config->device_status = 0;
    }
}

void Device::StatusAcknowledgeDriver() {
    if (trans_) {
        uint8_t val = ReadConfigBar(VIRTIO_PCI_DEVICE_STATUS);
        val |= VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
        WriteConfigBar(VIRTIO_PCI_DEVICE_STATUS, val);
    } else {
        mmio_regs_.common_config->device_status |= VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    }
}

void Device::StatusDriverOK() {
    if (trans_) {
        uint8_t val = ReadConfigBar(VIRTIO_PCI_DEVICE_STATUS);
        val |= VIRTIO_STATUS_DRIVER_OK;
        WriteConfigBar(VIRTIO_PCI_DEVICE_STATUS, val);
    } else {
        mmio_regs_.common_config->device_status |= VIRTIO_STATUS_DRIVER_OK;
    }
}

} // namespace virtio
