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

// This module defines an abstract interface for iterating through pages in a
// Parquet column chunk within a row group. It could be extended in the future
// to iterate through all data pages in all chunks in a file.

#pragma once

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "arrow/testing/util.h"

#include "parquet/column_page.h"
#include "parquet/column_reader.h"
#include "parquet/column_writer.h"
#include "parquet/encoding.h"
#include "parquet/platform.h"

namespace parquet {
namespace test {

std::string data_file(const char* file); 

using FileClass = ::arrow::io::FileOutputStream;

using parquet::ConvertedType;
using parquet::Repetition;
using parquet::Type;
using schema::GroupNode;
using schema::NodePtr;
using schema::PrimitiveNode;

constexpr int kFixedLength = 10;

const char kFooterEncryptionKey[] = "0123456789012345";  // 128bit/16
const char kColumnEncryptionKey1[] = "1234567890123450";
const char kColumnEncryptionKey2[] = "1234567890123451";
const char kFileName[] = "tester";


}  // namespace test
}  // namespace parquet
