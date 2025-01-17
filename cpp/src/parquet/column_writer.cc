// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "parquet/column_writer.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "arrow/buffer-builder.h"
#include "arrow/status.h"
#include "arrow/util/bit-stream-utils.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/compression.h"
#include "arrow/util/logging.h"
#include "arrow/util/rle-encoding.h"

#include "parquet/metadata.h"
#include "parquet/platform.h"
#include "parquet/properties.h"
#include "parquet/statistics.h"
#include "parquet/thrift.h"
#include "parquet/types.h"

#ifdef PARQUET_ENCRYPTION
#include "parquet/encryption_internal.h"
#include "parquet/internal_file_encryptor.h"
#endif

namespace parquet {

using ::arrow::internal::checked_cast;

using BitWriter = ::arrow::BitUtil::BitWriter;
using RleEncoder = ::arrow::util::RleEncoder;

LevelEncoder::LevelEncoder() {}
LevelEncoder::~LevelEncoder() {}

void LevelEncoder::Init(Encoding::type encoding, int16_t max_level,
                        int num_buffered_values, uint8_t* data, int data_size) {
  bit_width_ = BitUtil::Log2(max_level + 1);
  encoding_ = encoding;
  switch (encoding) {
    case Encoding::RLE: {
      rle_encoder_.reset(new RleEncoder(data, data_size, bit_width_));
      break;
    }
    case Encoding::BIT_PACKED: {
      int num_bytes =
          static_cast<int>(BitUtil::BytesForBits(num_buffered_values * bit_width_));
      bit_packed_encoder_.reset(new BitWriter(data, num_bytes));
      break;
    }
    default:
      throw ParquetException("Unknown encoding type for levels.");
  }
}

int LevelEncoder::MaxBufferSize(Encoding::type encoding, int16_t max_level,
                                int num_buffered_values) {
  int bit_width = BitUtil::Log2(max_level + 1);
  int num_bytes = 0;
  switch (encoding) {
    case Encoding::RLE: {
      // TODO: Due to the way we currently check if the buffer is full enough,
      // we need to have MinBufferSize as head room.
      num_bytes = RleEncoder::MaxBufferSize(bit_width, num_buffered_values) +
                  RleEncoder::MinBufferSize(bit_width);
      break;
    }
    case Encoding::BIT_PACKED: {
      num_bytes =
          static_cast<int>(BitUtil::BytesForBits(num_buffered_values * bit_width));
      break;
    }
    default:
      throw ParquetException("Unknown encoding type for levels.");
  }
  return num_bytes;
}

int LevelEncoder::Encode(int batch_size, const int16_t* levels) {
  int num_encoded = 0;
  if (!rle_encoder_ && !bit_packed_encoder_) {
    throw ParquetException("Level encoders are not initialized.");
  }

  if (encoding_ == Encoding::RLE) {
    for (int i = 0; i < batch_size; ++i) {
      if (!rle_encoder_->Put(*(levels + i))) {
        break;
      }
      ++num_encoded;
    }
    rle_encoder_->Flush();
    rle_length_ = rle_encoder_->len();
  } else {
    for (int i = 0; i < batch_size; ++i) {
      if (!bit_packed_encoder_->PutValue(*(levels + i), bit_width_)) {
        break;
      }
      ++num_encoded;
    }
    bit_packed_encoder_->Flush();
  }
  return num_encoded;
}

// ----------------------------------------------------------------------
// PageWriter implementation

// This subclass delimits pages appearing in a serialized stream, each preceded
// by a serialized Thrift format::PageHeader indicating the type of each page
// and the page metadata.
class SerializedPageWriter : public PageWriter {
 public:
  SerializedPageWriter(const std::shared_ptr<ArrowOutputStream>& sink,
                       Compression::type codec, ColumnChunkMetaDataBuilder* metadata,
                       int16_t row_group_ordinal, int16_t column_chunk_ordinal,
                       ::arrow::MemoryPool* pool = ::arrow::default_memory_pool(),
                       std::shared_ptr<Encryptor> meta_encryptor = NULLPTR,
                       std::shared_ptr<Encryptor> data_encryptor = NULLPTR)
      : sink_(sink),
        metadata_(metadata),
        pool_(pool),
        num_values_(0),
        dictionary_page_offset_(0),
        data_page_offset_(0),
        total_uncompressed_size_(0),
        total_compressed_size_(0),
        page_ordinal_(0),
        row_group_ordinal_(row_group_ordinal),
        column_ordinal_(column_chunk_ordinal),
        meta_encryptor_(meta_encryptor),
        data_encryptor_(data_encryptor) {
    if (data_encryptor_ != NULLPTR || meta_encryptor_ != NULLPTR) {
#ifdef PARQUET_ENCRYPTION
      InitEncryption();
#endif
    }
    compressor_ = GetCodecFromArrow(codec);
    thrift_serializer_.reset(new ThriftSerializer);
  }

  int64_t WriteDictionaryPage(const DictionaryPage& page) override {
    int64_t uncompressed_size = page.size();
    std::shared_ptr<Buffer> compressed_data = nullptr;
    if (has_compressor()) {
      auto buffer = std::static_pointer_cast<ResizableBuffer>(
          AllocateBuffer(pool_, uncompressed_size));
      Compress(*(page.buffer().get()), buffer.get());
      compressed_data = std::static_pointer_cast<Buffer>(buffer);
    } else {
      compressed_data = page.buffer();
    }

    format::DictionaryPageHeader dict_page_header;
    dict_page_header.__set_num_values(page.num_values());
    dict_page_header.__set_encoding(ToThrift(page.encoding()));
    dict_page_header.__set_is_sorted(page.is_sorted());

    const uint8_t* output_data_buffer = compressed_data->data();
    int32_t output_data_len = static_cast<int32_t>(compressed_data->size());

#ifdef PARQUET_ENCRYPTION
    std::shared_ptr<Buffer> encrypted_data_buffer = nullptr;
    if (data_encryptor_.get()) {
      UpdateEncryption(encryption::kDictionaryPage);
      encrypted_data_buffer = std::static_pointer_cast<ResizableBuffer>(AllocateBuffer(
          pool_, data_encryptor_->CiphertextSizeDelta() + output_data_len));
      output_data_len = data_encryptor_->Encrypt(compressed_data->data(), output_data_len,
                                                 encrypted_data_buffer->mutable_data());
      output_data_buffer = encrypted_data_buffer->data();
    }
#endif

    format::PageHeader page_header;
    page_header.__set_type(format::PageType::DICTIONARY_PAGE);
    page_header.__set_uncompressed_page_size(static_cast<int32_t>(uncompressed_size));
    page_header.__set_compressed_page_size(static_cast<int32_t>(output_data_len));
    page_header.__set_dictionary_page_header(dict_page_header);
    // TODO(PARQUET-594) crc checksum

    int64_t start_pos = -1;
    PARQUET_THROW_NOT_OK(sink_->Tell(&start_pos));
    if (dictionary_page_offset_ == 0) {
      dictionary_page_offset_ = start_pos;
    }

#ifdef PARQUET_ENCRYPTION
    if (meta_encryptor_) {
      UpdateEncryption(encryption::kDictionaryPageHeader);
    }
#endif
    int64_t header_size =
        thrift_serializer_->Serialize(&page_header, sink_.get(), meta_encryptor_);

    PARQUET_THROW_NOT_OK(sink_->Write(output_data_buffer, output_data_len));

    total_uncompressed_size_ += uncompressed_size + header_size;
    total_compressed_size_ += output_data_len + header_size;

    int64_t final_pos = -1;
    PARQUET_THROW_NOT_OK(sink_->Tell(&final_pos));
    return final_pos - start_pos;
  }

  void Close(bool has_dictionary, bool fallback) override {
#ifdef PARQUET_ENCRYPTION
    if (meta_encryptor_ != nullptr) {
      UpdateEncryption(encryption::kColumnMetaData);
    }
#endif
    // index_page_offset = -1 since they are not supported
    metadata_->Finish(num_values_, dictionary_page_offset_, -1, data_page_offset_,
                      total_compressed_size_, total_uncompressed_size_, has_dictionary,
                      fallback, meta_encryptor_);
    // Write metadata at end of column chunk
    metadata_->WriteTo(sink_.get());
  }

  /**
   * Compress a buffer.
   */
  void Compress(const Buffer& src_buffer, ResizableBuffer* dest_buffer) override {
    DCHECK(compressor_ != nullptr);

    // Compress the data
    int64_t max_compressed_size =
        compressor_->MaxCompressedLen(src_buffer.size(), src_buffer.data());

    // Use Arrow::Buffer::shrink_to_fit = false
    // underlying buffer only keeps growing. Resize to a smaller size does not reallocate.
    PARQUET_THROW_NOT_OK(dest_buffer->Resize(max_compressed_size, false));

    int64_t compressed_size;
    PARQUET_THROW_NOT_OK(
        compressor_->Compress(src_buffer.size(), src_buffer.data(), max_compressed_size,
                              dest_buffer->mutable_data(), &compressed_size));
    PARQUET_THROW_NOT_OK(dest_buffer->Resize(compressed_size, false));
  }

  int64_t WriteDataPage(const CompressedDataPage& page) override {
    int64_t uncompressed_size = page.uncompressed_size();
    std::shared_ptr<Buffer> compressed_data = page.buffer();
    format::DataPageHeader data_page_header;
    data_page_header.__set_num_values(page.num_values());
    data_page_header.__set_encoding(ToThrift(page.encoding()));
    data_page_header.__set_definition_level_encoding(
        ToThrift(page.definition_level_encoding()));
    data_page_header.__set_repetition_level_encoding(
        ToThrift(page.repetition_level_encoding()));
    data_page_header.__set_statistics(ToThrift(page.statistics()));

    const uint8_t* output_data_buffer = compressed_data->data();
    int32_t output_data_len = static_cast<int32_t>(compressed_data->size());

#ifdef PARQUET_ENCRYPTION
    std::shared_ptr<ResizableBuffer> encrypted_data_buffer = AllocateBuffer(pool_, 0);
    if (data_encryptor_.get()) {
      UpdateEncryption(encryption::kDataPage);
      PARQUET_THROW_NOT_OK(encrypted_data_buffer->Resize(
          data_encryptor_->CiphertextSizeDelta() + output_data_len));
      output_data_len = data_encryptor_->Encrypt(compressed_data->data(), output_data_len,
                                                 encrypted_data_buffer->mutable_data());
      output_data_buffer = encrypted_data_buffer->data();
    }
#endif

    format::PageHeader page_header;
    page_header.__set_type(format::PageType::DATA_PAGE);
    page_header.__set_uncompressed_page_size(static_cast<int32_t>(uncompressed_size));
    page_header.__set_compressed_page_size(static_cast<int32_t>(output_data_len));
    page_header.__set_data_page_header(data_page_header);
    // TODO(PARQUET-594) crc checksum

    int64_t start_pos = -1;
    PARQUET_THROW_NOT_OK(sink_->Tell(&start_pos));
    if (data_page_offset_ == 0) {
      data_page_offset_ = start_pos;
    }

#ifdef PARQUET_ENCRYPTION
    if (meta_encryptor_) {
      UpdateEncryption(encryption::kDataPageHeader);
    }
#endif
    int64_t header_size =
        thrift_serializer_->Serialize(&page_header, sink_.get(), meta_encryptor_);
    PARQUET_THROW_NOT_OK(sink_->Write(output_data_buffer, output_data_len));

    total_uncompressed_size_ += uncompressed_size + header_size;
    total_compressed_size_ += output_data_len + header_size;
    num_values_ += page.num_values();

    page_ordinal_++;
    int64_t current_pos = -1;
    PARQUET_THROW_NOT_OK(sink_->Tell(&current_pos));
    return current_pos - start_pos;
  }

  bool has_compressor() override { return (compressor_ != nullptr); }

  int64_t num_values() { return num_values_; }

  int64_t dictionary_page_offset() { return dictionary_page_offset_; }

  int64_t data_page_offset() { return data_page_offset_; }

  int64_t total_compressed_size() { return total_compressed_size_; }

  int64_t total_uncompressed_size() { return total_uncompressed_size_; }

 private:
#ifdef PARQUET_ENCRYPTION
  void InitEncryption() {
    // Prepare the AAD for quick update later.
    if (data_encryptor_ != NULLPTR) {
      data_pageAAD_ = encryption::CreateModuleAad(
          data_encryptor_->file_aad(), encryption::kDataPage, row_group_ordinal_,
          column_ordinal_, static_cast<int16_t>(-1));
    }
    if (meta_encryptor_ != NULLPTR) {
      data_page_headerAAD_ = encryption::CreateModuleAad(
          meta_encryptor_->file_aad(), encryption::kDataPageHeader, row_group_ordinal_,
          column_ordinal_, static_cast<int16_t>(-1));
    }
  }

  void UpdateEncryption(int8_t module_type) {
    switch (module_type) {
      case encryption::kColumnMetaData: {
        meta_encryptor_->UpdateAad(encryption::CreateModuleAad(
            meta_encryptor_->file_aad(), module_type, row_group_ordinal_, column_ordinal_,
            static_cast<int16_t>(-1)));
        break;
      }
      case encryption::kDataPage: {
        encryption::QuickUpdatePageAad(data_pageAAD_, page_ordinal_);
        data_encryptor_->UpdateAad(data_pageAAD_);
        break;
      }
      case encryption::kDataPageHeader: {
        encryption::QuickUpdatePageAad(data_page_headerAAD_, page_ordinal_);
        meta_encryptor_->UpdateAad(data_page_headerAAD_);
        break;
      }
      case encryption::kDictionaryPageHeader: {
        meta_encryptor_->UpdateAad(encryption::CreateModuleAad(
            meta_encryptor_->file_aad(), module_type, row_group_ordinal_, column_ordinal_,
            static_cast<int16_t>(-1)));
        break;
      }
      case encryption::kDictionaryPage: {
        data_encryptor_->UpdateAad(encryption::CreateModuleAad(
            data_encryptor_->file_aad(), module_type, row_group_ordinal_, column_ordinal_,
            static_cast<int16_t>(-1)));
        break;
      }
      default:
        throw ParquetException("Unknown module type in UpdateEncryption");
    }
  }
#endif

  std::shared_ptr<ArrowOutputStream> sink_;
  ColumnChunkMetaDataBuilder* metadata_;
  ::arrow::MemoryPool* pool_;
  int64_t num_values_;
  int64_t dictionary_page_offset_;
  int64_t data_page_offset_;
  int64_t total_uncompressed_size_;
  int64_t total_compressed_size_;
  int16_t page_ordinal_;
  int16_t row_group_ordinal_;
  int16_t column_ordinal_;

  std::unique_ptr<ThriftSerializer> thrift_serializer_;

  // Compression codec to use.
  std::unique_ptr<::arrow::util::Codec> compressor_;

#ifdef PARQUET_ENCRYPTION
  std::string data_pageAAD_;
  std::string data_page_headerAAD_;
#endif

  std::shared_ptr<Encryptor> meta_encryptor_;
  std::shared_ptr<Encryptor> data_encryptor_;
};

// This implementation of the PageWriter writes to the final sink on Close .
class BufferedPageWriter : public PageWriter {
 public:
  BufferedPageWriter(const std::shared_ptr<ArrowOutputStream>& sink,
                     Compression::type codec, ColumnChunkMetaDataBuilder* metadata,
                     int16_t row_group_ordinal, int16_t current_column_ordinal,
                     ::arrow::MemoryPool* pool = ::arrow::default_memory_pool(),
                     std::shared_ptr<Encryptor> meta_encryptor = NULLPTR,
                     std::shared_ptr<Encryptor> data_encryptor = NULLPTR)
      : final_sink_(sink), metadata_(metadata) {
    in_memory_sink_ = CreateOutputStream(pool);

    pager_ = std::unique_ptr<SerializedPageWriter>(new SerializedPageWriter(
        in_memory_sink_, codec, metadata, row_group_ordinal, current_column_ordinal, pool,
        meta_encryptor, data_encryptor));
  }

  int64_t WriteDictionaryPage(const DictionaryPage& page) override {
    return pager_->WriteDictionaryPage(page);
  }

  void Close(bool has_dictionary, bool fallback) override {
    // index_page_offset = -1 since they are not supported
    int64_t final_position = -1;
    PARQUET_THROW_NOT_OK(final_sink_->Tell(&final_position));
    metadata_->Finish(
        pager_->num_values(), pager_->dictionary_page_offset() + final_position, -1,
        pager_->data_page_offset() + final_position, pager_->total_compressed_size(),
        pager_->total_uncompressed_size(), has_dictionary, fallback);

    // Write metadata at end of column chunk
    metadata_->WriteTo(in_memory_sink_.get());

    // flush everything to the serialized sink
    std::shared_ptr<Buffer> buffer;
    PARQUET_THROW_NOT_OK(in_memory_sink_->Finish(&buffer));
    PARQUET_THROW_NOT_OK(final_sink_->Write(buffer->data(), buffer->size()));
  }

  int64_t WriteDataPage(const CompressedDataPage& page) override {
    return pager_->WriteDataPage(page);
  }

  void Compress(const Buffer& src_buffer, ResizableBuffer* dest_buffer) override {
    pager_->Compress(src_buffer, dest_buffer);
  }

  bool has_compressor() override { return pager_->has_compressor(); }

 private:
  std::shared_ptr<ArrowOutputStream> final_sink_;
  ColumnChunkMetaDataBuilder* metadata_;
  std::shared_ptr<::arrow::io::BufferOutputStream> in_memory_sink_;
  std::unique_ptr<SerializedPageWriter> pager_;
};

std::unique_ptr<PageWriter> PageWriter::Open(
    const std::shared_ptr<ArrowOutputStream>& sink, Compression::type codec,
    ColumnChunkMetaDataBuilder* metadata, int16_t row_group_ordinal,
    int16_t column_chunk_ordinal, ::arrow::MemoryPool* pool, bool buffered_row_group,
    std::shared_ptr<Encryptor> meta_encryptor,
    std::shared_ptr<Encryptor> data_encryptor) {
  if (buffered_row_group) {
    return std::unique_ptr<PageWriter>(new BufferedPageWriter(
        sink, codec, metadata, row_group_ordinal, column_chunk_ordinal, pool,
        meta_encryptor, data_encryptor));
  } else {
    return std::unique_ptr<PageWriter>(new SerializedPageWriter(
        sink, codec, metadata, row_group_ordinal, column_chunk_ordinal, pool,
        meta_encryptor, data_encryptor));
  }
}

// ----------------------------------------------------------------------
// ColumnWriter

std::shared_ptr<WriterProperties> default_writer_properties() {
  static std::shared_ptr<WriterProperties> default_writer_properties =
      WriterProperties::Builder().build();
  return default_writer_properties;
}

class ColumnWriterImpl {
 public:
  ColumnWriterImpl(ColumnChunkMetaDataBuilder* metadata,
                   std::unique_ptr<PageWriter> pager, const bool use_dictionary,
                   Encoding::type encoding, const WriterProperties* properties)
      : metadata_(metadata),
        descr_(metadata->descr()),
        pager_(std::move(pager)),
        has_dictionary_(use_dictionary),
        encoding_(encoding),
        properties_(properties),
        allocator_(properties->memory_pool()),
        num_buffered_values_(0),
        num_buffered_encoded_values_(0),
        rows_written_(0),
        total_bytes_written_(0),
        total_compressed_bytes_(0),
        closed_(false),
        fallback_(false),
        definition_levels_sink_(allocator_),
        repetition_levels_sink_(allocator_) {
    definition_levels_rle_ =
        std::static_pointer_cast<ResizableBuffer>(AllocateBuffer(allocator_, 0));
    repetition_levels_rle_ =
        std::static_pointer_cast<ResizableBuffer>(AllocateBuffer(allocator_, 0));
    uncompressed_data_ =
        std::static_pointer_cast<ResizableBuffer>(AllocateBuffer(allocator_, 0));
    if (pager_->has_compressor()) {
      compressed_data_ =
          std::static_pointer_cast<ResizableBuffer>(AllocateBuffer(allocator_, 0));
    }
  }

  virtual ~ColumnWriterImpl() = default;

  int64_t Close();

 protected:
  virtual std::shared_ptr<Buffer> GetValuesBuffer() = 0;

  // Serializes Dictionary Page if enabled
  virtual void WriteDictionaryPage() = 0;

  // Plain-encoded statistics of the current page
  virtual EncodedStatistics GetPageStatistics() = 0;

  // Plain-encoded statistics of the whole chunk
  virtual EncodedStatistics GetChunkStatistics() = 0;

  // Merges page statistics into chunk statistics, then resets the values
  virtual void ResetPageStatistics() = 0;

  // Adds Data Pages to an in memory buffer in dictionary encoding mode
  // Serializes the Data Pages in other encoding modes
  void AddDataPage();

  // Serializes Data Pages
  void WriteDataPage(const CompressedDataPage& page);

  // Write multiple definition levels
  void WriteDefinitionLevels(int64_t num_levels, const int16_t* levels) {
    DCHECK(!closed_);
    PARQUET_THROW_NOT_OK(
        definition_levels_sink_.Append(levels, sizeof(int16_t) * num_levels));
  }

  // Write multiple repetition levels
  void WriteRepetitionLevels(int64_t num_levels, const int16_t* levels) {
    DCHECK(!closed_);
    PARQUET_THROW_NOT_OK(
        repetition_levels_sink_.Append(levels, sizeof(int16_t) * num_levels));
  }

  // RLE encode the src_buffer into dest_buffer and return the encoded size
  int64_t RleEncodeLevels(const void* src_buffer, ResizableBuffer* dest_buffer,
                          int16_t max_level);

  // Serialize the buffered Data Pages
  void FlushBufferedDataPages();

  ColumnChunkMetaDataBuilder* metadata_;
  const ColumnDescriptor* descr_;

  std::unique_ptr<PageWriter> pager_;

  bool has_dictionary_;
  Encoding::type encoding_;
  const WriterProperties* properties_;

  LevelEncoder level_encoder_;

  ::arrow::MemoryPool* allocator_;

  // The total number of values stored in the data page. This is the maximum of
  // the number of encoded definition levels or encoded values. For
  // non-repeated, required columns, this is equal to the number of encoded
  // values. For repeated or optional values, there may be fewer data values
  // than levels, and this tells you how many encoded levels there are in that
  // case.
  int64_t num_buffered_values_;

  // The total number of stored values. For repeated or optional values, this
  // number may be lower than num_buffered_values_.
  int64_t num_buffered_encoded_values_;

  // Total number of rows written with this ColumnWriter
  int rows_written_;

  // Records the total number of bytes written by the serializer
  int64_t total_bytes_written_;

  // Records the current number of compressed bytes in a column
  int64_t total_compressed_bytes_;

  // Flag to check if the Writer has been closed
  bool closed_;

  // Flag to infer if dictionary encoding has fallen back to PLAIN
  bool fallback_;

  ::arrow::BufferBuilder definition_levels_sink_;
  ::arrow::BufferBuilder repetition_levels_sink_;

  std::shared_ptr<ResizableBuffer> definition_levels_rle_;
  std::shared_ptr<ResizableBuffer> repetition_levels_rle_;

  std::shared_ptr<ResizableBuffer> uncompressed_data_;
  std::shared_ptr<ResizableBuffer> compressed_data_;

  std::vector<CompressedDataPage> data_pages_;

 private:
  void InitSinks() {
    definition_levels_sink_.Rewind(0);
    repetition_levels_sink_.Rewind(0);
  }
};

// return the size of the encoded buffer
int64_t ColumnWriterImpl::RleEncodeLevels(const void* src_buffer,
                                          ResizableBuffer* dest_buffer,
                                          int16_t max_level) {
  // TODO: This only works with due to some RLE specifics
  int64_t rle_size = LevelEncoder::MaxBufferSize(Encoding::RLE, max_level,
                                                 static_cast<int>(num_buffered_values_)) +
                     sizeof(int32_t);

  // Use Arrow::Buffer::shrink_to_fit = false
  // underlying buffer only keeps growing. Resize to a smaller size does not reallocate.
  PARQUET_THROW_NOT_OK(dest_buffer->Resize(rle_size, false));

  level_encoder_.Init(Encoding::RLE, max_level, static_cast<int>(num_buffered_values_),
                      dest_buffer->mutable_data() + sizeof(int32_t),
                      static_cast<int>(dest_buffer->size() - sizeof(int32_t)));
  int encoded = level_encoder_.Encode(static_cast<int>(num_buffered_values_),
                                      reinterpret_cast<const int16_t*>(src_buffer));
  DCHECK_EQ(encoded, num_buffered_values_);
  reinterpret_cast<int32_t*>(dest_buffer->mutable_data())[0] = level_encoder_.len();
  int64_t encoded_size = level_encoder_.len() + sizeof(int32_t);
  return encoded_size;
}

void ColumnWriterImpl::AddDataPage() {
  int64_t definition_levels_rle_size = 0;
  int64_t repetition_levels_rle_size = 0;

  std::shared_ptr<Buffer> values = GetValuesBuffer();

  if (descr_->max_definition_level() > 0) {
    definition_levels_rle_size =
        RleEncodeLevels(definition_levels_sink_.data(), definition_levels_rle_.get(),
                        descr_->max_definition_level());
  }

  if (descr_->max_repetition_level() > 0) {
    repetition_levels_rle_size =
        RleEncodeLevels(repetition_levels_sink_.data(), repetition_levels_rle_.get(),
                        descr_->max_repetition_level());
  }

  int64_t uncompressed_size =
      definition_levels_rle_size + repetition_levels_rle_size + values->size();

  // Use Arrow::Buffer::shrink_to_fit = false
  // underlying buffer only keeps growing. Resize to a smaller size does not reallocate.
  PARQUET_THROW_NOT_OK(uncompressed_data_->Resize(uncompressed_size, false));

  // Concatenate data into a single buffer
  uint8_t* uncompressed_ptr = uncompressed_data_->mutable_data();
  memcpy(uncompressed_ptr, repetition_levels_rle_->data(), repetition_levels_rle_size);
  uncompressed_ptr += repetition_levels_rle_size;
  memcpy(uncompressed_ptr, definition_levels_rle_->data(), definition_levels_rle_size);
  uncompressed_ptr += definition_levels_rle_size;
  memcpy(uncompressed_ptr, values->data(), values->size());

  EncodedStatistics page_stats = GetPageStatistics();
  page_stats.ApplyStatSizeLimits(properties_->max_statistics_size(descr_->path()));
  page_stats.set_is_signed(SortOrder::SIGNED == descr_->sort_order());
  ResetPageStatistics();

  std::shared_ptr<Buffer> compressed_data;
  if (pager_->has_compressor()) {
    pager_->Compress(*(uncompressed_data_.get()), compressed_data_.get());
    compressed_data = compressed_data_;
  } else {
    compressed_data = uncompressed_data_;
  }

  // Write the page to OutputStream eagerly if there is no dictionary or
  // if dictionary encoding has fallen back to PLAIN
  if (has_dictionary_ && !fallback_) {  // Save pages until end of dictionary encoding
    std::shared_ptr<Buffer> compressed_data_copy;
    PARQUET_THROW_NOT_OK(compressed_data->Copy(0, compressed_data->size(), allocator_,
                                               &compressed_data_copy));
    CompressedDataPage page(compressed_data_copy,
                            static_cast<int32_t>(num_buffered_values_), encoding_,
                            Encoding::RLE, Encoding::RLE, uncompressed_size, page_stats);
    total_compressed_bytes_ += page.size() + sizeof(format::PageHeader);
    data_pages_.push_back(std::move(page));
  } else {  // Eagerly write pages
    CompressedDataPage page(compressed_data, static_cast<int32_t>(num_buffered_values_),
                            encoding_, Encoding::RLE, Encoding::RLE, uncompressed_size,
                            page_stats);
    WriteDataPage(page);
  }

  // Re-initialize the sinks for next Page.
  InitSinks();
  num_buffered_values_ = 0;
  num_buffered_encoded_values_ = 0;
}

void ColumnWriterImpl::WriteDataPage(const CompressedDataPage& page) {
  total_bytes_written_ += pager_->WriteDataPage(page);
}

int64_t ColumnWriterImpl::Close() {
  if (!closed_) {
    closed_ = true;
    if (has_dictionary_ && !fallback_) {
      WriteDictionaryPage();
    }

    FlushBufferedDataPages();

    EncodedStatistics chunk_statistics = GetChunkStatistics();
    chunk_statistics.ApplyStatSizeLimits(
        properties_->max_statistics_size(descr_->path()));
    chunk_statistics.set_is_signed(SortOrder::SIGNED == descr_->sort_order());

    // Write stats only if the column has at least one row written
    if (rows_written_ > 0 && chunk_statistics.is_set()) {
      metadata_->SetStatistics(chunk_statistics);
    }
    pager_->Close(has_dictionary_, fallback_);
  }

  return total_bytes_written_;
}

void ColumnWriterImpl::FlushBufferedDataPages() {
  // Write all outstanding data to a new page
  if (num_buffered_values_ > 0) {
    AddDataPage();
  }
  for (size_t i = 0; i < data_pages_.size(); i++) {
    WriteDataPage(data_pages_[i]);
  }
  data_pages_.clear();
  total_compressed_bytes_ = 0;
}

// ----------------------------------------------------------------------
// TypedColumnWriter

template <typename DType>
class TypedColumnWriterImpl : public ColumnWriterImpl, public TypedColumnWriter<DType> {
 public:
  using T = typename DType::c_type;

  TypedColumnWriterImpl(ColumnChunkMetaDataBuilder* metadata,
                        std::unique_ptr<PageWriter> pager, const bool use_dictionary,
                        Encoding::type encoding, const WriterProperties* properties)
      : ColumnWriterImpl(metadata, std::move(pager), use_dictionary, encoding,
                         properties) {
    current_encoder_ = MakeEncoder(DType::type_num, encoding, use_dictionary, descr_,
                                   properties->memory_pool());

    if (properties->statistics_enabled(descr_->path()) &&
        (SortOrder::UNKNOWN != descr_->sort_order())) {
      page_statistics_ = TypedStats::Make(descr_, allocator_);
      chunk_statistics_ = TypedStats::Make(descr_, allocator_);
    }
  }

  int64_t Close() override { return ColumnWriterImpl::Close(); }

  void WriteBatch(int64_t num_values, const int16_t* def_levels,
                  const int16_t* rep_levels, const T* values) override;

  void WriteBatchSpaced(int64_t num_values, const int16_t* def_levels,
                        const int16_t* rep_levels, const uint8_t* valid_bits,
                        int64_t valid_bits_offset, const T* values) override;

  int64_t EstimatedBufferedValueBytes() const override {
    return current_encoder_->EstimatedDataEncodedSize();
  }

 protected:
  std::shared_ptr<Buffer> GetValuesBuffer() override {
    return current_encoder_->FlushValues();
  }
  void WriteDictionaryPage() override;

  // Checks if the Dictionary Page size limit is reached
  // If the limit is reached, the Dictionary and Data Pages are serialized
  // The encoding is switched to PLAIN
  void CheckDictionarySizeLimit();

  EncodedStatistics GetPageStatistics() override {
    EncodedStatistics result;
    if (page_statistics_) result = page_statistics_->Encode();
    return result;
  }

  EncodedStatistics GetChunkStatistics() override {
    EncodedStatistics result;
    if (chunk_statistics_) result = chunk_statistics_->Encode();
    return result;
  }

  void ResetPageStatistics() override;

  Type::type type() const override { return descr_->physical_type(); }

  const ColumnDescriptor* descr() const override { return descr_; }

  int64_t rows_written() const override { return rows_written_; }

  int64_t total_compressed_bytes() const override { return total_compressed_bytes_; }

  int64_t total_bytes_written() const override { return total_bytes_written_; }

  const WriterProperties* properties() override { return properties_; }

 private:
  inline int64_t WriteMiniBatch(int64_t num_values, const int16_t* def_levels,
                                const int16_t* rep_levels, const T* values);

  inline int64_t WriteMiniBatchSpaced(int64_t num_values, const int16_t* def_levels,
                                      const int16_t* rep_levels,
                                      const uint8_t* valid_bits,
                                      int64_t valid_bits_offset, const T* values,
                                      int64_t* num_spaced_written);

  // Write values to a temporary buffer before they are encoded into pages
  void WriteValues(int64_t num_values, const T* values);
  void WriteValuesSpaced(int64_t num_values, const uint8_t* valid_bits,
                         int64_t valid_bits_offset, const T* values);

  using ValueEncoderType = typename EncodingTraits<DType>::Encoder;
  std::unique_ptr<Encoder> current_encoder_;

  using TypedStats = TypedStatistics<DType>;
  std::shared_ptr<TypedStats> page_statistics_;
  std::shared_ptr<TypedStats> chunk_statistics_;
};

// Only one Dictionary Page is written.
// Fallback to PLAIN if dictionary page limit is reached.
template <typename DType>
void TypedColumnWriterImpl<DType>::CheckDictionarySizeLimit() {
  // We have to dynamic cast here because TypedEncoder<Type> as some compilers
  // don't want to cast through virtual inheritance
  auto dict_encoder = dynamic_cast<DictEncoder<DType>*>(current_encoder_.get());
  if (dict_encoder->dict_encoded_size() >= properties_->dictionary_pagesize_limit()) {
    WriteDictionaryPage();
    // Serialize the buffered Dictionary Indicies
    FlushBufferedDataPages();
    fallback_ = true;
    // Only PLAIN encoding is supported for fallback in V1
    current_encoder_ = MakeEncoder(DType::type_num, Encoding::PLAIN, false, descr_,
                                   properties_->memory_pool());
    encoding_ = Encoding::PLAIN;
  }
}

template <typename DType>
void TypedColumnWriterImpl<DType>::WriteDictionaryPage() {
  // We have to dynamic cast here because TypedEncoder<Type> as some compilers
  // don't want to cast through virtual inheritance
  auto dict_encoder = dynamic_cast<DictEncoder<DType>*>(current_encoder_.get());
  DCHECK(dict_encoder);
  std::shared_ptr<ResizableBuffer> buffer =
      AllocateBuffer(properties_->memory_pool(), dict_encoder->dict_encoded_size());
  dict_encoder->WriteDict(buffer->mutable_data());

  DictionaryPage page(buffer, dict_encoder->num_entries(),
                      properties_->dictionary_page_encoding());
  total_bytes_written_ += pager_->WriteDictionaryPage(page);
}

template <typename DType>
void TypedColumnWriterImpl<DType>::ResetPageStatistics() {
  if (chunk_statistics_ != nullptr) {
    chunk_statistics_->Merge(*page_statistics_);
    page_statistics_->Reset();
  }
}

// ----------------------------------------------------------------------
// Instantiate templated classes

template <typename DType>
int64_t TypedColumnWriterImpl<DType>::WriteMiniBatch(int64_t num_values,
                                                     const int16_t* def_levels,
                                                     const int16_t* rep_levels,
                                                     const T* values) {
  int64_t values_to_write = 0;
  // If the field is required and non-repeated, there are no definition levels
  if (descr_->max_definition_level() > 0) {
    for (int64_t i = 0; i < num_values; ++i) {
      if (def_levels[i] == descr_->max_definition_level()) {
        ++values_to_write;
      }
    }

    WriteDefinitionLevels(num_values, def_levels);
  } else {
    // Required field, write all values
    values_to_write = num_values;
  }

  // Not present for non-repeated fields
  if (descr_->max_repetition_level() > 0) {
    // A row could include more than one value
    // Count the occasions where we start a new row
    for (int64_t i = 0; i < num_values; ++i) {
      if (rep_levels[i] == 0) {
        rows_written_++;
      }
    }

    WriteRepetitionLevels(num_values, rep_levels);
  } else {
    // Each value is exactly one row
    rows_written_ += static_cast<int>(num_values);
  }

  // PARQUET-780
  if (values_to_write > 0) {
    DCHECK(nullptr != values) << "Values ptr cannot be NULL";
  }

  WriteValues(values_to_write, values);

  if (page_statistics_ != nullptr) {
    page_statistics_->Update(values, values_to_write, num_values - values_to_write);
  }

  num_buffered_values_ += num_values;
  num_buffered_encoded_values_ += values_to_write;

  if (current_encoder_->EstimatedDataEncodedSize() >= properties_->data_pagesize()) {
    AddDataPage();
  }
  if (has_dictionary_ && !fallback_) {
    CheckDictionarySizeLimit();
  }

  return values_to_write;
}

template <typename DType>
int64_t TypedColumnWriterImpl<DType>::WriteMiniBatchSpaced(
    int64_t num_levels, const int16_t* def_levels, const int16_t* rep_levels,
    const uint8_t* valid_bits, int64_t valid_bits_offset, const T* values,
    int64_t* num_spaced_written) {
  int64_t values_to_write = 0;
  int64_t spaced_values_to_write = 0;
  // If the field is required and non-repeated, there are no definition levels
  if (descr_->max_definition_level() > 0) {
    // Minimal definition level for which spaced values are written
    int16_t min_spaced_def_level = descr_->max_definition_level();
    if (descr_->schema_node()->is_optional()) {
      min_spaced_def_level--;
    }
    for (int64_t i = 0; i < num_levels; ++i) {
      if (def_levels[i] == descr_->max_definition_level()) {
        ++values_to_write;
      }
      if (def_levels[i] >= min_spaced_def_level) {
        ++spaced_values_to_write;
      }
    }

    WriteDefinitionLevels(num_levels, def_levels);
  } else {
    // Required field, write all values
    values_to_write = num_levels;
    spaced_values_to_write = num_levels;
  }

  // Not present for non-repeated fields
  if (descr_->max_repetition_level() > 0) {
    // A row could include more than one value
    // Count the occasions where we start a new row
    for (int64_t i = 0; i < num_levels; ++i) {
      if (rep_levels[i] == 0) {
        rows_written_++;
      }
    }

    WriteRepetitionLevels(num_levels, rep_levels);
  } else {
    // Each value is exactly one row
    rows_written_ += static_cast<int>(num_levels);
  }

  if (descr_->schema_node()->is_optional()) {
    WriteValuesSpaced(spaced_values_to_write, valid_bits, valid_bits_offset, values);
  } else {
    WriteValues(values_to_write, values);
  }
  *num_spaced_written = spaced_values_to_write;

  if (page_statistics_ != nullptr) {
    page_statistics_->UpdateSpaced(values, valid_bits, valid_bits_offset, values_to_write,
                                   spaced_values_to_write - values_to_write);
  }

  num_buffered_values_ += num_levels;
  num_buffered_encoded_values_ += values_to_write;

  if (current_encoder_->EstimatedDataEncodedSize() >= properties_->data_pagesize()) {
    AddDataPage();
  }
  if (has_dictionary_ && !fallback_) {
    CheckDictionarySizeLimit();
  }

  return values_to_write;
}

template <typename DType>
void TypedColumnWriterImpl<DType>::WriteBatch(int64_t num_values,
                                              const int16_t* def_levels,
                                              const int16_t* rep_levels,
                                              const T* values) {
  // We check for DataPage limits only after we have inserted the values. If a user
  // writes a large number of values, the DataPage size can be much above the limit.
  // The purpose of this chunking is to bound this. Even if a user writes large number
  // of values, the chunking will ensure the AddDataPage() is called at a reasonable
  // pagesize limit
  int64_t write_batch_size = properties_->write_batch_size();
  int num_batches = static_cast<int>(num_values / write_batch_size);
  int64_t num_remaining = num_values % write_batch_size;
  int64_t value_offset = 0;
  for (int round = 0; round < num_batches; round++) {
    int64_t offset = round * write_batch_size;
    int64_t num_values = WriteMiniBatch(write_batch_size, &def_levels[offset],
                                        &rep_levels[offset], &values[value_offset]);
    value_offset += num_values;
  }
  // Write the remaining values
  int64_t offset = num_batches * write_batch_size;
  WriteMiniBatch(num_remaining, &def_levels[offset], &rep_levels[offset],
                 &values[value_offset]);
}

template <typename DType>
void TypedColumnWriterImpl<DType>::WriteBatchSpaced(
    int64_t num_values, const int16_t* def_levels, const int16_t* rep_levels,
    const uint8_t* valid_bits, int64_t valid_bits_offset, const T* values) {
  // We check for DataPage limits only after we have inserted the values. If a user
  // writes a large number of values, the DataPage size can be much above the limit.
  // The purpose of this chunking is to bound this. Even if a user writes large number
  // of values, the chunking will ensure the AddDataPage() is called at a reasonable
  // pagesize limit
  int64_t write_batch_size = properties_->write_batch_size();
  int num_batches = static_cast<int>(num_values / write_batch_size);
  int64_t num_remaining = num_values % write_batch_size;
  int64_t num_spaced_written = 0;
  int64_t values_offset = 0;
  for (int round = 0; round < num_batches; round++) {
    int64_t offset = round * write_batch_size;
    WriteMiniBatchSpaced(write_batch_size, &def_levels[offset], &rep_levels[offset],
                         valid_bits, valid_bits_offset + values_offset,
                         values + values_offset, &num_spaced_written);
    values_offset += num_spaced_written;
  }
  // Write the remaining values
  int64_t offset = num_batches * write_batch_size;
  WriteMiniBatchSpaced(num_remaining, &def_levels[offset], &rep_levels[offset],
                       valid_bits, valid_bits_offset + values_offset,
                       values + values_offset, &num_spaced_written);
}

template <typename DType>
void TypedColumnWriterImpl<DType>::WriteValues(int64_t num_values, const T* values) {
  dynamic_cast<ValueEncoderType*>(current_encoder_.get())
      ->Put(values, static_cast<int>(num_values));
}

template <typename DType>
void TypedColumnWriterImpl<DType>::WriteValuesSpaced(int64_t num_values,
                                                     const uint8_t* valid_bits,
                                                     int64_t valid_bits_offset,
                                                     const T* values) {
  dynamic_cast<ValueEncoderType*>(current_encoder_.get())
      ->PutSpaced(values, static_cast<int>(num_values), valid_bits, valid_bits_offset);
}

// ----------------------------------------------------------------------
// Dynamic column writer constructor

std::shared_ptr<ColumnWriter> ColumnWriter::Make(ColumnChunkMetaDataBuilder* metadata,
                                                 std::unique_ptr<PageWriter> pager,
                                                 const WriterProperties* properties) {
  const ColumnDescriptor* descr = metadata->descr();
  const bool use_dictionary = properties->dictionary_enabled(descr->path()) &&
                              descr->physical_type() != Type::BOOLEAN;
  Encoding::type encoding = properties->encoding(descr->path());
  if (use_dictionary) {
    encoding = properties->dictionary_index_encoding();
  }
  switch (descr->physical_type()) {
    case Type::BOOLEAN:
      return std::make_shared<TypedColumnWriterImpl<BooleanType>>(
          metadata, std::move(pager), use_dictionary, encoding, properties);
    case Type::INT32:
      return std::make_shared<TypedColumnWriterImpl<Int32Type>>(
          metadata, std::move(pager), use_dictionary, encoding, properties);
    case Type::INT64:
      return std::make_shared<TypedColumnWriterImpl<Int64Type>>(
          metadata, std::move(pager), use_dictionary, encoding, properties);
    case Type::INT96:
      return std::make_shared<TypedColumnWriterImpl<Int96Type>>(
          metadata, std::move(pager), use_dictionary, encoding, properties);
    case Type::FLOAT:
      return std::make_shared<TypedColumnWriterImpl<FloatType>>(
          metadata, std::move(pager), use_dictionary, encoding, properties);
    case Type::DOUBLE:
      return std::make_shared<TypedColumnWriterImpl<DoubleType>>(
          metadata, std::move(pager), use_dictionary, encoding, properties);
    case Type::BYTE_ARRAY:
      return std::make_shared<TypedColumnWriterImpl<ByteArrayType>>(
          metadata, std::move(pager), use_dictionary, encoding, properties);
    case Type::FIXED_LEN_BYTE_ARRAY:
      return std::make_shared<TypedColumnWriterImpl<FLBAType>>(
          metadata, std::move(pager), use_dictionary, encoding, properties);
    default:
      ParquetException::NYI("type reader not implemented");
  }
  // Unreachable code, but supress compiler warning
  return std::shared_ptr<ColumnWriter>(nullptr);
}

}  // namespace parquet
