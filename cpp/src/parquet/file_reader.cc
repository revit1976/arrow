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

#include "parquet/file_reader.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "arrow/buffer.h"
#include "arrow/io/file.h"
#include "arrow/status.h"
#include "arrow/util/logging.h"
#include "arrow/util/ubsan.h"

#include "parquet/column_reader.h"
#include "parquet/column_scanner.h"
#include "parquet/deprecated_io.h"
#include "parquet/exception.h"
#include "parquet/file_writer.h"
#include "parquet/metadata.h"
#include "parquet/platform.h"
#include "parquet/properties.h"
#include "parquet/schema.h"
#include "parquet/types.h"

#ifdef PARQUET_ENCRYPTION
#include "parquet/encryption_internal.h"
#include "parquet/internal_file_decryptor.h"
#else
namespace parquet {
class InternalFileDecryptor;
}
#endif

namespace parquet {

// PARQUET-978: Minimize footer reads by reading 64 KB from the end of the file
static constexpr int64_t kDefaultFooterReadSize = 64 * 1024;
static constexpr uint32_t kFooterSize = 8;

// For PARQUET-816
static constexpr int64_t kMaxDictHeaderSize = 100;

// ----------------------------------------------------------------------
// RowGroupReader public API

RowGroupReader::RowGroupReader(std::unique_ptr<Contents> contents)
    : contents_(std::move(contents)) {}

std::shared_ptr<ColumnReader> RowGroupReader::Column(int i) {
  DCHECK(i < metadata()->num_columns())
      << "The RowGroup only has " << metadata()->num_columns()
      << "columns, requested column: " << i;
  const ColumnDescriptor* descr = metadata()->schema()->Column(i);

  std::unique_ptr<PageReader> page_reader = contents_->GetColumnPageReader(i);
  return ColumnReader::Make(
      descr, std::move(page_reader),
      const_cast<ReaderProperties*>(contents_->properties())->memory_pool());
}

std::unique_ptr<PageReader> RowGroupReader::GetColumnPageReader(int i) {
  DCHECK(i < metadata()->num_columns())
      << "The RowGroup only has " << metadata()->num_columns()
      << "columns, requested column: " << i;
  return contents_->GetColumnPageReader(i);
}

// Returns the rowgroup metadata
const RowGroupMetaData* RowGroupReader::metadata() const { return contents_->metadata(); }

// RowGroupReader::Contents implementation for the Parquet file specification
class SerializedRowGroup : public RowGroupReader::Contents {
 public:
  SerializedRowGroup(const std::shared_ptr<ArrowInputFile>& source,
                     FileMetaData* file_metadata, int row_group_number,
                     const ReaderProperties& props,
                     InternalFileDecryptor* file_decryptor = NULLPTR)
      : source_(source),
        file_metadata_(file_metadata),
        properties_(props),
        row_group_ordinal_((int16_t)row_group_number),
        file_decryptor_(file_decryptor) {
    row_group_metadata_ = file_metadata->RowGroup(row_group_number);
  }

  const RowGroupMetaData* metadata() const override { return row_group_metadata_.get(); }

  const ReaderProperties* properties() const override { return &properties_; }

  std::unique_ptr<PageReader> GetColumnPageReader(int i) override {
    // Read column chunk from the file
    auto col = row_group_metadata_->ColumnChunk(i, row_group_ordinal_, file_decryptor_);

    int64_t col_start = col->data_page_offset();
    if (col->has_dictionary_page() && col->dictionary_page_offset() > 0 &&
        col_start > col->dictionary_page_offset()) {
      col_start = col->dictionary_page_offset();
    }

    int64_t col_length = col->total_compressed_size();

    // PARQUET-816 workaround for old files created by older parquet-mr
    const ApplicationVersion& version = file_metadata_->writer_version();
    if (version.VersionLt(ApplicationVersion::PARQUET_816_FIXED_VERSION())) {
      // The Parquet MR writer had a bug in 1.2.8 and below where it didn't include the
      // dictionary page header size in total_compressed_size and total_uncompressed_size
      // (see IMPALA-694). We add padding to compensate.
      int64_t size = -1;
      PARQUET_THROW_NOT_OK(source_->GetSize(&size));
      int64_t bytes_remaining = size - (col_start + col_length);
      int64_t padding = std::min<int64_t>(kMaxDictHeaderSize, bytes_remaining);
      col_length += padding;
    }

    std::shared_ptr<ArrowInputStream> stream =
        properties_.GetStream(source_, col_start, col_length);

#ifdef PARQUET_ENCRYPTION
    std::unique_ptr<ColumnCryptoMetaData> crypto_metadata = col->crypto_metadata();

    // Column is encrypted only if crypto_metadata exists.
    if (!crypto_metadata) {
      PageReaderContext ctx = {col->has_dictionary_page(), row_group_ordinal_,
                               static_cast<int16_t>(i), NULLPTR, NULLPTR};
      return PageReader::Open(stream, col->num_values(), col->compression(),
                              properties_.memory_pool(), &ctx);
    }

    // The column is encrypted

    // The column is encrypted with footer key
    if (crypto_metadata->encrypted_with_footer_key()) {
      auto meta_decryptor = file_decryptor_->GetFooterDecryptorForColumnMeta();
      auto data_decryptor = file_decryptor_->GetFooterDecryptorForColumnData();
      PageReaderContext ctx = {col->has_dictionary_page(), row_group_ordinal_,
                               static_cast<int16_t>(i), meta_decryptor, data_decryptor};
      return PageReader::Open(stream, col->num_values(), col->compression(),
                              properties_.memory_pool(), &ctx);
    }

    // The column is encrypted with its own key
    std::string column_key_metadata = crypto_metadata->key_metadata();
    std::shared_ptr<schema::ColumnPath> column_path =
        std::make_shared<schema::ColumnPath>(crypto_metadata->path_in_schema());

    auto meta_decryptor =
        file_decryptor_->GetColumnMetaDecryptor(column_path, column_key_metadata);
    auto data_decryptor =
        file_decryptor_->GetColumnDataDecryptor(column_path, column_key_metadata);

    PageReaderContext ctx = {col->has_dictionary_page(), row_group_ordinal_,
                             static_cast<int16_t>(i), meta_decryptor, data_decryptor};
    return PageReader::Open(stream, col->num_values(), col->compression(),
                            properties_.memory_pool(), &ctx);
#else
    return PageReader::Open(stream, col->num_values(), col->compression(),
                            properties_.memory_pool());
#endif
  }

 private:
  std::shared_ptr<ArrowInputFile> source_;
  FileMetaData* file_metadata_;
  std::unique_ptr<RowGroupMetaData> row_group_metadata_;
  ReaderProperties properties_;
  int16_t row_group_ordinal_;
  InternalFileDecryptor* file_decryptor_;
};

// ----------------------------------------------------------------------
// SerializedFile: An implementation of ParquetFileReader::Contents that deals
// with the Parquet file structure, Thrift deserialization, and other internal
// matters

// This class takes ownership of the provided data source
class SerializedFile : public ParquetFileReader::Contents {
 public:
  SerializedFile(const std::shared_ptr<ArrowInputFile>& source,
                 const ReaderProperties& props = default_reader_properties())
      : source_(source), properties_(props) {}

  ~SerializedFile() override {
    try {
      Close();
    } catch (...) {
    }
  }

  void Close() override {
#ifdef PARQUET_ENCRYPTION
    if (file_decryptor_) file_decryptor_->WipeOutDecryptionKeys();
#endif
  }

  std::shared_ptr<RowGroupReader> GetRowGroup(int i) override {
#ifdef PARQUET_ENCRYPTION
    std::unique_ptr<SerializedRowGroup> contents(new SerializedRowGroup(
        source_, file_metadata_.get(), i, properties_, file_decryptor_.get()));
#else
    std::unique_ptr<SerializedRowGroup> contents(
        new SerializedRowGroup(source_, file_metadata_.get(), i, properties_));
#endif
    return std::make_shared<RowGroupReader>(std::move(contents));
  }

  std::shared_ptr<FileMetaData> metadata() const override { return file_metadata_; }

  void set_metadata(const std::shared_ptr<FileMetaData>& metadata) {
    file_metadata_ = metadata;
  }

  void ParseMetaData() {
    int64_t file_size = -1;
    PARQUET_THROW_NOT_OK(source_->GetSize(&file_size));

    if (file_size == 0) {
      throw ParquetException("Invalid Parquet file size is 0 bytes");
    } else if (file_size < kFooterSize) {
      std::stringstream ss;
      ss << "Invalid Parquet file size is " << file_size
         << " bytes, smaller than standard file footer (" << kFooterSize << " bytes)";
      throw ParquetException(ss.str());
    }

    std::shared_ptr<Buffer> footer_buffer;
    int64_t footer_read_size = std::min(file_size, kDefaultFooterReadSize);
    PARQUET_THROW_NOT_OK(
        source_->ReadAt(file_size - footer_read_size, footer_read_size, &footer_buffer));

    // Check if all bytes are read. Check if last 4 bytes read have the magic bits
#ifdef PARQUET_ENCRYPTION
    if (footer_buffer->size() != footer_read_size ||
        (memcmp(footer_buffer->data() + footer_read_size - 4, kParquetMagic, 4) != 0 &&
         memcmp(footer_buffer->data() + footer_read_size - 4, kParquetEMagic, 4) != 0)) {
      throw ParquetException("Invalid parquet file. Corrupt footer.");
    }
#else
    if (footer_buffer->size() != footer_read_size ||
        (memcmp(footer_buffer->data() + footer_read_size - 4, kParquetMagic, 4) != 0 &&
         memcmp(footer_buffer->data() + footer_read_size - 4, kParquetEMagic, 4) != 0)) {
      throw ParquetException("Invalid parquet file. Corrupt footer.");
    } else if (memcmp(footer_buffer->data() + footer_read_size - 4, kParquetEMagic, 4) ==
               0) {
      throw ParquetException(
          "Encrypted parquet file. "
          "Should build with parquet encryption support.");
    }
#endif

#if PARQUET_ENCRYPTION
    // No encryption or encryption with plaintext footer mode.
    if (memcmp(footer_buffer->data() + footer_read_size - 4, kParquetMagic, 4) == 0) {
      std::shared_ptr<Buffer> metadata_buffer;
      uint32_t metadata_len, read_metadata_len;
      ParseUnencryptedFileMetadata(footer_buffer, footer_read_size, file_size,
                                   &metadata_buffer, &metadata_len, &read_metadata_len);

      auto file_decryption_properties = properties_.file_decryption_properties();
      if (!file_metadata_->is_encryption_algorithm_set()) {  // Non encrypted file.
        if (file_decryption_properties != NULLPTR) {
          if (!file_decryption_properties->plaintext_files_allowed()) {
            throw ParquetException("Applying decryption properties on plaintext file");
          }
        }
      } else {
        // Encrypted file with plaintext footer mode.
        ParseMetaDataOfEncryptedFileWithPlaintextFooter(
            file_decryption_properties, metadata_buffer, metadata_len, read_metadata_len);
      }
    } else {
      // Encrypted file with Encrypted footer.
      ParseMetaDataOfEncryptedFileWithEncryptedFooter(footer_buffer, footer_read_size,
                                                      file_size);
    }
#else  // not defined PARQUET_ENCRYPTION
    std::shared_ptr<Buffer> metadata_buffer;
    uint32_t metadata_len, read_metadata_len;
    ParseUnencryptedFileMetadata(footer_buffer, footer_read_size, file_size,
                                 &metadata_buffer, &metadata_len, &read_metadata_len);
#endif
  }

 private:
  std::shared_ptr<ArrowInputFile> source_;
  std::shared_ptr<FileMetaData> file_metadata_;
  ReaderProperties properties_;

#ifdef PARQUET_ENCRYPTION
  std::unique_ptr<InternalFileDecryptor> file_decryptor_;
#endif

  void ParseUnencryptedFileMetadata(const std::shared_ptr<Buffer>& footer_buffer,
                                    int64_t footer_read_size, int64_t file_size,
                                    std::shared_ptr<Buffer>* metadata_buffer,
                                    uint32_t* metadata_len, uint32_t* read_metadata_len);

#if PARQUET_ENCRYPTION
  std::string HandleAadPrefix(FileDecryptionProperties* file_decryption_properties,
                              EncryptionAlgorithm& algo);

  void ParseMetaDataOfEncryptedFileWithPlaintextFooter(
      FileDecryptionProperties* file_decryption_properties,
      const std::shared_ptr<Buffer>& metadata_buffer, uint32_t metadata_len,
      uint32_t read_metadata_len);

  void ParseMetaDataOfEncryptedFileWithEncryptedFooter(
      const std::shared_ptr<Buffer>& footer_buffer, int64_t footer_read_size,
      int64_t file_size);
#endif
};

void SerializedFile::ParseUnencryptedFileMetadata(
    const std::shared_ptr<Buffer>& footer_buffer, int64_t footer_read_size,
    int64_t file_size, std::shared_ptr<Buffer>* metadata_buffer, uint32_t* metadata_len,
    uint32_t* read_metadata_len) {
  *metadata_len = arrow::util::SafeLoadAs<uint32_t>(
      reinterpret_cast<const uint8_t*>(footer_buffer->data()) + footer_read_size -
      kFooterSize);
  int64_t metadata_start = file_size - kFooterSize - *metadata_len;
  if (kFooterSize + *metadata_len > file_size) {
    throw ParquetException(
        "Invalid parquet file. File is less than "
        "file metadata size.");
  }

  // Check if the footer_buffer contains the entire metadata
  if (footer_read_size >= (*metadata_len + kFooterSize)) {
    *metadata_buffer = SliceBuffer(
        footer_buffer, footer_read_size - *metadata_len - kFooterSize, *metadata_len);
  } else {
    PARQUET_THROW_NOT_OK(source_->ReadAt(metadata_start, *metadata_len, metadata_buffer));
    if ((*metadata_buffer)->size() != *metadata_len) {
      throw ParquetException("Invalid parquet file. Could not read metadata bytes.");
    }
  }

  *read_metadata_len = *metadata_len;
  file_metadata_ = FileMetaData::Make((*metadata_buffer)->data(), read_metadata_len);
}

#ifdef PARQUET_ENCRYPTION
void SerializedFile::ParseMetaDataOfEncryptedFileWithEncryptedFooter(
    const std::shared_ptr<Buffer>& footer_buffer, int64_t footer_read_size,
    int64_t file_size) {
  // encryption with encrypted footer
  // both metadata & crypto metadata length
  uint32_t footer_len = arrow::util::SafeLoadAs<uint32_t>(
      reinterpret_cast<const uint8_t*>(footer_buffer->data()) + footer_read_size -
      kFooterSize);
  int64_t crypto_metadata_start = file_size - kFooterSize - footer_len;
  if (kFooterSize + footer_len > file_size) {
    throw ParquetException(
        "Invalid parquet file. File is less than "
        "file metadata size.");
  }
  std::shared_ptr<Buffer> crypto_metadata_buffer;
  // Check if the footer_buffer contains the entire metadata
  if (footer_read_size >= (footer_len + kFooterSize)) {
    crypto_metadata_buffer = SliceBuffer(
        footer_buffer, footer_read_size - footer_len - kFooterSize, footer_len);
  } else {
    PARQUET_THROW_NOT_OK(
        source_->ReadAt(crypto_metadata_start, footer_len, &crypto_metadata_buffer));
    if (crypto_metadata_buffer->size() != footer_len) {
      throw ParquetException("Invalid parquet file. Could not read metadata bytes.");
    }
  }
  auto file_decryption_properties = properties_.file_decryption_properties();
  if (file_decryption_properties == nullptr) {
    throw ParquetException(
        "No decryption properties are provided. Could not read "
        "encrypted footer metadata");
  }
  uint32_t crypto_metadata_len = footer_len;
  std::shared_ptr<FileCryptoMetaData> file_crypto_metadata =
      FileCryptoMetaData::Make(crypto_metadata_buffer->data(), &crypto_metadata_len);
  // Handle AAD prefix
  EncryptionAlgorithm algo = file_crypto_metadata->encryption_algorithm();
  std::string file_aad = HandleAadPrefix(file_decryption_properties, algo);
  file_decryptor_.reset(new InternalFileDecryptor(
      file_decryption_properties, file_aad, algo.algorithm,
      file_crypto_metadata->key_metadata(), properties_.memory_pool()));
  int64_t metadata_offset = file_size - kFooterSize - footer_len + crypto_metadata_len;
  uint32_t metadata_len = footer_len - crypto_metadata_len;
  std::shared_ptr<Buffer> metadata_buffer;
  PARQUET_THROW_NOT_OK(source_->ReadAt(metadata_offset, metadata_len, &metadata_buffer));
  if (metadata_buffer->size() != metadata_len) {
    throw ParquetException(
        "Invalid encrypted parquet file. "
        "Could not read footer metadata bytes.");
  }

  auto footer_decryptor = file_decryptor_->GetFooterDecryptor();
  file_metadata_ =
      FileMetaData::Make(metadata_buffer->data(), &metadata_len, footer_decryptor);
}

void SerializedFile::ParseMetaDataOfEncryptedFileWithPlaintextFooter(
    FileDecryptionProperties* file_decryption_properties,
    const std::shared_ptr<Buffer>& metadata_buffer, uint32_t metadata_len,
    uint32_t read_metadata_len) {
  // Providing decryption properties in plaintext footer mode is not mendatory, for
  // example when reading by legacy reader.
  if (file_decryption_properties != NULLPTR) {
    EncryptionAlgorithm algo = file_metadata_->encryption_algorithm();
    // Handle AAD prefix
    std::string file_aad = HandleAadPrefix(file_decryption_properties, algo);
    file_decryptor_.reset(new InternalFileDecryptor(
        file_decryption_properties, file_aad, algo.algorithm,
        file_metadata_->footer_signing_key_metadata(), properties_.memory_pool()));

    if (file_decryption_properties->check_plaintext_footer_integrity()) {
      if (metadata_len - read_metadata_len != 28) {
        throw ParquetException(
            "Invalid parquet file. Cannot verify plaintext mode footer.");
      }

      if (!file_metadata_->VerifySignature(file_decryptor_.get(),
                                           metadata_buffer->data() + read_metadata_len)) {
        throw ParquetException(
            "Invalid parquet file. Could not verify plaintext "
            "footer metadata");
      }
    }
  }
}

std::string SerializedFile::HandleAadPrefix(
    FileDecryptionProperties* file_decryption_properties, EncryptionAlgorithm& algo) {
  std::string aad_prefix_in_properties = file_decryption_properties->aad_prefix();
  std::string aad_prefix = aad_prefix_in_properties;
  bool file_has_aad_prefix = algo.aad.aad_prefix.empty() ? false : true;
  std::string aad_prefix_in_file = algo.aad.aad_prefix;

  if (algo.aad.supply_aad_prefix && aad_prefix_in_properties.empty()) {
    throw ParquetException(
        "AAD prefix used for file encryption, "
        "but not stored in file and not supplied "
        "in decryption properties");
  }

  if (file_has_aad_prefix) {
    if (!aad_prefix_in_properties.empty()) {
      if (aad_prefix_in_properties.compare(aad_prefix_in_file) != 0) {
        throw ParquetException(
            "AAD Prefix in file and in properties "
            "is not the same");
      }
    }
    aad_prefix = aad_prefix_in_file;
    std::shared_ptr<AADPrefixVerifier> aad_prefix_verifier =
        file_decryption_properties->aad_prefix_verifier();
    if (aad_prefix_verifier != NULLPTR) aad_prefix_verifier->Verify(aad_prefix);
  } else {
    if (!algo.aad.supply_aad_prefix && !aad_prefix_in_properties.empty()) {
      throw ParquetException(
          "AAD Prefix set in decryption properties, but was not used "
          "for file encryption");
    }
    std::shared_ptr<AADPrefixVerifier> aad_prefix_verifier =
        file_decryption_properties->aad_prefix_verifier();
    if (aad_prefix_verifier != NULLPTR) {
      throw ParquetException(
          "AAD Prefix Verifier is set, but AAD Prefix not found in file");
    }
  }
  return aad_prefix + algo.aad.aad_file_unique;
}
#endif  // PARQUET_ENCRYPTION

// ----------------------------------------------------------------------
// ParquetFileReader public API

ParquetFileReader::ParquetFileReader() {}

ParquetFileReader::~ParquetFileReader() {
  try {
    Close();
  } catch (...) {
  }
}

// Open the file. If no metadata is passed, it is parsed from the footer of
// the file
std::unique_ptr<ParquetFileReader::Contents> ParquetFileReader::Contents::Open(
    const std::shared_ptr<ArrowInputFile>& source, const ReaderProperties& props,
    const std::shared_ptr<FileMetaData>& metadata) {
  std::unique_ptr<ParquetFileReader::Contents> result(new SerializedFile(source, props));

  // Access private methods here, but otherwise unavailable
  SerializedFile* file = static_cast<SerializedFile*>(result.get());

  if (metadata == nullptr) {
    // Validates magic bytes, parses metadata, and initializes the SchemaDescriptor
    file->ParseMetaData();
  } else {
    file->set_metadata(metadata);
  }

  return result;
}

std::unique_ptr<ParquetFileReader> ParquetFileReader::Open(
    const std::shared_ptr<::arrow::io::RandomAccessFile>& source,
    const ReaderProperties& props, const std::shared_ptr<FileMetaData>& metadata) {
  auto contents = SerializedFile::Open(source, props, metadata);
  std::unique_ptr<ParquetFileReader> result(new ParquetFileReader());
  result->Open(std::move(contents));
  return result;
}

std::unique_ptr<ParquetFileReader> ParquetFileReader::Open(
    std::unique_ptr<RandomAccessSource> source, const ReaderProperties& props,
    const std::shared_ptr<FileMetaData>& metadata) {
  auto wrapper = std::make_shared<ParquetInputWrapper>(std::move(source));
  return Open(wrapper, props, metadata);
}

std::unique_ptr<ParquetFileReader> ParquetFileReader::OpenFile(
    const std::string& path, bool memory_map, const ReaderProperties& props,
    const std::shared_ptr<FileMetaData>& metadata) {
  std::shared_ptr<::arrow::io::RandomAccessFile> source;
  if (memory_map) {
    std::shared_ptr<::arrow::io::MemoryMappedFile> handle;
    PARQUET_THROW_NOT_OK(
        ::arrow::io::MemoryMappedFile::Open(path, ::arrow::io::FileMode::READ, &handle));
    source = handle;
  } else {
    std::shared_ptr<::arrow::io::ReadableFile> handle;
    PARQUET_THROW_NOT_OK(
        ::arrow::io::ReadableFile::Open(path, props.memory_pool(), &handle));
    source = handle;
  }

  return Open(source, props, metadata);
}

void ParquetFileReader::Open(std::unique_ptr<ParquetFileReader::Contents> contents) {
  contents_ = std::move(contents);
}

void ParquetFileReader::Close() {
  if (contents_) {
    contents_->Close();
  }
}

std::shared_ptr<FileMetaData> ParquetFileReader::metadata() const {
  return contents_->metadata();
}

std::shared_ptr<RowGroupReader> ParquetFileReader::RowGroup(int i) {
  DCHECK(i < metadata()->num_row_groups())
      << "The file only has " << metadata()->num_row_groups()
      << "row groups, requested reader for: " << i;

  return contents_->GetRowGroup(i);
}

// ----------------------------------------------------------------------
// File metadata helpers

std::shared_ptr<FileMetaData> ReadMetaData(
    const std::shared_ptr<::arrow::io::RandomAccessFile>& source) {
  return ParquetFileReader::Open(source)->metadata();
}

// ----------------------------------------------------------------------
// File scanner for performance testing

int64_t ScanFileContents(std::vector<int> columns, const int32_t column_batch_size,
                         ParquetFileReader* reader) {
  std::vector<int16_t> rep_levels(column_batch_size);
  std::vector<int16_t> def_levels(column_batch_size);

  int num_columns = static_cast<int>(columns.size());

  // columns are not specified explicitly. Add all columns
  if (columns.size() == 0) {
    num_columns = reader->metadata()->num_columns();
    columns.resize(num_columns);
    for (int i = 0; i < num_columns; i++) {
      columns[i] = i;
    }
  }

  std::vector<int64_t> total_rows(num_columns, 0);

  for (int r = 0; r < reader->metadata()->num_row_groups(); ++r) {
    auto group_reader = reader->RowGroup(r);
    int col = 0;
    for (auto i : columns) {
      std::shared_ptr<ColumnReader> col_reader = group_reader->Column(i);
      size_t value_byte_size = GetTypeByteSize(col_reader->descr()->physical_type());
      std::vector<uint8_t> values(column_batch_size * value_byte_size);

      int64_t values_read = 0;
      while (col_reader->HasNext()) {
        int64_t levels_read =
            ScanAllValues(column_batch_size, def_levels.data(), rep_levels.data(),
                          values.data(), &values_read, col_reader.get());
        if (col_reader->descr()->max_repetition_level() > 0) {
          for (int64_t i = 0; i < levels_read; i++) {
            if (rep_levels[i] == 0) {
              total_rows[col]++;
            }
          }
        } else {
          total_rows[col] += levels_read;
        }
      }
      col++;
    }
  }

  for (int i = 1; i < num_columns; ++i) {
    if (total_rows[0] != total_rows[i]) {
      throw ParquetException("Parquet error: Total rows among columns do not match");
    }
  }

  return total_rows[0];
}

}  // namespace parquet
