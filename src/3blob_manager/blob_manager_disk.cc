/*
 * Copyright (C) 2005-2015 Christoph Rupp (chris@crupp.de).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "0root/root.h"

#include <algorithm>
#include <vector>

#include "3rdparty/murmurhash3/MurmurHash3.h"

// Always verify that a file of level N does not include headers > N!
#include "1base/error.h"
#include "1base/dynamic_array.h"
#include "2compressor/compressor.h"
#include "2device/device.h"
#include "3blob_manager/blob_manager_disk.h"
#include "3page_manager/page_manager.h"
#include "4db/db_local.h"

#ifndef HAM_ROOT_H
#  error "root.h was not included"
#endif

using namespace hamsterdb;

uint64_t
DiskBlobManager::do_allocate(Context *context, ham_record_t *record,
                uint32_t flags)
{
  uint8_t *chunk_data[2];
  uint32_t chunk_size[2];
  uint32_t page_size = m_env->config().page_size_bytes;

  void *record_data = record->data;
  uint32_t record_size = record->size;
  uint32_t original_size = record->size;

  // compression enabled? then try to compress the data
  Compressor *compressor = db->get_record_compressor();
  if (compressor && !(flags & kDisableCompression)) {
    m_metric_before_compression += record_size;
    uint32_t len = compressor->compress((uint8_t *)record->data,
                        record->size);
    if (len < record->size) {
      record_data = (void *)compressor->get_output_data();
      record_size = len;
    }
    m_metric_after_compression += record_size;
  }

  PBlobHeader blob_header;
  uint32_t alloc_size = sizeof(PBlobHeader) + record_size;

  // first check if we can add another blob to the last used page
  Page *page = m_env->page_manager()->get_last_blob_page(context);

  PBlobPageHeader *header = 0;
  uint64_t address = 0;
  if (page) {
    header = PBlobPageHeader::from_page(page);
    // allocate space for the blob
    if (!alloc_from_freelist(header, alloc_size, &address))
      page = 0;
    else
      address += page->get_address();
  }

  if (!address) {
    // Allocate a new page. If the blob exceeds a page then allocate multiple
    // pages that are directly next to each other.
    uint32_t required_size = alloc_size + kPageOverhead;
    uint32_t num_pages = required_size / page_size;
    if (num_pages * page_size < required_size)
      num_pages++;

    // |page| now points to the first page that was allocated, and
    // the only one which has a header and a freelist
    page = m_env->page_manager()->alloc_multiple_blob_pages(context, num_pages);
    ham_assert(page->is_without_header() == false);

    // initialize the PBlobPageHeader
    header = PBlobPageHeader::from_page(page);
    header->initialize();
    header->set_num_pages(num_pages);
    header->set_free_bytes((num_pages * page_size) - kPageOverhead);

    // and move the remaining space to the freelist, unless we span multiple
    // pages (then the rest will be discarded)
    if (num_pages == 1
          && kPageOverhead + alloc_size > 0
          && header->get_free_bytes() - alloc_size > 0) {
      header->set_freelist_offset(0, kPageOverhead + alloc_size);
      header->set_freelist_size(0, header->get_free_bytes() - alloc_size);
    }

    // Pro: multi-page blobs store their CRC in the first freelist offset,
    // but only if partial writes are not used
    if (unlikely(num_pages > 1
        && (m_env->get_flags() & HAM_ENABLE_CRC32))) {
      uint32_t crc32 = 0;
      if (!(flags & HAM_PARTIAL))
        MurmurHash3_x86_32(record->data, record->size, 0, &crc32);
      header->set_freelist_offset(0, crc32);
    }

    address = page->get_address() + kPageOverhead;
    ham_assert(check_integrity(header));
  }

  // addjust "free bytes" counter
  ham_assert(header->get_free_bytes() >= alloc_size);
  header->set_free_bytes(header->get_free_bytes() - alloc_size);

  // store the page id if it still has space left
  if (header->get_free_bytes())
    m_env->page_manager()->set_last_blob_page(page);
  else
    m_env->page_manager()->set_last_blob_page(0);

  // initialize the blob header
  blob_header.set_alloc_size(alloc_size);
  blob_header.set_size(original_size);
  blob_header.set_self(address);
  blob_header.set_flags(original_size != record_size ? kIsCompressed : 0);

  // PARTIAL WRITE
  //
  // Are there gaps at the beginning? If yes, then we'll fill with zeros.
  // Partial updates are not allowed in combination with compression,
  // therefore we do not have to check any compression conditions if
  // HAM_PARTIAL is set.
  ByteArray zeroes;
  if ((flags & HAM_PARTIAL) && (record->partial_offset > 0)) {
    uint32_t gapsize = record->partial_offset;

    // first: write the header
    chunk_data[0] = (uint8_t *)&blob_header;
    chunk_size[0] = sizeof(blob_header);
    write_chunks(context, page, address, chunk_data, chunk_size, 1);

    address += sizeof(blob_header);

    // now fill the gap; if the gap is bigger than a pagesize we'll
    // split the gap into smaller chunks
    while (gapsize) {
      uint32_t size = gapsize >= page_size
                          ? page_size
                          : gapsize;
      chunk_data[0] = (uint8_t *)zeroes.resize(size, 0);
      chunk_size[0] = size;
      write_chunks(context, page, address, chunk_data, chunk_size, 1);
      gapsize -= size;
      address += size;
    }

    // now write the "real" data
    chunk_data[0] = (uint8_t *)record->data;
    chunk_size[0] = record->partial_size;

    write_chunks(context, page, address, chunk_data, chunk_size, 1);
    address += record->partial_size;
  }
  else {
    // not writing partially: write header and data, then we're done
    chunk_data[0] = (uint8_t *)&blob_header;
    chunk_size[0] = sizeof(blob_header);
    chunk_data[1] = (uint8_t *)record_data;
    chunk_size[1] = (flags & HAM_PARTIAL)
                        ? record->partial_size
                        : record_size;

    write_chunks(context, page, address, chunk_data, chunk_size, 2);
    address += chunk_size[0] + chunk_size[1];
  }

  // store the blobid; it will be returned to the caller
  uint64_t blobid = blob_header.get_self();

  // PARTIAL WRITES:
  //
  // if we have gaps at the end of the blob: just append more chunks to
  // fill these gaps. Since they can be pretty large we split them into
  // smaller chunks if necessary.
  if (flags & HAM_PARTIAL) {
    if (record->partial_offset + record->partial_size < record->size) {
      uint32_t gapsize = record->size
                      - (record->partial_offset + record->partial_size);

      // now fill the gap; if the gap is bigger than a pagesize we'll
      // split the gap into smaller chunks
      //
      // we split this loop in two - the outer loop will allocate the
      // memory buffer, thus saving some allocations
      while (gapsize) {
        uint32_t size = gapsize > page_size
                            ? page_size
                            : gapsize;
        chunk_data[0] = (uint8_t *)zeroes.resize(size, 0);
        chunk_size[0] = size;
        write_chunks(context, page, address, chunk_data, chunk_size, 1);
        gapsize -= size;
        address += size;
      }
    }
  }

  ham_assert(check_integrity(header));

  return (blobid);
}

void
DiskBlobManager::do_read(Context *context, uint64_t blobid,
                ham_record_t *record, uint32_t flags, ByteArray *arena)
{
  Page *page;

  // first step: read the blob header
  PBlobHeader *blob_header = (PBlobHeader *)read_chunk(context, 0, &page,
                  blobid, true);

  // sanity check
  if (blob_header->get_self() != blobid) {
    ham_log(("blob %lld not found", blobid));
    throw Exception(HAM_BLOB_NOT_FOUND);
  }

  uint32_t blobsize = (uint32_t)blob_header->get_size();
  record->size = blobsize;

  if (flags & HAM_PARTIAL) {
    if (record->partial_offset > blobsize) {
      ham_trace(("partial offset is greater than the total record size"));
      throw Exception(HAM_INV_PARAMETER);
    }
    if (record->partial_offset + record->partial_size > blobsize)
      record->partial_size = blobsize = blobsize - record->partial_offset;
    else
      blobsize = record->partial_size;
  }

  // empty blob?
  if (!blobsize) {
    record->data = 0;
    record->size = 0;
    return;
  }

  // if the blob is in memory-mapped storage (and the user does not require
  // a copy of the data): simply return a pointer
  if ((flags & HAM_FORCE_DEEP_COPY) == 0
        && m_env->device()->is_mapped(blobid, blobsize)
        && !(blob_header->get_flags() & kIsCompressed)
        && !(record->flags & HAM_RECORD_USER_ALLOC)) {
    record->data = read_chunk(context, page, 0,
                        blobid + sizeof(PBlobHeader) + (flags & HAM_PARTIAL
                                ? record->partial_offset
                                : 0), true);
  }
  // otherwise resize the blob buffer and copy the blob data into the buffer
  else {
    // read the blob data. if compression is enabled then
    // read into the Compressor's arena, otherwise read directly into the
    // caller's arena
    if (blob_header->get_flags() & kIsCompressed) {
      Compressor *compressor = db->get_record_compressor();
      ham_assert(compressor != 0);

      // read into temporary buffer; we reuse the compressor's memory arena
      // for this
      ByteArray *dest = compressor->get_arena();
      dest->resize(blob_header->get_alloc_size() - sizeof(PBlobHeader));

      read_chunk(page, 0, blobid + sizeof(PBlobHeader),
                    db, (uint8_t *)dest->get_ptr(),
                    blob_header->get_alloc_size() - sizeof(PBlobHeader), true);

      // now uncompress into the caller's memory arena
      if (record->flags & HAM_RECORD_USER_ALLOC) {
        compressor->decompress((uint8_t *)dest->get_ptr(),
                      blob_header->get_alloc_size() - sizeof(PBlobHeader),
                      blobsize, (uint8_t *)record->data);
      }
      else {
        arena->resize(blobsize);
        compressor->decompress((uint8_t *)dest->get_ptr(),
                      blob_header->get_alloc_size() - sizeof(PBlobHeader),
                      blobsize, arena);
        record->data = arena->get_ptr();
      }
    }
    // if the data is uncompressed then allocate storage and read
    // into the allocated buffer
    else {
      if (!(record->flags & HAM_RECORD_USER_ALLOC)) {
        arena->resize(blobsize);
        record->data = arena->get_ptr();
      }

      copy_chunk(context, page, 0,
                  blobid + sizeof(PBlobHeader) + (flags & HAM_PARTIAL
                          ? record->partial_offset
                          : 0),
                  (uint8_t *)record->data, blobsize, true);
    }
  }

  // Pro: multi-page blobs store their CRC in the first freelist offset,
  // but only if partial writes are not used
  PBlobPageHeader *header = PBlobPageHeader::from_page(page);
  if (unlikely(header->get_num_pages() > 1
        && (m_env->config().flags & HAM_ENABLE_CRC32))
        && !(flags & HAM_PARTIAL)) {
    uint32_t old_crc32 = header->get_freelist_offset(0);
    uint32_t new_crc32;
    MurmurHash3_x86_32(record->data, record->size, 0, &new_crc32);

    if (old_crc32 != new_crc32) {
      ham_trace(("crc32 mismatch in page %lu: 0x%lx != 0x%lx",
                      page->get_address(), old_crc32, new_crc32));
      throw Exception(HAM_INTEGRITY_VIOLATED);
    }
  }
}

uint64_t
DiskBlobManager::do_get_blob_size(Context *context, uint64_t blobid)
{
  // read the blob header
  PBlobHeader *blob_header = (PBlobHeader *)read_chunk(context, 0, 0, blobid,
                  true);

  if (blob_header->get_self() != blobid)
    throw Exception(HAM_BLOB_NOT_FOUND);

  return (blob_header->get_size());
}

uint64_t
DiskBlobManager::do_overwrite(Context *context, uint64_t old_blobid,
                ham_record_t *record, uint32_t flags)
{
  PBlobHeader *old_blob_header, new_blob_header;


  // This routine basically ignores compression. The likelyhood that a
  // compressed buffer has an identical size as the record that's overwritten,
  // is very small. In most cases this check will be false, and then
  // the record would be compressed again in do_allocate().
  //
  // As a consequence, the existing record is only overwritten if the
  // uncompressed record would fit in. Otherwise a new record is allocated,
  // and this one then is compressed.

  Page *page;

  uint32_t alloc_size = sizeof(PBlobHeader) + record->size;

  // first, read the blob header; if the new blob fits into the
  // old blob, we overwrite the old blob (and add the remaining
  // space to the freelist, if there is any)
  old_blob_header = (PBlobHeader *)read_chunk(context, 0, &page,
                    old_blobid, false);

  // sanity check
  ham_assert(old_blob_header->get_self() == old_blobid);
  if (old_blob_header->get_self() != old_blobid)
    throw Exception(HAM_BLOB_NOT_FOUND);

  // now compare the sizes; does the new data fit in the old allocated
  // space?
  if (alloc_size <= old_blob_header->get_alloc_size()) {
    uint8_t *chunk_data[2];
    uint32_t chunk_size[2];

    // setup the new blob header
    new_blob_header.set_self(old_blob_header->get_self());
    new_blob_header.set_size(record->size);
    new_blob_header.set_alloc_size(alloc_size);
    new_blob_header.set_flags(0); // disable compression, just in case...

    // PARTIAL WRITE
    //
    // if we have a gap at the beginning, then we have to write the
    // blob header and the blob data in two steps; otherwise we can
    // write both immediately
    if ((flags & HAM_PARTIAL) && (record->partial_offset)) {
      chunk_data[0] = (uint8_t *)&new_blob_header;
      chunk_size[0] = sizeof(new_blob_header);
      write_chunks(context, page, new_blob_header.get_self(),
                      chunk_data, chunk_size, 1);

      chunk_data[0] = (uint8_t *)record->data;
      chunk_size[0] = record->partial_size;
      write_chunks(context, page, new_blob_header.get_self()
                    + sizeof(new_blob_header) + record->partial_offset,
                      chunk_data, chunk_size, 1);
    }
    else {
      chunk_data[0] = (uint8_t *)&new_blob_header;
      chunk_size[0] = sizeof(new_blob_header);
      chunk_data[1] = (uint8_t *)record->data;
      chunk_size[1] = (flags & HAM_PARTIAL)
                          ? record->partial_size
                          : record->size;

      write_chunks(context, page, new_blob_header.get_self(),
                      chunk_data, chunk_size, 2);
    }

    PBlobPageHeader *header = PBlobPageHeader::from_page(page);

    // move remaining data to the freelist
    if (alloc_size < old_blob_header->get_alloc_size()) {
      PBlobPageHeader *header = PBlobPageHeader::from_page(page);
      header->set_free_bytes(header->get_free_bytes()
                  + (uint32_t)(old_blob_header->get_alloc_size() - alloc_size));
      add_to_freelist(header,
                  (uint32_t)(old_blobid + alloc_size) - page->get_address(),
                  (uint32_t)old_blob_header->get_alloc_size() - alloc_size);
    }

    // Pro: multi-page blobs store their CRC in the first freelist offset,
    // but only if partial writes are not used
    if (unlikely(header->get_num_pages() > 1
        && (m_env->get_flags() & HAM_ENABLE_CRC32))) {
      uint32_t crc32 = 0;
      if (!(flags & HAM_PARTIAL))
        MurmurHash3_x86_32(record->data, record->size, 0, &crc32);
      header->set_freelist_offset(0, crc32);
    }

    // the old rid is the new rid
    return (new_blob_header.get_self());
  }

  // if the new data is larger: allocate a fresh space for it
  // and discard the old; 'overwrite' has become (delete + insert) now.
  uint64_t new_blobid = allocate(context, record, flags);
  erase(context, old_blobid, 0, 0);

  return (new_blobid);
}

void
DiskBlobManager::do_erase(Context *context, uint64_t blobid, Page *page,
                uint32_t flags)
{
  // fetch the blob header
  PBlobHeader *blob_header = (PBlobHeader *)read_chunk(context, 0, &page,
                        blobid, false);

  // sanity check
  ham_verify(blob_header->get_self() == blobid);
  if (blob_header->get_self() != blobid)
    throw Exception(HAM_BLOB_NOT_FOUND);

  // update the "free bytes" counter in the blob page header
  PBlobPageHeader *header = PBlobPageHeader::from_page(page);
  header->set_free_bytes(header->get_free_bytes()
                  + blob_header->get_alloc_size());

  // if the page is now completely empty (all blobs were erased) then move
  // it to the freelist
  if (header->get_free_bytes() == (header->get_num_pages()
              * m_env->config().page_size_bytes) - kPageOverhead) {
    m_env->page_manager()->set_last_blob_page(0);
    m_env->page_manager()->del(context, page, header->get_num_pages());
    header->initialize();
    return;
  }

  // otherwise move the blob to the freelist
  add_to_freelist(header, (uint32_t)(blobid - page->get_address()),
                  (uint32_t)blob_header->get_alloc_size());
}

bool
DiskBlobManager::alloc_from_freelist(PBlobPageHeader *header, uint32_t size,
                uint64_t *poffset)
{
  ham_assert(check_integrity(header));

  // freelist is not used if this is a multi-page blob
  if (header->get_num_pages() > 1)
    return (false);

  uint32_t count = header->get_freelist_entries();

  for (uint32_t i = 0; i < count; i++) {
    // exact match
    if (header->get_freelist_size(i) == size) {
      *poffset = header->get_freelist_offset(i);
      header->set_freelist_offset(i, 0);
      header->set_freelist_size(i, 0);
      ham_assert(check_integrity(header));
      return (true);
    }
    // space in freelist is larger than what we need? return this space,
    // make sure the remaining gap stays in the freelist
    if (header->get_freelist_size(i) > size) {
      *poffset = header->get_freelist_offset(i);
      header->set_freelist_offset(i, (uint32_t)(*poffset + size));
      header->set_freelist_size(i, header->get_freelist_size(i) - size);
      ham_assert(check_integrity(header));
      return (true);
    }
  }

  // there was no gap large enough for the blob
  return (false);
}

void
DiskBlobManager::add_to_freelist(PBlobPageHeader *header,
                uint32_t offset, uint32_t size)
{
  ham_assert(check_integrity(header));

  // freelist is not used if this is a multi-page blob
  if (header->get_num_pages() > 1)
    return;

  uint32_t count = header->get_freelist_entries();

  // first try to collapse the blobs
  for (uint32_t i = 0; i < count; i++) {
    if (offset + size == header->get_freelist_offset(i)) {
      header->set_freelist_offset(i, offset);
      header->set_freelist_size(i, header->get_freelist_size(i) + size);
      ham_assert(check_integrity(header));
      return;
    }
    if (header->get_freelist_offset(i) + header->get_freelist_size(i)
            == offset) {
      header->set_freelist_size(i, header->get_freelist_size(i) + size);
      ham_assert(check_integrity(header));
      return;
    }
  }

  // otherwise store the blob in a new slot, if available
  uint32_t smallest = 0;
  for (uint32_t i = 0; i < count; i++) {
    // slot is empty
    if (header->get_freelist_size(i) == 0) {
      header->set_freelist_offset(i, offset);
      header->set_freelist_size(i, size);
      ham_assert(check_integrity(header));
      return;
    }
    // otherwise look for the smallest entry
    if (header->get_freelist_size(i) < header->get_freelist_size(smallest)) {
      smallest = i;
      continue;
    }
  }

  // overwrite the smallest entry?
  if (size > header->get_freelist_size(smallest)) {
    header->set_freelist_offset(smallest, offset);
    header->set_freelist_size(smallest, size);
  }

  ham_assert(check_integrity(header));
}

bool
DiskBlobManager::check_integrity(PBlobPageHeader *header) const
{
  ham_assert(header->get_num_pages() > 0);

  if (header->get_free_bytes() + kPageOverhead
        > (m_env->config().page_size_bytes * header->get_num_pages())) {
    ham_trace(("integrity violated: free bytes exceeds page boundary"));
    return (false);
  }

  // freelist is not used if this is a multi-page blob
  if (header->get_num_pages() > 1)
    return (true);

  uint32_t count = header->get_freelist_entries();
  uint32_t total_sizes = 0;
  typedef std::pair<uint32_t, uint32_t> Range;
  typedef std::vector<Range> RangeVec;
  RangeVec ranges;

  for (uint32_t i = 0; i < count - 1; i++) {
    if (header->get_freelist_size(i) == 0) {
      ham_assert(header->get_freelist_offset(i) == 0);
      continue;
    }
    total_sizes += header->get_freelist_size(i);
    ranges.push_back(std::make_pair(header->get_freelist_offset(i),
                header->get_freelist_size(i)));
  }

  // the sum of freelist chunks must not exceed total number of free bytes
  if (total_sizes > header->get_free_bytes()) {
    ham_trace(("integrity violated: total freelist slots exceed free bytes"));
    return (false);
  }

  std::sort(ranges.begin(), ranges.end());

  if (!ranges.empty()) {
    for (uint32_t i = 0; i < ranges.size() - 1; i++) {
      if (ranges[i].first + ranges[i].second
          > m_env->config().page_size_bytes * header->get_num_pages()) {
        ham_trace(("integrity violated: freelist slot %u/%u exceeds page",
                    ranges[i].first, ranges[i].second));
        return (false);
      }
      if (ranges[i].first + ranges[i].second > ranges[i + 1].first) {
        ham_trace(("integrity violated: freelist slot %u/%u overlaps with %lu",
                    ranges[i].first, ranges[i].second,
                    ranges[i + 1].first));
        throw Exception(HAM_INTEGRITY_VIOLATED);
      }
    }
  }

  return (true);
}

void
DiskBlobManager::write_chunks(Context *context, Page *page,
                uint64_t address, uint8_t **chunk_data, uint32_t *chunk_size,
                uint32_t chunks)
{
  uint32_t page_size = m_env->config().page_size_bytes;

  // for each chunk...
  for (uint32_t i = 0; i < chunks; i++) {
    uint32_t size = chunk_size[i];
    uint8_t *data = chunk_data[i];

    while (size) {
      // get the page-id from this chunk
      uint64_t pageid = address - (address % page_size);

      // is this the current page? if yes then continue working with this page,
      // otherwise fetch the page
      if (page && page->get_address() != pageid)
        page = 0;
      if (!page)
        page = m_env->page_manager()->fetch(context, pageid,
                        PageManager::kNoHeader);

      uint32_t write_start = (uint32_t)(address - page->get_address());
      uint32_t write_size = (uint32_t)(page_size - write_start);

      // now write the data
      if (write_size > size)
        write_size = size;
      memcpy(&page->get_raw_payload()[write_start], data, write_size);
      page->set_dirty(true);
      address += write_size;
      data += write_size;
      size -= write_size;
    }
  }
}

void
DiskBlobManager::copy_chunk(Context *context, Page *page, Page **ppage,
                uint64_t address, uint8_t *data, uint32_t size,
                bool fetch_read_only)
{
  uint32_t page_size = m_env->config().page_size_bytes;
  bool first_page = true;

  while (size) {
    // get the page-id from this chunk
    uint64_t pageid = address - (address % page_size);

    // is this the current page? if yes then continue working with this page,
    // otherwise fetch the page
    if (page && page->get_address() != pageid)
      page = 0;

    if (!page) {
      uint32_t flags = 0;
      if (fetch_read_only)
        flags |= PageManager::kReadOnly;
      if (!first_page)
        flags |= PageManager::kNoHeader;
      page = m_env->page_manager()->fetch(context, pageid, flags);
    }

    // now read the data from the page
    uint32_t read_start = (uint32_t)(address - page->get_address());
    uint32_t read_size = (uint32_t)(page_size - read_start);
    if (read_size > size)
      read_size = size;
    memcpy(data, &page->get_raw_payload()[read_start], read_size);
    address += read_size;
    data += read_size;
    size -= read_size;

    first_page = false;
  }

  if (ppage)
    *ppage = page;
}

uint8_t *
DiskBlobManager::read_chunk(Context *context, Page *page, Page **ppage,
                uint64_t address, bool fetch_read_only)
{
  // get the page-id from this chunk
  uint32_t page_size = m_env->config().page_size_bytes;
  uint64_t pageid = address - (address % page_size);

  // is this the current page? if yes then continue working with this page,
  // otherwise fetch the page
  if (page && page->get_address() != pageid)
    page = 0;

  if (!page) {
    uint32_t flags = 0;
    if (fetch_read_only)
      flags |= PageManager::kReadOnly;
    page = m_env->page_manager()->fetch(context, pageid, flags);
    if (ppage)
      *ppage = page;
  }

  uint32_t read_start = (uint32_t)(address - page->get_address());
  return (&page->get_raw_payload()[read_start]);
}
