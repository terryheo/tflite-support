/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow_lite_support/cc/task/processor/text_preprocessor.h"

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/strings/ascii.h"  // from @com_google_absl
#include "tensorflow_lite_support/cc/common.h"
#include "tensorflow_lite_support/cc/port/status_macros.h"
#include "tensorflow_lite_support/cc/task/core/task_utils.h"
#include "tensorflow_lite_support/cc/text/tokenizers/regex_tokenizer.h"
#include "tensorflow_lite_support/cc/text/tokenizers/tokenizer_utils.h"
#include "tensorflow_lite_support/cc/utils/common_utils.h"

namespace tflite {
namespace task {
namespace processor {

namespace {

using ::absl::StatusCode;
using ::tflite::TensorMetadata;
using ::tflite::support::CreateStatusWithPayload;
using ::tflite::support::StatusOr;
using ::tflite::support::TfLiteSupportStatus;
using ::tflite::support::text::tokenizer::CreateTokenizerFromProcessUnit;
using ::tflite::support::text::tokenizer::RegexTokenizer;
using ::tflite::support::text::tokenizer::TokenizerResult;
using ::tflite::task::core::FindIndexByMetadataTensorName;
using ::tflite::task::core::PopulateTensor;

constexpr int kTokenizerProcessUnitIndex = 0;
constexpr char kIdsTensorName[] = "ids";
constexpr char kMaskTensorName[] = "mask";
constexpr char kSegmentIdsTensorName[] = "segment_ids";
constexpr char kClassificationToken[] = "[CLS]";
constexpr char kSeparator[] = "[SEP]";

}  // namespace

/* static */ StatusOr<std::unique_ptr<TextPreprocessor>>
TextPreprocessor::Create(
    tflite::task::core::TfLiteEngine* engine,
    const std::initializer_list<int> input_tensor_indices) {
  if (input_tensor_indices.size() != 1 && input_tensor_indices.size() != 3) {
    return CreateStatusWithPayload(
        StatusCode::kInvalidArgument,
        absl::StrFormat(
            "TextPrerocessor accepts either 1 input tesnor (for Regex "
            "tokenizer or String tensor) or 3 input tensors (for Bert "
            "tokenizer), but got %d tensors.",
            input_tensor_indices.size()));
  }

  ASSIGN_OR_RETURN(auto processor,
                   Processor::Create<TextPreprocessor>(
                       /* num_expected_tensors = */ input_tensor_indices.size(),
                       engine, input_tensor_indices,
                       /* requires_metadata = */ false));
  RETURN_IF_ERROR(processor->Init());
  return processor;
}

absl::Status TextPreprocessor::Preprocess(const std::string& input_text) {
  switch (tokenzier_type_) {
    case TokenizerType::kNone:
      return PopulateTensor(input_text, GetTensor());
    case TokenizerType::kRegex:
      return RegexPreprocess(input_text);
    case TokenizerType::kBert:
      return BertPreprocess(input_text);
    default:
      break;
  }
  // Should never happen.
  return CreateStatusWithPayload(
      StatusCode::kInternal,
      absl::StrCat("The tokenzier type is unsupported: ", tokenzier_type_));
}

absl::Status TextPreprocessor::Init() {
  const tflite::ProcessUnit* tokenzier_metadata;
  switch (tensor_indices_.size()) {
    // One input text tensor: regular text input.
    case 1: {
      // Check if the input is a STRING. If so, no tokenizer is needed.
      if (GetTensor()->type == kTfLiteString) {
        tokenzier_type_ = TokenizerType::kNone;
        return absl::OkStatus();
      }
      // Try if RegexTokenzier metadata can be found.
      ASSIGN_OR_RETURN(tokenzier_metadata, TryFindRegexTokenizerMetadata());
      tokenzier_type_ = TokenizerType::kRegex;
      break;
    }
    // Three input tensors: bert models.
    case 3: {
      // Try if RegexTokenzier can be found.
      // BertTokenzier is packed in the processing unit of the InputTensors in
      // SubgraphMetadata.
      tokenzier_metadata = GetMetadataExtractor()->GetInputProcessUnit(
          kTokenizerProcessUnitIndex);
      tokenzier_type_ = TokenizerType::kBert;
      // Identify the tensor index for three Bert input tensors.
      auto tensors_metadata = GetMetadataExtractor()->GetInputTensorMetadata();
      int ids_tensor_index =
          FindIndexByMetadataTensorName(tensors_metadata, kIdsTensorName);
      ids_tensor_index_ =
          ids_tensor_index == -1 ? tensor_indices_[0] : ids_tensor_index;
      int mask_tensor_index =
          FindIndexByMetadataTensorName(tensors_metadata, kMaskTensorName);
      mask_tensor_index_ =
          mask_tensor_index == -1 ? tensor_indices_[1] : mask_tensor_index;
      int segment_ids_tensor_index = FindIndexByMetadataTensorName(
          tensors_metadata, kSegmentIdsTensorName);
      segment_ids_tensor_index_ = segment_ids_tensor_index == -1
                                      ? tensor_indices_[2]
                                      : segment_ids_tensor_index;

      if (GetLastDimSize(ids_tensor_index_) !=
              GetLastDimSize(mask_tensor_index_) ||
          GetLastDimSize(ids_tensor_index_) !=
              GetLastDimSize(segment_ids_tensor_index_)) {
        return CreateStatusWithPayload(
            absl::StatusCode::kInternal,
            absl::StrFormat("The three input tensors in Bert models are "
                            "expected to have same length, but got ids_tensor "
                            "(%d), mask_tensor (%d), segment_ids_tensor (%d).",
                            GetLastDimSize(ids_tensor_index_),
                            GetLastDimSize(mask_tensor_index_),
                            GetLastDimSize(segment_ids_tensor_index_)),
            TfLiteSupportStatus::kInvalidNumOutputTensorsError);
      }
      bert_max_seq_len_ = GetLastDimSize(ids_tensor_index_);
      break;
    }
    default:
      // Should not happen, because Create() already checks it.
      return CreateStatusWithPayload(
          StatusCode::kInvalidArgument,
          absl::StrFormat(
              "TextPrerocessor accepts either 1 input tesnor (for Regex "
              "tokenizer or String tensor) or 3 input tensors (for Bert "
              "tokenizer), but got %d tensors.",
              tensor_indices_.size()));
  }

  ASSIGN_OR_RETURN(tokenizer_, CreateTokenizerFromProcessUnit(
                                   tokenzier_metadata, GetMetadataExtractor()));
  return absl::OkStatus();
}

absl::Status TextPreprocessor::BertPreprocess(const std::string& input_text) {
  auto* ids_tensor =
      engine_->GetInput(engine_->interpreter(), ids_tensor_index_);
  auto* mask_tensor =
      engine_->GetInput(engine_->interpreter(), mask_tensor_index_);
  auto* segment_ids_tensor =
      engine_->GetInput(engine_->interpreter(), segment_ids_tensor_index_);

  std::string processed_input = input_text;
  absl::AsciiStrToLower(&processed_input);

  TokenizerResult input_tokenize_results;
  input_tokenize_results = tokenizer_->Tokenize(processed_input);

  // 2 accounts for [CLS], [SEP]
  absl::Span<const std::string> query_tokens =
      absl::MakeSpan(input_tokenize_results.subwords.data(),
                     input_tokenize_results.subwords.data() +
                         std::min(static_cast<size_t>(bert_max_seq_len_ - 2),
                                  input_tokenize_results.subwords.size()));

  std::vector<std::string> tokens;
  tokens.reserve(2 + query_tokens.size());
  // Start of generating the features.
  tokens.push_back(kClassificationToken);
  // For query input.
  for (const auto& query_token : query_tokens) {
    tokens.push_back(query_token);
  }
  // For Separation.
  tokens.push_back(kSeparator);

  std::vector<int> input_ids(bert_max_seq_len_, 0);
  std::vector<int> input_mask(bert_max_seq_len_, 0);
  // Convert tokens back into ids and set mask
  for (int i = 0; i < tokens.size(); ++i) {
    tokenizer_->LookupId(tokens[i], &input_ids[i]);
    input_mask[i] = 1;
  }
  //                           |<--------bert_max_seq_len_--------->|
  // input_ids                 [CLS] s1  s2...  sn [SEP]  0  0...  0
  // input_masks                 1    1   1...  1    1    0  0...  0
  // segment_ids                 0    0   0...  0    0    0  0...  0

  RETURN_IF_ERROR(PopulateTensor(input_ids, ids_tensor));
  RETURN_IF_ERROR(PopulateTensor(input_mask, mask_tensor));
  RETURN_IF_ERROR(PopulateTensor(std::vector<int>(bert_max_seq_len_, 0),
                                 segment_ids_tensor));
  return absl::OkStatus();
}

absl::Status TextPreprocessor::RegexPreprocess(const std::string& input_text) {
  TfLiteTensor* input_tensor = GetTensor();
  auto regex_tokenizer = std::unique_ptr<RegexTokenizer>(
      dynamic_cast<RegexTokenizer*>(tokenizer_.release()));

  //                              |<-------sentence_length-------->|
  // input_tensor                 <START>, t1, t2... <PAD>, <PAD>...
  // <START> is optional, t1, t2... will be replaced by <UNKNOWN> if it's
  // not found in tokenizer vocab.
  TokenizerResult result = regex_tokenizer->Tokenize(input_text);

  size_t max_sentence_length = input_tensor->dims->size == 2
                                   ? input_tensor->dims->data[1]
                                   : input_tensor->dims->data[0];

  int unknown_token_id = 0;
  regex_tokenizer->GetUnknownToken(&unknown_token_id);

  int pad_token_id = 0;
  regex_tokenizer->GetPadToken(&pad_token_id);

  std::vector<int> input_tokens(max_sentence_length, pad_token_id);
  int start_token_id = 0;
  size_t input_token_index = 0;
  if (regex_tokenizer->GetStartToken(&start_token_id)) {
    input_tokens[0] = start_token_id;
    input_token_index = 1;
  }

  for (size_t i = 0; (i < result.subwords.size()) &&
                     (input_token_index < max_sentence_length);
       ++i, ++input_token_index) {
    const std::string& token = result.subwords[i];
    int token_id = 0;
    if (regex_tokenizer->LookupId(token, &token_id)) {
      input_tokens[input_token_index] = token_id;
    } else {
      input_tokens[input_token_index] = unknown_token_id;
    }
  }
  return PopulateTensor(input_tokens, input_tensor);
}

StatusOr<const tflite::ProcessUnit*>
TextPreprocessor::TryFindRegexTokenizerMetadata() {
  // RegexTokenizer is packed in the processing unit of the input tensor.
  const TensorMetadata* tensor_metadata = GetTensorMetadata();
  if (tensor_metadata == nullptr) {
    return nullptr;
  }

  ASSIGN_OR_RETURN(
      auto tokenizer_metadata,
      GetMetadataExtractor()->FindFirstProcessUnit(
          *tensor_metadata, ProcessUnitOptions_RegexTokenizerOptions));

  if (tokenizer_metadata != nullptr) {
    // RegexTokenizer is found. Check if the tensor type matches.
    auto input_tensor = GetTensor();
    if (input_tensor->type != kTfLiteInt32) {
      return CreateStatusWithPayload(
          StatusCode::kInvalidArgument,
          absl::StrCat("Type mismatch for input tensor ", input_tensor->name,
                       ". Requested INT32 for RegexTokenizer, got ",
                       TfLiteTypeGetName(input_tensor->type), "."),
          TfLiteSupportStatus::kInvalidInputTensorTypeError);
    }
  }
  return tokenizer_metadata;
}

int TextPreprocessor::GetLastDimSize(int tensor_index) {
  auto tensor = engine_->GetInput(engine_->interpreter(), tensor_index);
  return tensor->dims->data[tensor->dims->size - 1];
}

}  // namespace processor
}  // namespace task
}  // namespace tflite
