/*
 * Copyright (C) 2005-2016 Christoph Rupp (chris@crupp.de).
 * All Rights Reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING for License information.
 */

/*
 * Device-implementation for disk-based files. Exception safety is "strong"
 * for most operations, but currently it's possible that the Page is modified
 * if DiskDevice::read_page fails in the middle.
 *
 * @exception_safe: basic/strong
 * @thread_safe: no
 */

#ifndef UPS_DEVICE_DISK_H
#define UPS_DEVICE_DISK_H

#include "0root/root.h"

// Always verify that a file of level N does not include headers > N!
#include "1base/error.h"
#include "1base/dynamic_array.h"
#include "1mem/mem.h"
#include "1os/file.h"
#ifdef UPS_ENABLE_ENCRYPTION
#  include "2aes/aes.h"
#endif
#include "2device/device.h"
#include "2page/page.h"

#ifndef UPS_ROOT_H
#  error "root.h was not included"
#endif

namespace upscaledb {

/*
 * a File-based device
 */
class DiskDevice : public Device {
    struct State {
      // the database file
      File file;

      // pointer to the the mmapped data
      uint8_t *mmapptr;

      // the size of mmapptr as used in mmap
      uint64_t mapped_size;

      // the (cached) size of the file
      uint64_t file_size;

      // excess storage at the end of the file
      uint64_t excess_at_end;
    };

  public:
    DiskDevice(const EnvironmentConfiguration &config)
      : Device(config) {
      State state;
      state.mmapptr = 0;
      state.mapped_size = 0;
      state.file_size = 0;
      state.excess_at_end = 0;
      std::swap(m_state, state);
    }

    // Create a new device
    virtual void create() {
      ScopedSpinlock lock(m_mutex);

      File file;
      file.create(m_config.filename.c_str(), m_config.file_mode);
      file.set_posix_advice(m_config.posix_advice);
      m_state.file = file;
    }

    // opens an existing device
    //
    // tries to map the file; if it fails then continue with read/write 
    virtual void open() {
      bool read_only = (m_config.flags & UPS_READ_ONLY) != 0;

      ScopedSpinlock lock(m_mutex);

      State state = m_state;
      state.file.open(m_config.filename.c_str(), read_only);
      state.file.set_posix_advice(m_config.posix_advice);

      // the file size which backs the mapped ptr
      state.file_size = state.file.get_file_size();

      if (m_config.flags & UPS_DISABLE_MMAP) {
        std::swap(m_state, state);
        return;
      }

      // make sure we do not exceed the "real" size of the file, otherwise
      // we crash when accessing memory which exceeds the mapping (at least
      // on Win32)
      size_t granularity = File::get_granularity();
      if (state.file_size == 0 || state.file_size % granularity) {
        std::swap(m_state, state);
        return;
      }

      state.mapped_size = state.file_size;
      try {
        state.file.mmap(0, state.mapped_size, read_only, &state.mmapptr);
      }
      catch (Exception &ex) {
        ups_log(("mmap failed with error %d, falling back to read/write",
                    ex.code));
      }
      std::swap(m_state, state);
    }

    // returns true if the device is open
    virtual bool is_open() {
      ScopedSpinlock lock(m_mutex);
      return (m_state.file.is_open());
    }

    // closes the device
    virtual void close() {
      ScopedSpinlock lock(m_mutex);
      State state = m_state;
      if (state.mmapptr)
        state.file.munmap(state.mmapptr, state.mapped_size);
      state.file.close();

      std::swap(m_state, state);
    }

    // flushes the device
    virtual void flush() {
      ScopedSpinlock lock(m_mutex);
      m_state.file.flush();
    }

    // truncate/resize the device
    virtual void truncate(uint64_t new_file_size) {
      ScopedSpinlock lock(m_mutex);
      truncate_nolock(new_file_size);
    }

    // get the current file/storage size
    virtual uint64_t file_size() {
      ScopedSpinlock lock(m_mutex);
      ups_assert(m_state.file_size == m_state.file.get_file_size());
      return (m_state.file_size);
    }

    // seek to a position in a file
    virtual void seek(uint64_t offset, int whence) {
      ScopedSpinlock lock(m_mutex);
      m_state.file.seek(offset, whence);
    }

    // tell the position in a file
    virtual uint64_t tell() {
      ScopedSpinlock lock(m_mutex);
      return (m_state.file.tell());
    }

    // reads from the device; this function does NOT use mmap
    virtual void read(uint64_t offset, void *buffer, size_t len) {
      ScopedSpinlock lock(m_mutex);
      m_state.file.pread(offset, buffer, len);
#ifdef UPS_ENABLE_ENCRYPTION
      if (m_config.is_encryption_enabled) {
        AesCipher aes(m_config.encryption_key, offset);
        aes.decrypt((uint8_t *)buffer, (uint8_t *)buffer, len);
      }
#endif
    }

    // writes to the device; this function does not use mmap,
    // and is responsible for writing the data is run through the file
    // filters
    virtual void write(uint64_t offset, void *buffer, size_t len) {
      ScopedSpinlock lock(m_mutex);
#ifdef UPS_ENABLE_ENCRYPTION
      if (m_config.is_encryption_enabled) {
        // encryption disables direct I/O -> only full pages are allowed
        ups_assert(offset % len == 0);

        uint8_t *encryption_buffer = (uint8_t *)::alloca(len);
        AesCipher aes(m_config.encryption_key, offset);
        aes.encrypt((uint8_t *)buffer, encryption_buffer, len);
        m_state.file.pwrite(offset, encryption_buffer, len);
        return;
      }
#endif
      m_state.file.pwrite(offset, buffer, len);
    }

    // allocate storage from this device; this function
    // will *NOT* return mmapped memory
    virtual uint64_t alloc(size_t requested_length) {
      ScopedSpinlock lock(m_mutex);
      uint64_t address;

      if (m_state.excess_at_end >= requested_length) {
        address = m_state.file_size - m_state.excess_at_end;
        m_state.excess_at_end -= requested_length;
      }
      else {
        uint64_t excess = 0;
        bool allocate_excess = true;

        // If the file is large enough then allocate more space to avoid
        // frequent calls to ftruncate(); these calls cause bad performance
        // spikes.
        //
        // Disabled on win32 because truncating a mapped file is not allowed!
#ifdef WIN32
        if (m_state.mapped_size != 0)
          allocate_excess = false;
#endif

        if (allocate_excess) {
          if (m_state.file_size < requested_length * 100)
            excess = 0;
          else if (m_state.file_size < requested_length * 250)
            excess = requested_length * 100;
          else if (m_state.file_size < requested_length * 1000)
            excess = requested_length * 250;
          else
            excess = requested_length * 1000;
        }

        address = m_state.file_size;
        truncate_nolock(address + requested_length + excess);
        m_state.excess_at_end = excess;
      }
      return (address);
    }

    // reads a page from the device; this function CAN return a
	// pointer to mmapped memory
    virtual void read_page(Page *page, uint64_t address) {
      ScopedSpinlock lock(m_mutex);
      // if this page is in the mapped area: return a pointer into that area.
      // otherwise fall back to read/write.
      if (address < m_state.mapped_size && m_state.mmapptr != 0) {
        // ok, this page is mapped. If the Page object has a memory buffer
        // then free it; afterwards return a pointer into the mapped memory
        page->free_buffer();
        // the following line will not throw a C++ exception, but can
        // raise a signal. If that's the case then we don't catch it because
        // something is seriously wrong and proper recovery is not possible.
        page->assign_mapped_buffer(&m_state.mmapptr[address], address);
        return;
      }

      // this page is not in the mapped area; allocate a buffer
      if (page->get_data() == 0) {
        // note that |p| will not leak if file.pread() throws; |p| is stored
        // in the |page| object and will be cleaned up by the caller in
        // case of an exception.
        uint8_t *p = Memory::allocate<uint8_t>(m_config.page_size_bytes);
        page->assign_allocated_buffer(p, address);
      }

      m_state.file.pread(address, page->get_data(), m_config.page_size_bytes);
#ifdef UPS_ENABLE_ENCRYPTION
      if (m_config.is_encryption_enabled) {
        AesCipher aes(m_config.encryption_key, page->get_address());
        aes.decrypt((uint8_t *)page->get_data(),
        (uint8_t *)page->get_data(), m_config.page_size_bytes);
      }
#endif
    }

    // Allocates storage for a page from this device; this function
    // will *NOT* return mmapped memory
    virtual void alloc_page(Page *page) {
      uint64_t address = alloc(m_config.page_size_bytes);
      page->set_address(address);

      // allocate a memory buffer
      uint8_t *p = Memory::allocate<uint8_t>(m_config.page_size_bytes);
      page->assign_allocated_buffer(p, address);
    }

    // Frees a page on the device; plays counterpoint to |alloc_page|
    virtual void free_page(Page *page) {
      ScopedSpinlock lock(m_mutex);
      ups_assert(page->get_data() != 0);
      page->free_buffer();
    }

    // Returns true if the specified range is in mapped memory
    virtual bool is_mapped(uint64_t file_offset, size_t size) const {
      return (file_offset + size <= m_state.mapped_size);
    }

    // Removes unused space at the end of the file
    virtual void reclaim_space() {
      ScopedSpinlock lock(m_mutex);
      if (m_state.excess_at_end > 0) {
        truncate_nolock(m_state.file_size - m_state.excess_at_end);
        m_state.excess_at_end = 0;
      }
    }

    // Returns a pointer directly into mapped memory
    uint8_t *mapped_pointer(uint64_t address) const {
      return (&m_state.mmapptr[address]);
    }

  private:
    // truncate/resize the device, sans locking
    void truncate_nolock(uint64_t new_file_size) {
      if (new_file_size > m_config.file_size_limit_bytes)
        throw Exception(UPS_LIMITS_REACHED);
      m_state.file.truncate(new_file_size);
      m_state.file_size = new_file_size;
    }

    // For synchronizing access
    Spinlock m_mutex;

    State m_state;
};

} // namespace upscaledb

#endif /* UPS_DEVICE_DISK_H */
