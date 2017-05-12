//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/format.h"

#include <string>
#include <inttypes.h>

#include "async/random_read_context.h"
#include "monitoring/perf_context_imp.h"
#include "monitoring/statistics.h"
#include "rocksdb/env.h"
#include "table/block.h"
#include "table/block_based_table_reader.h"
#include "table/persistent_cache_helper.h"
#include "util/coding.h"
#include "util/compression.h"
#include "util/crc32c.h"
#include "util/file_reader_writer.h"
#include "util/logging.h"
#include "util/stop_watch.h"
#include "util/string_util.h"
#include "util/xxhash.h"

namespace rocksdb {

extern const uint64_t kLegacyBlockBasedTableMagicNumber;
extern const uint64_t kBlockBasedTableMagicNumber;

#ifndef ROCKSDB_LITE
extern const uint64_t kLegacyPlainTableMagicNumber;
extern const uint64_t kPlainTableMagicNumber;
#else
// ROCKSDB_LITE doesn't have plain table
const uint64_t kLegacyPlainTableMagicNumber = 0;
const uint64_t kPlainTableMagicNumber = 0;
#endif

bool ShouldReportDetailedTime(Env* env, Statistics* stats) {
  return env != nullptr && stats != nullptr &&
         stats->stats_level_ > kExceptDetailedTimers;
}

void BlockHandle::EncodeTo(std::string* dst) const {
  // Sanity check that all fields have been set
  assert(offset_ != ~static_cast<uint64_t>(0));
  assert(size_ != ~static_cast<uint64_t>(0));
  PutVarint64Varint64(dst, offset_, size_);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  if (GetVarint64(input, &offset_) &&
      GetVarint64(input, &size_)) {
    return Status::OK();
  } else {
    // reset in case failure after partially decoding
    offset_ = 0;
    size_ = 0;
    return Status::Corruption("bad block handle");
  }
}

// Return a string that contains the copy of handle.
std::string BlockHandle::ToString(bool hex) const {
  std::string handle_str;
  EncodeTo(&handle_str);
  if (hex) {
    return Slice(handle_str).ToString(true);
  } else {
    return handle_str;
  }
}

const BlockHandle BlockHandle::kNullBlockHandle(0, 0);

namespace {
inline bool IsLegacyFooterFormat(uint64_t magic_number) {
  return magic_number == kLegacyBlockBasedTableMagicNumber ||
         magic_number == kLegacyPlainTableMagicNumber;
}
inline uint64_t UpconvertLegacyFooterFormat(uint64_t magic_number) {
  if (magic_number == kLegacyBlockBasedTableMagicNumber) {
    return kBlockBasedTableMagicNumber;
  }
  if (magic_number == kLegacyPlainTableMagicNumber) {
    return kPlainTableMagicNumber;
  }
  assert(false);
  return 0;
}
}  // namespace

// legacy footer format:
//    metaindex handle (varint64 offset, varint64 size)
//    index handle     (varint64 offset, varint64 size)
//    <padding> to make the total size 2 * BlockHandle::kMaxEncodedLength
//    table_magic_number (8 bytes)
// new footer format:
//    checksum (char, 1 byte)
//    metaindex handle (varint64 offset, varint64 size)
//    index handle     (varint64 offset, varint64 size)
//    <padding> to make the total size 2 * BlockHandle::kMaxEncodedLength + 1
//    footer version (4 bytes)
//    table_magic_number (8 bytes)
void Footer::EncodeTo(std::string* dst) const {
  assert(HasInitializedTableMagicNumber());
  if (IsLegacyFooterFormat(table_magic_number())) {
    // has to be default checksum with legacy footer
    assert(checksum_ == kCRC32c);
    const size_t original_size = dst->size();
    metaindex_handle_.EncodeTo(dst);
    index_handle_.EncodeTo(dst);
    dst->resize(original_size + 2 * BlockHandle::kMaxEncodedLength);  // Padding
    PutFixed32(dst, static_cast<uint32_t>(table_magic_number() & 0xffffffffu));
    PutFixed32(dst, static_cast<uint32_t>(table_magic_number() >> 32));
    assert(dst->size() == original_size + kVersion0EncodedLength);
  } else {
    const size_t original_size = dst->size();
    dst->push_back(static_cast<char>(checksum_));
    metaindex_handle_.EncodeTo(dst);
    index_handle_.EncodeTo(dst);
    dst->resize(original_size + kNewVersionsEncodedLength - 12);  // Padding
    PutFixed32(dst, version());
    PutFixed32(dst, static_cast<uint32_t>(table_magic_number() & 0xffffffffu));
    PutFixed32(dst, static_cast<uint32_t>(table_magic_number() >> 32));
    assert(dst->size() == original_size + kNewVersionsEncodedLength);
  }
}

Footer::Footer(uint64_t _table_magic_number, uint32_t _version)
  : version_(_version),
    checksum_(kCRC32c),
    table_magic_number_(_table_magic_number) {
  // This should be guaranteed by constructor callers
  assert(!IsLegacyFooterFormat(_table_magic_number) || version_ == 0);
}

Status Footer::DecodeFrom(Slice* input) {
  assert(!HasInitializedTableMagicNumber());
  assert(input != nullptr);
  assert(input->size() >= kMinEncodedLength);

  const char *magic_ptr =
    input->data() + input->size() - kMagicNumberLengthByte;
  const uint32_t magic_lo = DecodeFixed32(magic_ptr);
  const uint32_t magic_hi = DecodeFixed32(magic_ptr + 4);
  uint64_t magic = ((static_cast<uint64_t>(magic_hi) << 32) |
                    (static_cast<uint64_t>(magic_lo)));

  // We check for legacy formats here and silently upconvert them
  bool legacy = IsLegacyFooterFormat(magic);
  if (legacy) {
    magic = UpconvertLegacyFooterFormat(magic);
  }
  set_table_magic_number(magic);

  if (legacy) {
    // The size is already asserted to be at least kMinEncodedLength
    // at the beginning of the function
    input->remove_prefix(input->size() - kVersion0EncodedLength);
    version_ = 0 /* legacy */;
    checksum_ = kCRC32c;
  } else {
    version_ = DecodeFixed32(magic_ptr - 4);
    // Footer version 1 and higher will always occupy exactly this many bytes.
    // It consists of the checksum type, two block handles, padding,
    // a version number, and a magic number
    if (input->size() < kNewVersionsEncodedLength) {
      return Status::Corruption("input is too short to be an sstable");
    } else {
      input->remove_prefix(input->size() - kNewVersionsEncodedLength);
    }
    uint32_t chksum;
    if (!GetVarint32(input, &chksum)) {
      return Status::Corruption("bad checksum type");
    }
    checksum_ = static_cast<ChecksumType>(chksum);
  }

  Status result = metaindex_handle_.DecodeFrom(input);
  if (result.ok()) {
    result = index_handle_.DecodeFrom(input);
  }
  if (result.ok()) {
    // We skip over any leftover data (just padding for now) in "input"
    const char* end = magic_ptr + kMagicNumberLengthByte;
    *input = Slice(end, input->data() + input->size() - end);
  }
  return result;
}

std::string Footer::ToString() const {
  std::string result, handle_;
  result.reserve(1024);

  bool legacy = IsLegacyFooterFormat(table_magic_number_);
  if (legacy) {
    result.append("metaindex handle: " + metaindex_handle_.ToString() + "\n  ");
    result.append("index handle: " + index_handle_.ToString() + "\n  ");
    result.append("table_magic_number: " +
                  rocksdb::ToString(table_magic_number_) + "\n  ");
  } else {
    result.append("checksum: " + rocksdb::ToString(checksum_) + "\n  ");
    result.append("metaindex handle: " + metaindex_handle_.ToString() + "\n  ");
    result.append("index handle: " + index_handle_.ToString() + "\n  ");
    result.append("footer version: " + rocksdb::ToString(version_) + "\n  ");
    result.append("table_magic_number: " +
                  rocksdb::ToString(table_magic_number_) + "\n  ");
  }
  return result;
}

namespace async {

RandomReadContext::RandomReadContext(RandomAccessFileReader* file,
                                     uint64_t offset, size_t n,
                                     Slice* result, char* buf) {

  auto data = file->GetReadContextData();

  new (&ra_context_) RandomFileReadContext(file->file(), data.env_, data.stats_,
      data.file_read_hist_, data.hist_type_, file->use_direct_io(),
      file->file()->GetRequiredBufferAlignment());

  GetCtxRef().PrepareRead(offset, n, result, buf);
}


Status ReadFooterContext::OnReadFooterComplete(const Status& status, const Slice& slice) {

  OnRandomReadComplete(status, slice);

  if (!status.ok()) return status;

  // Check that we actually read the whole footer from the file. It may be
  // that size isn't correct.
  if (footer_input_.size() < Footer::kMinEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }

  Status s = footer_->DecodeFrom(&footer_input_);
  if (!s.ok()) {
    return s;
  }

  if (enforce_table_magic_number_ != 0 &&
      enforce_table_magic_number_ != footer_->table_magic_number()) {
    return Status::Corruption("Bad table magic number");
  }
  return Status::OK();
}

Status ReadFooterContext::OnIOCompletion(const Status& s, const Slice& slice) {
  std::unique_ptr<ReadFooterContext> self(this);
  Status status = OnReadFooterComplete(s, slice);
  // In these classes OnIOComplletion is only invoked when async
  // simply enforce this
  status.async(true);
  footer_cb_.Invoke(status);
  return status;
}

} // namespace async

Status ReadFooterFromFile(RandomAccessFileReader* file, uint64_t file_size,
                          Footer* footer, uint64_t enforce_table_magic_number) {

  return async::ReadFooterContext::ReadFooter(file, file_size, footer,
         enforce_table_magic_number);
}

namespace async {


/////////////////////////////////////////////////////////////////////////////////////////
// ReadBlockContext

Status ReadBlockContext::RequestBlockRead(const ReadBlockCallback& cb,
    RandomAccessFileReader* file, const Footer& footer,
    const ReadOptions& options, const BlockHandle& handle,
    Slice* contents, /* result of reading */ char* buf) {

  std::unique_ptr<ReadBlockContext> ctx(new ReadBlockContext(cb, file,
                                        footer.checksum(), options.verify_checksums,
                                        handle, contents, buf));

  auto iocb = ctx->GetIOCallback();
  Status s = ctx->RequestRead(iocb);

  if (s.IsIOPending()) {
    ctx.release();
    return s;
  }

  ctx->OnReadBlockComplete(s, *contents);

  return s;
}


Status ReadBlockContext::ReadBlock(RandomAccessFileReader * file,
                                   const Footer& footer,  const ReadOptions & options,
                                   const BlockHandle & handle, Slice * contents, char * buf) {

  ReadBlockContext ctx(ReadBlockCallback(), file, footer.checksum(),
                       options.verify_checksums, handle,
                       contents, buf);

  Status s = ctx.Read();

  s = ctx.OnReadBlockComplete(s, *contents);

  return s;
}

Status ReadBlockContext::OnReadBlockComplete(const Status& status, const Slice& raw_slice) {

  OnRandomReadComplete(status, raw_slice);

  PERF_TIMER_STOP(block_read_time);
  PERF_COUNTER_ADD(block_read_count, 1);
  PERF_COUNTER_ADD(block_read_byte, raw_slice.size());

  if (!status.ok()) {
    return status;
  }

  Status s(status);

  const Slice& slice = GetResult();
  auto n = GetRequestedSize();

  if (slice.size() != n) {
    return Status::Corruption("truncated block read");
  }

  // Bring back the original value
  n -= kBlockTrailerSize;

  // Check the crc of the type and the block contents
  const char* data = slice.data();  // Pointer to where Read put the data
  if (verify_checksums_) {
    PERF_TIMER_GUARD(block_checksum_time);
    uint32_t value = DecodeFixed32(data + n + 1);
    uint32_t actual = 0;
    switch (checksum_type_) {
    case kCRC32c:
      value = crc32c::Unmask(value);
      actual = crc32c::Value(data, n + 1);
      break;
    case kxxHash:
      actual = XXH32(data, static_cast<int>(n) + 1, 0);
      break;
    default:
      s = Status::Corruption("unknown checksum type");
    }
    if (s.ok() && actual != value) {
      s = Status::Corruption("block checksum mismatch");
    }
    if (!s.ok()) {
      return s;
    }
  }
  return s;
}

Status ReadBlockContext::OnIoCompletion(const Status& status, const Slice& raw_slice) {

  std::unique_ptr<ReadBlockContext> self(this);
  Status s = OnReadBlockComplete(status, raw_slice);
  // In these classes OnIOComplletion is only invoked when async
  // simply enforce this
  s.async(true);
  client_cb_.Invoke(s, GetResult());
  return s;
}

}

// Without anonymous namespace here, we fail the warning -Wmissing-prototypes
namespace {

// Read a block and check its CRC
// contents is the result of reading.
// According to the implementation of file->Read, contents may not point to buf
Status ReadBlock(RandomAccessFileReader* file, const Footer& footer,
                 const ReadOptions& options, const BlockHandle& handle,
                 Slice* contents, /* result of reading */ char* buf) {


  return async::ReadBlockContext::ReadBlock(file, footer, options, handle,
         contents, buf);
}

}  // namespace

namespace async {

/////////////////////////////////////////////////////////////////////////////////
/// ReadBlockContentsContext

Status ReadBlockContentsContext::CheckPersistentCache(bool&
    need_decompression) {

  size_t n = GetN();
  Status status;

  need_decompression = true;

  if (cache_options_->persistent_cache &&
      !cache_options_->persistent_cache->IsCompressed()) {
    status = PersistentCacheHelper::LookupUncompressedPage(*cache_options_,
             handle_, contents_);
    if (status.ok()) {
      // uncompressed page is found for the block handle
      need_decompression = false;
      return status;
    } else {
      // uncompressed page is not found
      if (ioptions_->info_log && !status.IsNotFound()) {
        assert(!status.ok());
        ROCKS_LOG_INFO(ioptions_->info_log,
                       "Error reading from persistent cache. %s",
                       status.ToString().c_str());
      }
    }
  }

  if (cache_options_->persistent_cache &&
      cache_options_->persistent_cache->IsCompressed()) {
    // lookup uncompressed cache mode p-cache
    status = PersistentCacheHelper::LookupRawPage(
               *cache_options_, handle_, &heap_buf_, n + kBlockTrailerSize);
  } else {
    status = Status::NotFound();
  }

  if (status.ok()) {
    // cache hit
    result_ = Slice(heap_buf_.get(), n);
  } else if (ioptions_->info_log && !status.IsNotFound()) {
    assert(!status.ok());
    ROCKS_LOG_INFO(ioptions_->info_log,
                   "Error reading from persistent cache. %s",
                   status.ToString().c_str());
  }

  return status;
}

Status ReadBlockContentsContext::RequestContentstRead(const
    ReadBlockContCallback& client_cb_,
    RandomAccessFileReader* file,
    const Footer& footer,
    const ReadOptions& read_options,
    const BlockHandle & handle,
    BlockContents* contents,
    const ImmutableCFOptions& ioptions,
    bool decompression_requested,
    const Slice& compression_dict,
    const PersistentCacheOptions& cache_options) {


  std::unique_ptr<ReadBlockContentsContext> context(new ReadBlockContentsContext(
        client_cb_, footer,
        read_options, handle, contents, ioptions, decompression_requested,
        compression_dict, cache_options));

  bool need_decompression = false;
  Status status = context->CheckPersistentCache(need_decompression);

  if (status.ok()) {
    if (need_decompression) {
      return context->OnReadBlockContentsComplete(status, context->result_);
    }
    return status;
  }

  // Proceed with reading the block from disk
  context->ConstructReadBlockContext(file);

  auto iocb = context->GetIOCallback();
  status = context->RequestRead(iocb);

  if (status.IsIOPending()) {
    context.release();
    return status;
  }

  return context->OnReadBlockContentsComplete(status, context->result_);
}

Status ReadBlockContentsContext::ReadContents(RandomAccessFileReader* file,
    const Footer& footer,
    const ReadOptions& read_options,
    const BlockHandle& handle,
    BlockContents* contents,
    const ImmutableCFOptions& ioptions,
    bool decompression_requested,
    const Slice & compression_dict,
    const PersistentCacheOptions & cache_options) {

  ReadBlockContentsContext context(ReadBlockContCallback(), footer, read_options,
                                   handle, contents, ioptions, decompression_requested,
                                   compression_dict, cache_options);

  bool need_decompression = false;
  Status status = context.CheckPersistentCache(need_decompression);

  if (status.ok()) {
    if (need_decompression) {
      return context.OnReadBlockContentsComplete(status, context.result_);
    }
    return status;
  }

  // Proceed with reading the block from disk
  context.ConstructReadBlockContext(file);

  status = context.Read();

  return context.OnReadBlockContentsComplete(status, context.result_);
}

Status ReadBlockContentsContext::OnReadBlockContentsComplete(const Status& s,
    const Slice& slice) {

  Status status(s);

  if (is_read_block_) {
    status = GetReadBlock()->OnReadBlockComplete(s, slice);
  }

  if (!status.ok()) {
    return status;
  }

  size_t n = GetN();

  // We only allocate heap_buf_ if necessary
  char* used_buf = (heap_buf_) ? heap_buf_.get() : inclass_buf_;
  assert(used_buf != nullptr);

  if (read_options_->fill_cache &&
      cache_options_->persistent_cache &&
      cache_options_->persistent_cache->IsCompressed()) {
    // insert to raw cache
    PersistentCacheHelper::InsertRawPage(*cache_options_, handle_, used_buf,
                                         n + kBlockTrailerSize);
  }

  PERF_TIMER_GUARD(block_decompress_time);

  rocksdb::CompressionType compression_type =
    static_cast<rocksdb::CompressionType>(slice.data()[n]);

  if (decompression_requested_ && compression_type != kNoCompression) {
    // compressed page, uncompress, update cache
    status = UncompressBlockContents(slice.data(), n, contents_,
                                     footer_->version(), compression_dict_,
                                     *ioptions_);
  } else if (slice.data() != used_buf) {
    // the slice content is not the buffer provided
    *contents_ = BlockContents(Slice(slice.data(), n), false, compression_type);
  } else {
    // page is uncompressed, the buffer either stack or heap provided
    if (used_buf == inclass_buf_) {
      heap_buf_.reset(new char[n]);
      memcpy(heap_buf_.get(), inclass_buf_, n);
    }
    *contents_ = BlockContents(std::move(heap_buf_), n, true, compression_type);
  }

  if (status.ok() && read_options_->fill_cache &&
      cache_options_->persistent_cache &&
      !cache_options_->persistent_cache->IsCompressed()) {
    // insert to uncompressed cache
    PersistentCacheHelper::InsertUncompressedPage(*cache_options_, handle_,
        *contents_);
  }

  return status;
}

Status ReadBlockContentsContext::OnIoCompletion(const Status& status,
    const Slice& slice) {

  std::unique_ptr<ReadBlockContentsContext> self(this);
  Status s = OnReadBlockContentsComplete(status, slice);
  // In these classes OnIOComplletion is only invoked when async
  // simply enforce this
  s.async(true);
  client_cb_.Invoke(s);
  return s;
}

}

Status ReadBlockContents(RandomAccessFileReader* file, const Footer& footer,
                         const ReadOptions& read_options,
                         const BlockHandle& handle, BlockContents* contents,
                         const ImmutableCFOptions &ioptions,
                         bool decompression_requested,
                         const Slice& compression_dict,
                         const PersistentCacheOptions& cache_options) {

  return async::ReadBlockContentsContext::ReadContents(file, footer,
         read_options,
         handle, contents, ioptions, decompression_requested, compression_dict,
         cache_options);
}

Status UncompressBlockContentsForCompressionType(
  const char* data, size_t n, BlockContents* contents,
  uint32_t format_version, const Slice& compression_dict,
  CompressionType compression_type, const ImmutableCFOptions &ioptions) {
  std::unique_ptr<char[]> ubuf;

  assert(compression_type != kNoCompression && "Invalid compression type");

  StopWatchNano timer(ioptions.env,
                      ShouldReportDetailedTime(ioptions.env, ioptions.statistics));
  int decompress_size = 0;
  switch (compression_type) {
  case kSnappyCompression: {
    size_t ulength = 0;
    static char snappy_corrupt_msg[] =
      "Snappy not supported or corrupted Snappy compressed block contents";
    if (!Snappy_GetUncompressedLength(data, n, &ulength)) {
      return Status::Corruption(snappy_corrupt_msg);
    }
    ubuf.reset(new char[ulength]);
    if (!Snappy_Uncompress(data, n, ubuf.get())) {
      return Status::Corruption(snappy_corrupt_msg);
    }
    *contents = BlockContents(std::move(ubuf), ulength, true, kNoCompression);
    break;
  }
  case kZlibCompression:
    ubuf.reset(Zlib_Uncompress(
                 data, n, &decompress_size,
                 GetCompressFormatForVersion(kZlibCompression, format_version),
                 compression_dict));
    if (!ubuf) {
      static char zlib_corrupt_msg[] =
        "Zlib not supported or corrupted Zlib compressed block contents";
      return Status::Corruption(zlib_corrupt_msg);
    }
    *contents =
      BlockContents(std::move(ubuf), decompress_size, true, kNoCompression);
    break;
  case kBZip2Compression:
    ubuf.reset(BZip2_Uncompress(
                 data, n, &decompress_size,
                 GetCompressFormatForVersion(kBZip2Compression, format_version)));
    if (!ubuf) {
      static char bzip2_corrupt_msg[] =
        "Bzip2 not supported or corrupted Bzip2 compressed block contents";
      return Status::Corruption(bzip2_corrupt_msg);
    }
    *contents =
      BlockContents(std::move(ubuf), decompress_size, true, kNoCompression);
    break;
  case kLZ4Compression:
    ubuf.reset(LZ4_Uncompress(
                 data, n, &decompress_size,
                 GetCompressFormatForVersion(kLZ4Compression, format_version),
                 compression_dict));
    if (!ubuf) {
      static char lz4_corrupt_msg[] =
        "LZ4 not supported or corrupted LZ4 compressed block contents";
      return Status::Corruption(lz4_corrupt_msg);
    }
    *contents =
      BlockContents(std::move(ubuf), decompress_size, true, kNoCompression);
    break;
  case kLZ4HCCompression:
    ubuf.reset(LZ4_Uncompress(
                 data, n, &decompress_size,
                 GetCompressFormatForVersion(kLZ4HCCompression, format_version),
                 compression_dict));
    if (!ubuf) {
      static char lz4hc_corrupt_msg[] =
        "LZ4HC not supported or corrupted LZ4HC compressed block contents";
      return Status::Corruption(lz4hc_corrupt_msg);
    }
    *contents =
      BlockContents(std::move(ubuf), decompress_size, true, kNoCompression);
    break;
  case kXpressCompression:
    ubuf.reset(XPRESS_Uncompress(data, n, &decompress_size));
    if (!ubuf) {
      static char xpress_corrupt_msg[] =
        "XPRESS not supported or corrupted XPRESS compressed block contents";
      return Status::Corruption(xpress_corrupt_msg);
    }
    *contents =
      BlockContents(std::move(ubuf), decompress_size, true, kNoCompression);
    break;
  case kZSTD:
  case kZSTDNotFinalCompression:
    ubuf.reset(ZSTD_Uncompress(data, n, &decompress_size, compression_dict));
    if (!ubuf) {
      static char zstd_corrupt_msg[] =
        "ZSTD not supported or corrupted ZSTD compressed block contents";
      return Status::Corruption(zstd_corrupt_msg);
    }
    *contents =
      BlockContents(std::move(ubuf), decompress_size, true, kNoCompression);
    break;
  default:
    return Status::Corruption("bad block type");
  }

  if(ShouldReportDetailedTime(ioptions.env, ioptions.statistics)) {
    MeasureTime(ioptions.statistics, DECOMPRESSION_TIMES_NANOS,
                timer.ElapsedNanos());
    MeasureTime(ioptions.statistics, BYTES_DECOMPRESSED, contents->data.size());
    RecordTick(ioptions.statistics, NUMBER_BLOCK_DECOMPRESSED);
  }

  return Status::OK();
}

//
// The 'data' points to the raw block contents that was read in from file.
// This method allocates a new heap buffer and the raw block
// contents are uncompresed into this buffer. This
// buffer is returned via 'result' and it is upto the caller to
// free this buffer.
// format_version is the block format as defined in include/rocksdb/table.h
Status UncompressBlockContents(const char* data, size_t n,
                               BlockContents* contents, uint32_t format_version,
                               const Slice& compression_dict,
                               const ImmutableCFOptions &ioptions) {
  assert(data[n] != kNoCompression);
  return UncompressBlockContentsForCompressionType(
           data, n, contents, format_version, compression_dict,
           (CompressionType)data[n], ioptions);
}

}  // namespace rocksdb
