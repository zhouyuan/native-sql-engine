/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arrow/compute/context.h>
#include <arrow/status.h>
#include <arrow/type.h>
#include <arrow/type_fwd.h>
#include <arrow/type_traits.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <gandiva/configuration.h>
#include <gandiva/node.h>
#include <gandiva/tree_expr_builder.h>
#include <gandiva/projector.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>

#include "array_appender.h"
#include "codegen/arrow_compute/ext/array_item_index.h"
#include "codegen/arrow_compute/ext/code_generator_base.h"
#include "codegen/arrow_compute/ext/codegen_common.h"
#include "codegen/arrow_compute/ext/codegen_node_visitor.h"
#include "codegen/arrow_compute/ext/codegen_register.h"
#include "codegen/arrow_compute/ext/kernels_ext.h"
#include "codegen/arrow_compute/ext/typed_node_visitor.h"
#include "utils/macros.h"

namespace sparkcolumnarplugin {
namespace codegen {
namespace arrowcompute {
namespace extra {

using ArrayList = std::vector<std::shared_ptr<arrow::Array>>;

///////////////Operator  SMJ  ////////////////
class ConditionedJoinKernel::Impl {
 public:
  Impl() {}
  Impl(arrow::compute::FunctionContext* ctx,
       const gandiva::NodeVector& left_key_node_list,
       const gandiva::NodeVector& right_key_node_list,
       const gandiva::NodeVector& left_schema_node_list,
       const gandiva::NodeVector& right_schema_node_list,
       const gandiva::NodePtr& condition, int join_type,
       const gandiva::NodeVector& result_node_list,
       const gandiva::NodeVector& hash_configuration_list, int hash_relation_idx): ctx_(ctx),
        join_type_(join_type), condition_(condition) {
       }
  virtual ~Impl() {}
  virtual arrow::Status Evaluate(const ArrayList& in) {
    return arrow::Status::OK();
  }

  std::string GetSignature() { return ""; }

  virtual arrow::Status MakeResultIterator(
      std::shared_ptr<arrow::Schema> schema,
      std::shared_ptr<ResultIterator<arrow::RecordBatch>>* out) {
    return arrow::Status::OK();
  }

   arrow::Status DoCodeGen(
      int level,
      std::vector<std::pair<std::pair<std::string, std::string>, gandiva::DataTypePtr>>
          input,
      std::shared_ptr<CodeGenContext>* codegen_ctx_out, int* var_id) {
    return arrow::Status::OK();
  }

 private:
  arrow::compute::FunctionContext* ctx_;
  arrow::MemoryPool* pool_;
  std::string signature_;
  int join_type_;
  gandiva::NodePtr condition_;

};

arrow::Status ConditionedJoinKernel::Make(
    arrow::compute::FunctionContext* ctx, const gandiva::NodeVector& left_key_list,
    const gandiva::NodeVector& right_key_list,
    const gandiva::NodeVector& left_schema_list,
    const gandiva::NodeVector& right_schema_list, const gandiva::NodePtr& condition,
    int join_type, const gandiva::NodeVector& result_schema,
    const gandiva::NodeVector& hash_configuration_list, int hash_relation_idx,
    std::shared_ptr<KernalBase>* out) {
  *out = std::make_shared<ConditionedJoinKernel>(
      ctx, left_key_list, right_key_list, left_schema_list, right_schema_list, condition,
      join_type, result_schema, hash_configuration_list, hash_relation_idx);
  return arrow::Status::OK();
}

//TODO: implement multiple keys
template<typename... ArrowType>
class TypedJoinKernel : public ConditionedJoinKernel::Impl {
 public:
  TypedJoinKernel(arrow::compute::FunctionContext* ctx,
                  const gandiva::NodeVector& left_key_node_list,
                  const gandiva::NodeVector& right_key_node_list,
                  const gandiva::NodeVector& left_schema_node_list,
                  const gandiva::NodeVector& right_schema_node_list,
                  const gandiva::NodePtr& condition, int join_type,
                  const gandiva::NodeVector& result_node_list,
                  const gandiva::NodeVector& hash_configuration_list,
                  int hash_relation_idx)
      : ctx_(ctx), join_type_(join_type), condition_(condition) {
    for (auto node : left_schema_node_list) {
      left_field_list_.push_back(
          std::dynamic_pointer_cast<gandiva::FieldNode>(node)->field());
    }
    for (auto node : right_schema_node_list) {
      right_field_list_.push_back(
          std::dynamic_pointer_cast<gandiva::FieldNode>(node)->field());
    }
    for (auto node : result_node_list) {
      result_schema_.push_back(
          std::dynamic_pointer_cast<gandiva::FieldNode>(node)->field());
    }
    result_schema = std::make_shared<arrow::Schema>(result_schema_);
    //TODO(): fix with multiple keys
    exist_index_ = 1;
  }

  arrow::Status Evaluate(const ArrayList& in) override {
    col_num_ = left_field_list_.size();

    if (cached_.size() <= col_num_) {
      cached_.resize(col_num_);
    }
    for (int i = 0; i < col_num_; i++) {
      cached_[i].push_back(in[i]);
    }
    //TODO: fix key col
    auto typed_array_0 = std::dynamic_pointer_cast<ArrayType>(in[0]);
    left_list_.emplace_back(typed_array_0);
    idx_to_arrarid_.push_back(typed_array_0->length());
    return arrow::Status::OK();
  }

  std::string GetSignature() { return ""; }

  arrow::Status MakeResultIterator(
      std::shared_ptr<arrow::Schema> schema,
      std::shared_ptr<ResultIterator<arrow::RecordBatch>>* out) override {
    *out = std::make_shared<ProberResultIterator>(
        ctx_, schema, join_type_, &left_list_, &idx_to_arrarid_, cached_, exist_index_);

    return arrow::Status::OK();
  }

  arrow::Status DoCodeGen(
      int level,
      std::vector<std::pair<std::pair<std::string, std::string>, gandiva::DataTypePtr>>
          input,
      std::shared_ptr<CodeGenContext>* codegen_ctx_out, int* var_id) {
    return arrow::Status::OK();
  }

 private:
  using ArrayType = typename arrow::TypeTraits<typename std::tuple_element<0, std::tuple<ArrowType...> >::type>::ArrayType;
  using ArrayType1 = typename arrow::TypeTraits<typename std::tuple_element<1, std::tuple<ArrowType...> >::type>::ArrayType;
  arrow::compute::FunctionContext* ctx_;
  arrow::MemoryPool* pool_;
  std::string signature_;
  int join_type_;
  gandiva::NodePtr condition_;
  gandiva::FieldVector left_field_list_;
  gandiva::FieldVector right_field_list_;
  gandiva::FieldVector result_schema_;
  std::vector<int> right_key_index_list_;
  std::vector<int> left_shuffle_index_list_;
  std::vector<int> right_shuffle_index_list_;
  std::vector<std::shared_ptr<ArrayType>> left_list_;
  std::vector<int64_t> idx_to_arrarid_;
  int col_num_;
  std::vector<arrow::ArrayVector> cached_;
  std::shared_ptr<arrow::Schema> result_schema;
  int exist_index_ = -1;

  class FVector {
    using ViewType = decltype(std::declval<ArrayType>().GetView(0));

   public:
    FVector(std::vector<std::shared_ptr<ArrayType>>* left_list,
            std::vector<int64_t> segment_info) {
      left_list_ = left_list;
      total_len_ = std::accumulate(segment_info.begin(), segment_info.end(), 0);
      segment_info_ = segment_info;
      it = (*left_list_)[0];
      segment_len = 0;
    }

    void getpos(int64_t* cur_idx, int64_t* cur_segmeng_len, int64_t* passlen) {
      *cur_idx = idx;
      *cur_segmeng_len = segment_len;
      *passlen = passed_len;
    }

    void setpos(int64_t cur_idx, int64_t cur_segment_len, int64_t cur_passed_len) {
      idx = cur_idx;
      it = (*left_list_)[idx];
      segment_len = cur_segment_len;
      passed_len = cur_passed_len;
    }

    ViewType value() { return it->GetView(segment_len); }

    bool hasnext() { return (passed_len <= total_len_ - 1); }

    void next() {
      segment_len++;
      if (segment_len == segment_info_[idx] && passed_len != total_len_ - 1) {
        idx++;
        segment_len = 0;
        it = (*left_list_)[idx];
      }
      passed_len++;
    }
    ArrayItemIndex GetArrayItemIdex() {
      return ArrayItemIndex(idx, segment_len);
    }

   public:
    std::vector<std::shared_ptr<ArrayType>>* left_list_;
    int64_t total_len_;
    std::vector<int64_t> segment_info_;

    int64_t idx = 0;
    int64_t passed_len = 0;
    int64_t segment_len = 0;
    std::shared_ptr<ArrayType> it;
  };
  class ProberResultIterator : public ResultIterator<arrow::RecordBatch> {
   public:
    ProberResultIterator(arrow::compute::FunctionContext* ctx,
                         std::shared_ptr<arrow::Schema> schema,
                         int join_type,
                         std::vector<std::shared_ptr<ArrayType>>* left_list,
                         std::vector<int64_t>* idx_to_arrarid,
                         std::vector<arrow::ArrayVector> cached_in,
                         int exist_index)
        : ctx_(ctx),
          result_schema_(schema),
          join_type_(join_type),
          left_list_(left_list),
          last_pos(0),
          idx_to_arrarid_(idx_to_arrarid),
          cached_in_(cached_in), exist_index_(exist_index) {
            
      int result_col_num = schema->num_fields();

      left_result_col_num = cached_in.size();

      if (join_type_ == 2 || join_type_ == 3 || join_type_ == 4) {
        // for semi/anti/existence only return right batch
        left_result_col_num = 0;
      }

      for (int i = 0; i < result_col_num; i++) {
        auto appender_type = AppenderBase::left;
        auto field = schema->field(i);
        auto type = field->type();
        if (left_result_col_num == 0 || i >= left_result_col_num) {
          appender_type = AppenderBase::right;
        }
        if (i == exist_index_) {
          appender_type = AppenderBase::exist;
          type = arrow::boolean();
        }
        std::cout << "adding : " << type->ToString() << std::endl;
        std::shared_ptr<AppenderBase> appender;
        MakeAppender(ctx_, type, appender_type, &appender);
        appender_list_.push_back(appender);
      }

      for (int i = 0; i < left_result_col_num; i++) {
        arrow::ArrayVector array_vector = cached_in_[i];
        int array_num = array_vector.size();
        for (int array_id = 0; array_id < array_num; array_id++) {
          auto arr = array_vector[array_id];
          appender_list_[i]->AddArray(arr);
        }
      }
      left_it = std::make_shared<FVector>(left_list_, *idx_to_arrarid_);

      switch (join_type_) {
          case 0: { /*Inner Join*/
            auto func = std::make_shared<InnerProbeFunction>(left_it, appender_list_);
            probe_func_ = std::dynamic_pointer_cast<ProbeFunctionBase>(func);
          } break;
          case 1: { /*Outer Join*/
            auto func = std::make_shared<OuterProbeFunction>(left_it, appender_list_);
            probe_func_ = std::dynamic_pointer_cast<ProbeFunctionBase>(func);
          } break;
          case 2: { /*Anti Join*/
            auto func =
                std::make_shared<AntiProbeFunction>(left_it, appender_list_);
            probe_func_ = std::dynamic_pointer_cast<ProbeFunctionBase>(func);
          } break;
          case 3: { /*Semi Join*/
            auto func =
                std::make_shared<SemiProbeFunction>(left_it, appender_list_);
            probe_func_ = std::dynamic_pointer_cast<ProbeFunctionBase>(func);
          } break;
          case 4: { /*Existence Join*/
            auto func = std::make_shared<ExistenceProbeFunction>(left_it, appender_list_);
            probe_func_ = std::dynamic_pointer_cast<ProbeFunctionBase>(func);
          } break;
          default:
            throw;
        }
    }

    arrow::Status Process(const ArrayList& in, std::shared_ptr<arrow::RecordBatch>* out,
                          const std::shared_ptr<arrow::Array>& selection) override {

      std::shared_ptr<arrow::Array> key_array = in[0];
      arrow::ArrayVector projected_keys_outputs;

      //update right appender
      if (join_type_ == 4) {
        // do not touch the middle existence column
        for (int i = 0; i < appender_list_.size(); i++) {
          if (i == exist_index_) {
            continue;
          }
          if (i > exist_index_) {
            appender_list_[i]->AddArray(in[i-1]);
          } else {
            appender_list_[i]->AddArray(in[i]);
          }
        }
      } else {
        for (int i = 0; i < in.size(); i++) {
          appender_list_[i + left_result_col_num]->AddArray(in[i]);
        }
      }

      uint64_t out_length = 0;
      out_length = probe_func_->Evaluate(key_array);

      ArrayList arrays;
      for (auto& appender : appender_list_) {
        std::shared_ptr<arrow::Array> out_array;
        RETURN_NOT_OK(appender->Finish(&out_array));
        arrays.push_back(out_array);
        if (appender->GetType() == AppenderBase::right) {
          RETURN_NOT_OK(appender->PopArray());
        }
        appender->Reset();
      }

      *out = arrow::RecordBatch::Make(result_schema_, out_length, arrays);
      return arrow::Status::OK();
    }

   private:
    class ProbeFunctionBase {
     public:
      virtual ~ProbeFunctionBase() {}
      virtual uint64_t Evaluate(std::shared_ptr<arrow::Array>) { return 0; }
      virtual uint64_t Evaluate(std::shared_ptr<arrow::Array>,
                                const arrow::ArrayVector&) {
        return 0;
      }
    };
    class InnerProbeFunction : public ProbeFunctionBase {
      public:
        InnerProbeFunction(std::shared_ptr<FVector> left_it,
                           std::vector<std::shared_ptr<AppenderBase>> appender_list):
        left_it(left_it), appender_list_(appender_list) {}

        uint64_t Evaluate(std::shared_ptr<arrow::Array> key_array) override {
        auto typed_key_array = std::dynamic_pointer_cast<ArrayType>(key_array);

        uint64_t out_length = 0;
        for (int i = 0; i < key_array->length(); i++) {
          auto right_content = typed_key_array->GetView(i);
          while (left_it->hasnext() && left_it->value() < right_content) {
            left_it->next();
          }
          int64_t cur_idx, seg_len, pl; left_it->getpos(&cur_idx, &seg_len, &pl);
          while (left_it->hasnext() && left_it->value() == right_content) {
            auto item = left_it->GetArrayItemIdex();
            for (auto& appender : appender_list_) {
              if (appender->GetType() == AppenderBase::left) {
                appender->Append(item.array_id, item.id);
              } else {
                appender->Append(0, i);
              }
            }
            out_length += 1;
            left_it->next();
          }
          left_it->setpos(cur_idx, seg_len, pl);
        }
        return out_length;
      }

      private:
        std::vector<std::shared_ptr<AppenderBase>> appender_list_;
        std::shared_ptr<FVector> left_it;
    };

    class OuterProbeFunction : public ProbeFunctionBase {
      public:
        OuterProbeFunction(std::shared_ptr<FVector> left_it, std::vector<std::shared_ptr<AppenderBase>> appender_list): left_it(left_it), appender_list_(appender_list){
        }
        uint64_t Evaluate(std::shared_ptr<arrow::Array> key_array) override {
        auto typed_key_array = std::dynamic_pointer_cast<ArrayType>(key_array);

        uint64_t out_length = 0;
        int last_match_idx = -1;
        for (int i = 0; i < key_array->length(); i++) {
          auto right_content = typed_key_array->GetView(i);
          while (left_it->hasnext() && left_it->value() < right_content) {
            left_it->next();
          }
          int64_t cur_idx, seg_len, pl; left_it->getpos(&cur_idx, &seg_len, &pl);
          while (left_it->hasnext() && left_it->value() == right_content) {
            auto item = left_it->GetArrayItemIdex();
            for (auto& appender : appender_list_) {
              if (appender->GetType() == AppenderBase::left) {
                appender->Append(item.array_id, item.id);
              } else {
                appender->Append(0, i);
              }
            }
            last_match_idx = i;
            out_length += 1;
            left_it->next();
          }
          left_it->setpos(cur_idx, seg_len, pl);
          if(left_it->value() > right_content && left_it->hasnext() ) {
            if (last_match_idx == i) {
              continue;
            }
            for (auto& appender : appender_list_) {
              if (appender->GetType() == AppenderBase::left) {
                appender->AppendNull();
              } else {
                appender->Append(0, i);
              }
            }
            out_length += 1;
          }
          if (!left_it->hasnext()) {
            for (auto& appender : appender_list_) {
              if (appender->GetType() == AppenderBase::left) {
                appender->AppendNull();
              } else {
                appender->Append(0, i);
              }
            }
            out_length += 1;
          }
        }
        return out_length;
      }
      private:
        std::vector<std::shared_ptr<AppenderBase>> appender_list_;
        std::shared_ptr<FVector> left_it;
    };

    //TODO(): implement full outer
    class FullOuterProbeFunction : public ProbeFunctionBase {
      public:
        FullOuterProbeFunction(std::shared_ptr<FVector> left_it, std::vector<std::shared_ptr<AppenderBase>> appender_list): left_it(left_it), appender_list_(appender_list){
        }
        uint64_t Evaluate(std::shared_ptr<arrow::Array> key_array) override {
        auto typed_key_array = std::dynamic_pointer_cast<ArrayType>(key_array);

        uint64_t out_length = 0;
        for (int i = 0; i < key_array->length(); i++) {

        }
        return out_length;
      }
      private:
        std::vector<std::shared_ptr<AppenderBase>> appender_list_;
        std::shared_ptr<FVector> left_it;
    };

    class SemiProbeFunction : public ProbeFunctionBase {
      public:
        SemiProbeFunction(std::shared_ptr<FVector> left_it, std::vector<std::shared_ptr<AppenderBase>> appender_list):left_it(left_it), appender_list_(appender_list){
        }
        uint64_t Evaluate(std::shared_ptr<arrow::Array> key_array) override {
        auto typed_key_array = std::dynamic_pointer_cast<ArrayType>(key_array);

        uint64_t out_length = 0;
        for (int i = 0; i < key_array->length(); i++) {
          auto right_content = typed_key_array->GetView(i);
          while (left_it->hasnext() && left_it->value() < right_content) {
            left_it->next();
          }
          int64_t cur_idx, seg_len, pl; left_it->getpos(&cur_idx, &seg_len, &pl);
          if (left_it->value() == right_content && left_it->hasnext()) {
            for (auto& appender : appender_list_) {
              if (appender->GetType() == AppenderBase::right) {
                appender->Append(0, i);
              }
            }
            out_length += 1;
            left_it->next();
          }
          left_it->setpos(cur_idx, seg_len, pl);
        }
        return out_length;
      }
      private:
        std::vector<std::shared_ptr<AppenderBase>> appender_list_;
        std::shared_ptr<FVector> left_it;
    };

    class AntiProbeFunction : public ProbeFunctionBase {
      public:
        AntiProbeFunction(std::shared_ptr<FVector> left_it, std::vector<std::shared_ptr<AppenderBase>> appender_list):left_it(left_it), appender_list_(appender_list){
        }
        uint64_t Evaluate(std::shared_ptr<arrow::Array> key_array) override {
        auto typed_key_array = std::dynamic_pointer_cast<ArrayType>(key_array);

        uint64_t out_length = 0;
        int last_match_idx = -1;
        for (int i = 0; i < key_array->length(); i++) {
          auto right_content = typed_key_array->GetView(i);
          while (left_it->hasnext() && left_it->value() < right_content) {
            left_it->next();
          }
          int64_t cur_idx, seg_len, pl; left_it->getpos(&cur_idx, &seg_len, &pl);
          while (left_it->hasnext() && left_it->value() == right_content) {
            last_match_idx = i;
            left_it->next();
          }
          left_it->setpos(cur_idx, seg_len, pl);
          if (left_it->value() > right_content && left_it->hasnext()) {
            if (last_match_idx == i) {
              continue;
            }
            for (auto& appender : appender_list_) {
              if (appender->GetType() == AppenderBase::right) {
                appender->Append(0, i);
              }
            }
            out_length += 1;
          }
          if (!left_it->hasnext()) {
            for (auto& appender : appender_list_) {
              if (appender->GetType() == AppenderBase::right) {
                appender->Append(0, i);
              }
            }
            out_length += 1;
          }
        }
        return out_length;
      }
      private:
        std::vector<std::shared_ptr<AppenderBase>> appender_list_;
        std::shared_ptr<FVector> left_it;
    };

    class ExistenceProbeFunction : public ProbeFunctionBase {
      public:
        ExistenceProbeFunction(std::shared_ptr<FVector> left_it, std::vector<std::shared_ptr<AppenderBase>> appender_list): left_it(left_it), appender_list_(appender_list){
        }
        uint64_t Evaluate(std::shared_ptr<arrow::Array> key_array) override {
        auto typed_key_array = std::dynamic_pointer_cast<ArrayType>(key_array);

        uint64_t out_length = 0;
        int last_match_idx = -1;
        for (int i = 0; i < key_array->length(); i++) {
          auto right_content = typed_key_array->GetView(i);
          while (left_it->hasnext() && left_it->value() < right_content) {
            left_it->next();
          }
          int64_t cur_idx, seg_len, pl; left_it->getpos(&cur_idx, &seg_len, &pl);
          if (left_it->hasnext() && left_it->value() == right_content) {
                        for (auto& appender : appender_list_) {
              if (appender->GetType() == AppenderBase::right) {
                appender->Append(0, i);
              } else { // should be existence column
                appender->AppendExistence(true);
              }
            }
            out_length += 1;
            last_match_idx = i;
            while (left_it->hasnext() && left_it->value() == right_content) {
               left_it->next();
            }
          }
          left_it->setpos(cur_idx, seg_len, pl);
          if (left_it->value() > right_content && left_it->hasnext()) {
            if (last_match_idx == i) {
              continue;
            }
            for (auto& appender : appender_list_) {
              if (appender->GetType() == AppenderBase::right) {
                appender->Append(0, i);
              } else { // should be existence column
                appender->AppendExistence(false);
              }
            }
            out_length += 1;
          }
          if (!left_it->hasnext()) {
            for (auto& appender : appender_list_) {
              if (appender->GetType() == AppenderBase::right) {
                appender->Append(0, i);
              } else { // should be existence column
                appender->AppendExistence(false);
              }
            }
            out_length += 1;
          }
        }
        return out_length;
      }
      private:
        std::vector<std::shared_ptr<AppenderBase>> appender_list_;
        std::shared_ptr<FVector> left_it;
    };

   private:
    arrow::compute::FunctionContext* ctx_;
    std::shared_ptr<arrow::Schema> result_schema_;
    std::vector<std::shared_ptr<ArrayType>>* left_list_;
    std::shared_ptr<FVector> left_it;
    int64_t last_pos;
    int64_t last_idx = 0;
    int64_t last_seg = 0;
    int64_t last_pl = 0;
    int left_result_col_num = 0;
    int join_type_;
    int exist_index_;
    std::vector<int64_t>* idx_to_arrarid_;
    std::vector<arrow::ArrayVector> cached_in_;
    std::vector<std::shared_ptr<AppenderBase>> appender_list_;
    std::shared_ptr<ProbeFunctionBase> probe_func_;
  };
};

#define PROCESS_SUPPORTED_TYPES(PROCESS) \
  PROCESS(arrow::BooleanType)            \
  PROCESS(arrow::UInt8Type)              \
  PROCESS(arrow::Int8Type)               \
  PROCESS(arrow::UInt16Type)             \
  PROCESS(arrow::Int16Type)              \
  PROCESS(arrow::UInt32Type)             \
  PROCESS(arrow::Int32Type)              \
  PROCESS(arrow::UInt64Type)             \
  PROCESS(arrow::Int64Type)              \
  PROCESS(arrow::FloatType)              \
  PROCESS(arrow::DoubleType)             \
  PROCESS(arrow::Date32Type)             \
  PROCESS(arrow::Date64Type)
ConditionedJoinKernel::ConditionedJoinKernel(
    arrow::compute::FunctionContext* ctx, const gandiva::NodeVector& left_key_list,
    const gandiva::NodeVector& right_key_list,
    const gandiva::NodeVector& left_schema_list,
    const gandiva::NodeVector& right_schema_list, const gandiva::NodePtr& condition,
    int join_type, const gandiva::NodeVector& result_schema,
    const gandiva::NodeVector& hash_configuration_list, int hash_relation_idx) {
  if (left_key_list.size() == 1 && right_key_list.size() == 1) {
    auto key_node = left_key_list[0];
    std::shared_ptr<TypedNodeVisitor> node_visitor;
    std::shared_ptr<gandiva::FieldNode> field_node;
    THROW_NOT_OK(MakeTypedNodeVisitor(key_node, &node_visitor));
    if (node_visitor->GetResultType() == TypedNodeVisitor::FieldNode) {
        node_visitor->GetTypedNode(&field_node);
    }
    switch (field_node->field()->type()->id()) {
#define PROCESS(InType)                                                          \
  case InType::type_id: {                                                        \
    impl_.reset(new TypedJoinKernel<InType, InType>(                             \
        ctx, left_key_list, right_key_list, left_schema_list, right_schema_list, \
        condition, join_type, result_schema, hash_configuration_list,            \
        hash_relation_idx));                                                     \
  } break;
      PROCESS_SUPPORTED_TYPES(PROCESS)
#undef PROCESS
      default: {
        std::cout << "TypedjoinKernel type not supported, type is "
                  << field_node->field()->type() << std::endl;
      } break;
    }
    //using type = decltype(field_node->field()->type()->id());
    //constexpr auto type = field_node->field()->type()->id();
    //using keytype = precompile::TypeIdTraits<type>::Type;
    //using keytype = precompile::TypeIdTraits<arrow::Type::UINT32>::Type;
    //using keytype = typename precompile::TypeTraits<InType>::ArrayType; 
    //impl_.reset(new TypedJoinKernel<arrow::UInt32Type>(
      // impl_.reset(new TypedJoinKernel<keytype>(
      //   ctx, left_key_list, right_key_list, left_schema_list,
      //                  right_schema_list, condition, join_type, result_schema,
      //                  hash_configuration_list, hash_relation_idx));
  } else {
  impl_.reset(new Impl(ctx, left_key_list, right_key_list, left_schema_list,
                       right_schema_list, condition, join_type, result_schema,
                       hash_configuration_list, hash_relation_idx));
  }
  kernel_name_ = "ConditionedJoinKernel";
}

arrow::Status ConditionedJoinKernel::MakeResultIterator(
    std::shared_ptr<arrow::Schema> schema,
    std::shared_ptr<ResultIterator<arrow::RecordBatch>>* out) {
  return impl_->MakeResultIterator(schema, out);
}

std::string ConditionedJoinKernel::GetSignature() { return impl_->GetSignature(); }

arrow::Status ConditionedJoinKernel::DoCodeGen(
    int level,
    std::vector<std::pair<std::pair<std::string, std::string>, gandiva::DataTypePtr>>
        input,
    std::shared_ptr<CodeGenContext>* codegen_ctx_out, int* var_id) {
  return impl_->DoCodeGen(level, input, codegen_ctx_out, var_id);
}

arrow::Status ConditionedJoinKernel::Evaluate(const ArrayList& in) {
  return impl_->Evaluate(in);
}


///////////////Codegen  SMJ  ////////////////
class ConditionedJoinArraysKernel::Impl {
 public:
  Impl() {}
  Impl(arrow::compute::FunctionContext* ctx,
       const std::vector<std::shared_ptr<arrow::Field>>& left_key_list,
       const std::vector<std::shared_ptr<arrow::Field>>& right_key_list,
       const std::shared_ptr<gandiva::Node>& func_node, int join_type,
       const std::vector<std::shared_ptr<arrow::Field>>& left_field_list,
       const std::vector<std::shared_ptr<arrow::Field>>& right_field_list,
       const std::shared_ptr<arrow::Schema>& result_schema)
      : ctx_(ctx) {
    std::vector<int> left_key_index_list;
    THROW_NOT_OK(GetIndexList(left_key_list, left_field_list, &left_key_index_list));
    std::vector<int> right_key_index_list;
    THROW_NOT_OK(GetIndexList(right_key_list, right_field_list, &right_key_index_list));
    std::vector<int> left_shuffle_index_list;
    std::vector<int> right_shuffle_index_list;
    THROW_NOT_OK(
        GetIndexListFromSchema(result_schema, left_field_list, &left_shuffle_index_list));
    THROW_NOT_OK(GetIndexListFromSchema(result_schema, right_field_list,
                                        &right_shuffle_index_list));

    std::vector<std::pair<int, int>> result_schema_index_list;
    int exist_index = -1;
    THROW_NOT_OK(GetResultIndexList(result_schema, left_field_list, right_field_list,
                                    join_type, exist_index, &result_schema_index_list));

    THROW_NOT_OK(LoadJITFunction(
        func_node, join_type, left_key_index_list, right_key_index_list,
        left_shuffle_index_list, right_shuffle_index_list, left_field_list,
        right_field_list, result_schema_index_list, exist_index, &prober_));
  }

  virtual arrow::Status Evaluate(const ArrayList& in) {
    RETURN_NOT_OK(prober_->Evaluate(in));
    return arrow::Status::OK();
  }

  virtual arrow::Status MakeResultIterator(
      std::shared_ptr<arrow::Schema> schema,
      std::shared_ptr<ResultIterator<arrow::RecordBatch>>* out) {
    RETURN_NOT_OK(prober_->MakeResultIterator(schema, out));
    return arrow::Status::OK();
  }

  std::string GetSignature() { return signature_; }

 private:
  using ArrayType = typename arrow::TypeTraits<arrow::Int64Type>::ArrayType;

  arrow::compute::FunctionContext* ctx_;
  std::shared_ptr<CodeGenBase> prober_;
  std::string signature_;

  arrow::Status GetResultIndexList(
      const std::shared_ptr<arrow::Schema>& result_schema,
      const std::vector<std::shared_ptr<arrow::Field>>& left_field_list,
      const std::vector<std::shared_ptr<arrow::Field>>& right_field_list,
      const int join_type, int& exist_index,
      std::vector<std::pair<int, int>>* result_schema_index_list) {
    int i = 0;
    bool found = false;
    int target_index = -1;
    int right_found = 0;
    for (auto target_field : result_schema->fields()) {
      target_index++;
      i = 0;
      found = false;
      for (auto field : left_field_list) {
        if (target_field->name() == field->name()) {
          (*result_schema_index_list).push_back(std::make_pair(0, i));
          found = true;
          break;
        }
        i++;
      }
      if (found == true) continue;
      i = 0;
      for (auto field : right_field_list) {
        if (target_field->name() == field->name()) {
          (*result_schema_index_list).push_back(std::make_pair(1, i));
          found = true;
          right_found++;
          break;
        }
        i++;
      }
      if (found == true) continue;
      if (join_type == 4) exist_index = target_index;
    }
    // Add one more col if join_type is ExistenceJoin
    if (join_type == 4) {
      (*result_schema_index_list).push_back(std::make_pair(1, right_found));
    }
    return arrow::Status::OK();
  }
  arrow::Status LoadJITFunction(
      const std::shared_ptr<gandiva::Node>& func_node, int join_type,
      const std::vector<int>& left_key_index_list,
      const std::vector<int>& right_key_index_list,
      const std::vector<int>& left_shuffle_index_list,
      const std::vector<int>& right_shuffle_index_list,
      const std::vector<std::shared_ptr<arrow::Field>>& left_field_list,
      const std::vector<std::shared_ptr<arrow::Field>>& right_field_list,
      const std::vector<std::pair<int, int>>& result_schema_index_list, int exist_index,
      std::shared_ptr<CodeGenBase>* out) {
    // generate ddl signature
    std::stringstream func_args_ss;
    func_args_ss << "<MergeJoin>"
                 << "[JoinType]" << join_type;
    if (func_node) {
      std::shared_ptr<CodeGenRegister> node_tmp;
      RETURN_NOT_OK(MakeCodeGenRegister(func_node, &node_tmp));
      func_args_ss << "[cond]" << node_tmp->GetFingerprint();
    }
    func_args_ss << "[BuildSchema]";
    for (auto field : left_field_list) {
      func_args_ss << field->type()->ToString();
    }
    func_args_ss << "[ProbeSchema]";
    for (auto field : right_field_list) {
      func_args_ss << field->type()->ToString();
    }
    func_args_ss << "[LeftKeyIndex]";
    for (auto i : left_key_index_list) {
      func_args_ss << i << ",";
    }
    func_args_ss << "[RightKeyIndex]";
    for (auto i : right_key_index_list) {
      func_args_ss << i << ",";
    }
    func_args_ss << "[LeftShuffleIndex]";
    for (auto i : left_shuffle_index_list) {
      func_args_ss << i << ",";
    }
    func_args_ss << "[RightShuffleIndex]";
    for (auto i : right_shuffle_index_list) {
      func_args_ss << i << ",";
    }

#ifdef DEBUG
    std::cout << "signature original line is " << func_args_ss.str() << std::endl;
#endif
    std::stringstream signature_ss;
    signature_ss << std::hex << std::hash<std::string>{}(func_args_ss.str());
    signature_ = signature_ss.str();

    auto file_lock = FileSpinLock();
    auto status = LoadLibrary(signature_, ctx_, out);
    if (!status.ok()) {
      // process
      try {
        auto codes = ProduceCodes(
            func_node, join_type, left_key_index_list, right_key_index_list,
            left_shuffle_index_list, right_shuffle_index_list, left_field_list,
            right_field_list, result_schema_index_list, exist_index);
        // compile codes
        RETURN_NOT_OK(CompileCodes(codes, signature_));
        RETURN_NOT_OK(LoadLibrary(signature_, ctx_, out));
      } catch (const std::runtime_error& error) {
        FileSpinUnLock(file_lock);
        throw error;
      }
    }
    FileSpinUnLock(file_lock);
    return arrow::Status::OK();
  }

  class TypedProberCodeGenImpl {
   public:
    TypedProberCodeGenImpl(std::string indice, std::shared_ptr<arrow::DataType> data_type,
                           bool left = true)
        : indice_(indice), data_type_(data_type), left_(left) {}
    std::string GetImplCachedDefine() {
      std::stringstream ss;
      ss << "using ArrayType_" << indice_ << " = " << GetTypeString(data_type_, "Array")
         << ";" << std::endl;
      ss << "std::vector<std::shared_ptr<ArrayType_" << indice_ << ">> cached_" << indice_
         << "_;" << std::endl;
      return ss.str();
    }
    std::string GetResultIteratorPrepare() {
      std::stringstream ss;
      ss << "builder_" << indice_ << "_ = std::make_shared<"
         << GetTypeString(data_type_, "Builder") << ">(ctx_->memory_pool());"
         << std::endl;
      return ss.str();
    }
    std::string GetProcessFinish() {
      std::stringstream ss;
      ss << "std::shared_ptr<arrow::Array> out_" << indice_ << ";" << std::endl;
      ss << "RETURN_NOT_OK(builder_" << indice_ << "_->Finish(&out_" << indice_ << "));"
         << std::endl;
      ss << "builder_" << indice_ << "_->Reset();" << std::endl;
      return ss.str();
    }
    std::string GetProcessOutList() {
      std::stringstream ss;
      ss << "out_" << indice_;
      return ss.str();
    }
    std::string GetResultIterCachedDefine() {
      std::stringstream ss;
      if (left_) {
        ss << "std::vector<std::shared_ptr<" << GetTypeString(data_type_, "Array")
           << ">> cached_" << indice_ << "_;" << std::endl;
      } else {
        ss << "std::shared_ptr<" << GetTypeString(data_type_, "Array") << "> cached_"
           << indice_ << "_;" << std::endl;
      }
      ss << "using ArrayType_" << indice_ << " = " << GetTypeString(data_type_, "Array")
         << ";" << std::endl;
      ss << "std::shared_ptr<" << GetTypeString(data_type_, "Builder") << "> builder_"
         << indice_ << "_;" << std::endl;
      return ss.str();
    }

   private:
    std::string indice_;
    std::shared_ptr<arrow::DataType> data_type_;
    bool left_;
  };
  std::string GetJoinKeyFieldListDefine(
      const std::vector<int>& left_key_index_list,
      const std::vector<std::shared_ptr<arrow::Field>>& field_list) {
    std::stringstream ss;
    for (int i = 0; i < left_key_index_list.size(); i++) {
      auto field = field_list[left_key_index_list[i]];
      if (i != (left_key_index_list.size() - 1)) {
        ss << "arrow::field(\"" << field->name()
           << "\", arrow::" << GetArrowTypeDefString(field->type()) << "), ";
      } else {
        ss << "arrow::field(\"" << field->name()
           << "\", arrow::" << GetArrowTypeDefString(field->type()) << ")";
      }
    }
    return ss.str();
  }
  std::string GetEvaluateCacheInsert(const std::vector<int>& index_list) {
    std::stringstream ss;
    for (auto i : index_list) {
      ss << "cached_0_" << i << "_.push_back(std::make_shared<ArrayType_0_" << i
         << ">(in[" << i << "]));" << std::endl;
    }
    return ss.str();
  }
  std::string GetEncodeJoinKey(std::vector<int> key_indices) {
    std::stringstream ss;
    for (int i = 0; i < key_indices.size(); i++) {
      if (i != (key_indices.size() - 1)) {
        ss << "in[" << key_indices[i] << "], ";
      } else {
        ss << "in[" << key_indices[i] << "]";
      }
    }
    return ss.str();
  }
  std::string GetFinishCachedParameter(const std::vector<int>& key_indices) {
    std::stringstream ss;
    for (int i = 0; i < key_indices.size(); i++) {
      if (i != (key_indices.size() - 1)) {
        ss << "cached_0_" << key_indices[i] << "_, ";
      } else {
        ss << "cached_0_" << key_indices[i] << "_";
      }
    }
    auto ret = ss.str();
    if (ret.empty()) {
      return ret;
    } else {
      return ", " + ret;
    }
  }
  std::string GetImplCachedDefine(
      std::vector<std::shared_ptr<TypedProberCodeGenImpl>> codegen_list) {
    std::stringstream ss;
    for (auto codegen : codegen_list) {
      ss << codegen->GetImplCachedDefine() << std::endl;
    }
    return ss.str();
  }
  std::string GetResultIteratorParams(std::vector<int> key_indices) {
    std::stringstream ss;
    for (int i = 0; i < key_indices.size(); i++) {
      if (i != (key_indices.size() - 1)) {
        ss << "const std::vector<std::shared_ptr<ArrayType_0_" << key_indices[i]
           << ">> &cached_0_" << key_indices[i] << ", " << std::endl;
      } else {
        ss << "const std::vector<std::shared_ptr<ArrayType_0_" << key_indices[i]
           << ">> &cached_0_" << key_indices[i];
      }
    }
    auto ret = ss.str();
    if (ret.empty()) {
      return ret;
    } else {
      return ", " + ret;
    }
  }
  std::string GetResultIteratorSet(std::vector<int> key_indices) {
    std::stringstream ss;
    for (auto i : key_indices) {
      ss << "cached_0_" << i << "_ = cached_0_" << i << ";" << std::endl;
    }
    return ss.str();
  }
  std::string GetResultIteratorPrepare(
      std::vector<std::shared_ptr<TypedProberCodeGenImpl>> left_codegen_list,
      std::vector<std::shared_ptr<TypedProberCodeGenImpl>> right_codegen_list) {
    std::stringstream ss;
    for (auto codegen : left_codegen_list) {
      ss << codegen->GetResultIteratorPrepare() << std::endl;
    }
    for (auto codegen : right_codegen_list) {
      ss << codegen->GetResultIteratorPrepare() << std::endl;
    }
    return ss.str();
  }
  std::string GetProcessRightSet(std::vector<int> indices) {
    std::stringstream ss;
    for (auto i : indices) {
      ss << "cached_1_" << i << "_ = std::make_shared<ArrayType_1_" << i << ">(in[" << i
         << "]);" << std::endl;
    }
    return ss.str();
  }
  std::string GetProcessFinish(
      std::vector<std::shared_ptr<TypedProberCodeGenImpl>> left_codegen_list,
      std::vector<std::shared_ptr<TypedProberCodeGenImpl>> right_codegen_list) {
    std::stringstream ss;
    for (auto codegen : left_codegen_list) {
      ss << codegen->GetProcessFinish() << std::endl;
    }
    for (auto codegen : right_codegen_list) {
      ss << codegen->GetProcessFinish() << std::endl;
    }
    return ss.str();
  }
  std::string GetProcessOutList(
      const std::vector<std::pair<int, int>>& result_schema_index_list,
      std::vector<std::shared_ptr<TypedProberCodeGenImpl>> left_codegen_list,
      std::vector<std::shared_ptr<TypedProberCodeGenImpl>> right_codegen_list) {
    std::stringstream ss;
    auto item_count = result_schema_index_list.size();
    int i = 0;
    for (auto index : result_schema_index_list) {
      std::shared_ptr<TypedProberCodeGenImpl> codegen;
      if (index.first == 0) {
        codegen = left_codegen_list[index.second];
      } else {
        codegen = right_codegen_list[index.second];
      }
      if (i++ != (item_count - 1)) {
        ss << codegen->GetProcessOutList() << ", ";
      } else {
        ss << codegen->GetProcessOutList();
      }
    }
    return ss.str();
  }
  std::string GetResultIterCachedDefine(
      std::vector<std::shared_ptr<TypedProberCodeGenImpl>> left_codegen_list,
      std::vector<std::shared_ptr<TypedProberCodeGenImpl>> right_codegen_list) {
    std::stringstream ss;
    for (auto codegen : left_codegen_list) {
      ss << codegen->GetResultIterCachedDefine() << std::endl;
    }
    for (auto codegen : right_codegen_list) {
      ss << codegen->GetResultIterCachedDefine() << std::endl;
    }
    return ss.str();
  }
  std::string GetInnerJoin(bool cond_check,
                           const std::vector<int>& left_shuffle_index_list,
                           const std::vector<int>& right_shuffle_index_list,
                           const std::vector<int>& right_key_index_list) {
    std::stringstream ss;
    for (auto i : left_shuffle_index_list) {
      ss << "if (cached_0_" << i << "_[tmp.array_id]->null_count()) {" << std::endl;
      ss << "if (cached_0_" << i << "_[tmp.array_id]->IsNull(tmp.id)) {" << std::endl;
      ss << "  RETURN_NOT_OK(builder_0_" << i << "_->AppendNull());" << std::endl;
      ss << "} else {" << std::endl;
      ss << "  RETURN_NOT_OK(builder_0_" << i << "_->Append(cached_0_" << i
         << "_[tmp.array_id]->GetView(tmp.id)));" << std::endl;
      ss << "}" << std::endl;
      ss << "} else {" << std::endl;
      ss << "  RETURN_NOT_OK(builder_0_" << i << "_->Append(cached_0_" << i
         << "_[tmp.array_id]->GetView(tmp.id)));" << std::endl;
      ss << "}" << std::endl;
    }
    for (auto i : right_shuffle_index_list) {
      ss << "if (cached_1_" << i << "_->null_count()) {" << std::endl;
      ss << "if (cached_1_" << i << "_->IsNull(i)) {" << std::endl;
      ss << "  RETURN_NOT_OK(builder_1_" << i << "_->AppendNull());" << std::endl;
      ss << "} else {" << std::endl;
      ss << "  RETURN_NOT_OK(builder_1_" << i << "_->Append(cached_1_" << i
         << "_->GetView(i)));" << std::endl;
      ss << "}" << std::endl;
      ss << "} else {" << std::endl;
      ss << "  RETURN_NOT_OK(builder_1_" << i << "_->Append(cached_1_" << i
         << "_->GetView(i)));" << std::endl;
      ss << "}" << std::endl;
    }
    std::string shuffle_str;
    if (cond_check) {
      shuffle_str = R"(
              if (ConditionCheck(tmp, i)) {
                )" + ss.str() +
                    R"(
                out_length += 1;
              }
              left_it->next();
            }
            left_it->setpos(cur_idx, seg_len, pl);
      )";
    } else {
      shuffle_str = R"(
              )" + ss.str() +
                    R"(
              left_it->next();
              out_length += 1;}
              left_it->setpos(cur_idx, seg_len, pl);
      )";
    }
    std::string right_value;
    if (right_key_index_list.size() > 1) {
      right_value += "item_content{";
      for (auto i = 0; i < right_key_index_list.size(); i++) {
        right_value += "typed_array_" + std::to_string(i) + "->GetView(i)";
        if (i != right_key_index_list.size() - 1) {
          right_value += ",";
        }
      }
      right_value += "}";
    } else {
      right_value = "typed_array_0->GetView(i)";
    }
    return R"(
      auto right_content =)" +
           right_value + R"(;
        if (!typed_array_0->IsNull(i)) {
            while (left_it->hasnext() && left_it->value() < right_content) {
            left_it->next();
            }
            int64_t cur_idx, seg_len, pl; left_it->getpos(&cur_idx, &seg_len, &pl);
            while(left_it->hasnext() && left_it->value() == right_content) {
              auto tmp = GetArrayItemIdex(left_it);)" +
           shuffle_str + R"(
              //if (left_it->value() > right_content && left_it->hasnext()){
              //continue;
              //}
          }
  )";
  }
  std::string GetOuterJoin(bool cond_check,
                           const std::vector<int>& left_shuffle_index_list,
                           const std::vector<int>& right_shuffle_index_list,
                           const std::vector<int>& right_key_index_list) {
    std::stringstream left_null_ss;
    std::stringstream left_valid_ss;
    std::stringstream right_valid_ss;
    for (auto i : left_shuffle_index_list) {
      left_valid_ss << "if (cached_0_" << i << "_[tmp.array_id]->null_count()) {"
                    << std::endl;
      left_valid_ss << "if (cached_0_" << i << "_[tmp.array_id]->IsNull(tmp.id)) {"
                    << std::endl;
      left_valid_ss << "  RETURN_NOT_OK(builder_0_" << i << "_->AppendNull());"
                    << std::endl;
      left_valid_ss << "} else {" << std::endl;
      left_valid_ss << "  RETURN_NOT_OK(builder_0_" << i << "_->Append(cached_0_" << i
                    << "_[tmp.array_id]->GetView(tmp.id)));" << std::endl;
      left_valid_ss << "}" << std::endl;
      left_valid_ss << "} else {" << std::endl;
      left_valid_ss << "  RETURN_NOT_OK(builder_0_" << i << "_->Append(cached_0_" << i
                    << "_[tmp.array_id]->GetView(tmp.id)));" << std::endl;
      left_valid_ss << "}" << std::endl;
      left_null_ss << "RETURN_NOT_OK(builder_0_" << i << "_->AppendNull());" << std::endl;
    }
    for (auto i : right_shuffle_index_list) {
      right_valid_ss << "if (cached_1_" << i << "_->null_count()) {" << std::endl;
      right_valid_ss << "if (cached_1_" << i << "_->IsNull(i)) {" << std::endl;
      right_valid_ss << "  RETURN_NOT_OK(builder_1_" << i << "_->AppendNull());"
                     << std::endl;
      right_valid_ss << "} else {" << std::endl;
      right_valid_ss << "  RETURN_NOT_OK(builder_1_" << i << "_->Append(cached_1_" << i
                     << "_->GetView(i)));" << std::endl;
      right_valid_ss << "}" << std::endl;
      right_valid_ss << "} else {" << std::endl;
      right_valid_ss << "  RETURN_NOT_OK(builder_1_" << i << "_->Append(cached_1_" << i
                     << "_->GetView(i)));" << std::endl;
      right_valid_ss << "}" << std::endl;
    }
    std::string shuffle_str;
    if (cond_check) {
      shuffle_str = R"(
              if (ConditionCheck(tmp, i)) {
                )" + left_valid_ss.str() +
                    right_valid_ss.str() + R"(
                out_length += 1;
              }
      )";
    } else {
      shuffle_str = R"(
              )" + left_valid_ss.str() +
                    right_valid_ss.str() + R"(
              out_length += 1;
      )";
    }

    std::string right_value;
    if (right_key_index_list.size() > 1) {
      right_value += "item_content{";
      for (auto i = 0; i < right_key_index_list.size(); i++) {
        right_value += "typed_array_" + std::to_string(i) + "->GetView(i)";
        if (i != right_key_index_list.size() - 1) {
          right_value += ",";
        }
      }
      right_value += "}";
    } else {
      right_value = "typed_array_0->GetView(i)";
    }
    return R"(
      auto right_content =)" +
           right_value + R"(;
      if (!typed_array_0->IsNull(i)) {
         while (left_it->hasnext() && left_it->value() < right_content ) {
    left_it->next();
  }
  int64_t cur_idx, seg_len, pl; left_it->getpos(&cur_idx, &seg_len, &pl);
  while(left_it->hasnext() && left_it->value() == right_content) {
    auto tmp = GetArrayItemIdex(left_it);
          )" +  // TODO: cond check
           left_valid_ss.str() +
           right_valid_ss.str() + R"(
          left_it->next();
          last_match_idx = i;
          out_length += 1;
        }
        left_it->setpos(cur_idx, seg_len, pl);
        if(left_it->value() > right_content && left_it->hasnext() ) {
          if (last_match_idx == i) {
            continue;
          }
          auto tmp = GetArrayItemIdex(left_it);
            )" +
           left_null_ss.str() + right_valid_ss.str() + R"(
             out_length += 1;
          }
          if (!left_it->hasnext()) {
            )" +
           left_null_ss.str() + right_valid_ss.str() + R"(
            out_length += 1;
          }

        } else {
          int64_t cur_idx, seg_len, pl; left_it->getpos(&cur_idx, &seg_len, &pl);
          while(left_it->hasnext() && left_it->value() == right_content) {
            auto tmp = GetArrayItemIdex(left_it);
            )" +
           left_valid_ss.str() + right_valid_ss.str() + R"(
            left_it->next();
            out_length += 1;
            }
          left_it->setpos(cur_idx, seg_len, pl);
        }
  )";
  }
  std::string GetFullOuterJoin(bool cond_check,
                               const std::vector<int>& left_shuffle_index_list,
                               const std::vector<int>& right_shuffle_index_list,
                               const std::vector<int>& right_key_index_list) {
    std::stringstream left_null_ss;
    std::stringstream right_null_ss;
    std::stringstream left_valid_ss;
    std::stringstream right_valid_ss;
    for (auto i : left_shuffle_index_list) {
      left_valid_ss << "if (cached_0_" << i << "_[tmp.array_id]->null_count()) {"
                    << std::endl;
      left_valid_ss << "if (cached_0_" << i << "_[tmp.array_id]->IsNull(tmp.id)) {"
                    << std::endl;
      left_valid_ss << "  RETURN_NOT_OK(builder_0_" << i << "_->AppendNull());"
                    << std::endl;
      left_valid_ss << "} else {" << std::endl;
      left_valid_ss << "  RETURN_NOT_OK(builder_0_" << i << "_->Append(cached_0_" << i
                    << "_[tmp.array_id]->GetView(tmp.id)));" << std::endl;
      left_valid_ss << "}" << std::endl;
      left_valid_ss << "} else {" << std::endl;
      left_valid_ss << "  RETURN_NOT_OK(builder_0_" << i << "_->Append(cached_0_" << i
                    << "_[tmp.array_id]->GetView(tmp.id)));" << std::endl;
      left_valid_ss << "}" << std::endl;
      left_null_ss << "RETURN_NOT_OK(builder_0_" << i << "_->AppendNull());" << std::endl;
    }
    for (auto i : right_shuffle_index_list) {
      right_valid_ss << "if (cached_1_" << i << "_->null_count()) {" << std::endl;
      right_valid_ss << "if (cached_1_" << i << "_->IsNull(i)) {" << std::endl;
      right_valid_ss << "  RETURN_NOT_OK(builder_1_" << i << "_->AppendNull());"
                     << std::endl;
      right_valid_ss << "} else {" << std::endl;
      right_valid_ss << "  RETURN_NOT_OK(builder_1_" << i << "_->Append(cached_1_" << i
                     << "_->GetView(i)));" << std::endl;
      right_valid_ss << "}" << std::endl;
      right_valid_ss << "} else {" << std::endl;
      right_valid_ss << "  RETURN_NOT_OK(builder_1_" << i << "_->Append(cached_1_" << i
                     << "_->GetView(i)));" << std::endl;
      right_valid_ss << "}" << std::endl;
      right_null_ss << "RETURN_NOT_OK(builder_1_" << i << "_->AppendNull());"
                    << std::endl;
    }
    std::string shuffle_str;
    if (cond_check) {
      shuffle_str = R"(
              if (ConditionCheck(tmp, i)) {
                )" + left_valid_ss.str() +
                    right_valid_ss.str() + R"(
                out_length += 1;
              }
      )";
    } else {
      shuffle_str = R"(
              )" + left_valid_ss.str() +
                    right_valid_ss.str() + R"(
              out_length += 1;
      )";
    }

    std::string right_value;
    if (right_key_index_list.size() > 1) {
      right_value += "item_content{";
      for (auto i = 0; i < right_key_index_list.size(); i++) {
        right_value += "typed_array_" + std::to_string(i) + "->GetView(i)";
        if (i != right_key_index_list.size() - 1) {
          right_value += ",";
        }
      }
      right_value += "}";
    } else {
      right_value = "typed_array_0->GetView(i)";
    }
    return R"(
      //full outer join
      auto right_content =)" +
           right_value + R"(;
      while (left_it->hasnext() && left_it->value() < right_content ) {
        auto tmp = GetArrayItemIdex(left_it);
          )" +  // TODO: cond check
           left_valid_ss.str() +
           right_null_ss.str() + R"(
        left_it->next();
      }
  int64_t cur_idx, seg_len, pl; left_it->getpos(&cur_idx, &seg_len, &pl);
  while(left_it->hasnext() && left_it->value() == right_content) {
    auto tmp = GetArrayItemIdex(left_it);
          )" +  // TODO: cond check
           left_valid_ss.str() +
           right_valid_ss.str() + R"(
          left_it->next();
          last_match_idx = i;
          out_length += 1;
        }
  left_it->setpos(cur_idx, seg_len, pl);
        if(left_it->value() > right_content && left_it->hasnext() ) {
          if (last_match_idx == i) {
            continue;
          }
          auto tmp = GetArrayItemIdex(left_it);
            )" +
           left_null_ss.str() + right_valid_ss.str() + R"(
             out_length += 1;
          }
          if (!left_it->hasnext()) {
            )" +
           left_null_ss.str() + right_valid_ss.str() + R"(
            out_length += 1;
          }
  )";
  }
  std::string GetAntiJoin(bool cond_check,
                          const std::vector<int>& left_shuffle_index_list,
                          const std::vector<int>& right_shuffle_index_list) {
    std::stringstream left_null_ss;
    std::stringstream right_valid_ss;
    for (auto i : left_shuffle_index_list) {
      left_null_ss << "RETURN_NOT_OK(builder_0_" << i << "_->AppendNull());" << std::endl;
    }
    for (auto i : right_shuffle_index_list) {
      right_valid_ss << "if (cached_1_" << i << "_->null_count()) {" << std::endl;
      right_valid_ss << "if (cached_1_" << i << "_->IsNull(i)) {" << std::endl;
      right_valid_ss << "  RETURN_NOT_OK(builder_1_" << i << "_->AppendNull());"
                     << std::endl;
      right_valid_ss << "} else {" << std::endl;
      right_valid_ss << "  RETURN_NOT_OK(builder_1_" << i << "_->Append(cached_1_" << i
                     << "_->GetView(i)));" << std::endl;
      right_valid_ss << "}" << std::endl;
      right_valid_ss << "} else {" << std::endl;
      right_valid_ss << "  RETURN_NOT_OK(builder_1_" << i << "_->Append(cached_1_" << i
                     << "_->GetView(i)));" << std::endl;
      right_valid_ss << "}" << std::endl;
    }
    std::string shuffle_str;
    if (cond_check) {
      shuffle_str = R"(
          hasequaled = true;
          auto tmp = GetArrayItemIdex(left_it);
            if (ConditionCheck(tmp, i)) {
              found = true;
              break;
            }
            left_it->next();
    last_match_idx = i;
    }
          if (!found && hasequaled) {
              )" + left_null_ss.str() +
                    right_valid_ss.str() + R"(
            out_length += 1;
          }
      )";
    } else {
      shuffle_str = R"(
        left_it->next();
        last_match_idx = i;
        }
      )";
    }
    return R"(
  while (left_it->hasnext() && left_it->value() < typed_array_0->GetView(i)) {
    left_it->next();
  }

  int64_t cur_idx, seg_len, pl; left_it->getpos(&cur_idx, &seg_len, &pl);
  bool found = false;
  bool hasequaled = false;
  while (left_it->hasnext() && left_it->value() == typed_array_0->GetView(i)) {
    )" + shuffle_str +
           R"(
    left_it->setpos(cur_idx, seg_len, pl);
  if (left_it->value() > typed_array_0->GetView(i) && left_it->hasnext() ) {
    if (last_match_idx == i) {
      continue;
    }
          )" +
           left_null_ss.str() + right_valid_ss.str() + R"(
          out_length += 1; }
  if (!left_it->hasnext()) {
          )" +
           left_null_ss.str() + right_valid_ss.str() + R"(
          out_length += 1;
        }
  )";
  }
  std::string GetSemiJoin(bool cond_check,
                          const std::vector<int>& left_shuffle_index_list,
                          const std::vector<int>& right_shuffle_index_list,
                          const std::vector<int>& right_key_index_list) {
    std::stringstream ss;
    for (auto i : left_shuffle_index_list) {
      ss << "RETURN_NOT_OK(builder_0_" << i << "_->AppendNull());" << std::endl;
    }
    for (auto i : right_shuffle_index_list) {
      ss << "if (cached_1_" << i << "_->null_count()) {" << std::endl;
      ss << "if (cached_1_" << i << "_->IsNull(i)) {" << std::endl;
      ss << "  RETURN_NOT_OK(builder_1_" << i << "_->AppendNull());" << std::endl;
      ss << "} else {" << std::endl;
      ss << "  RETURN_NOT_OK(builder_1_" << i << "_->Append(cached_1_" << i
         << "_->GetView(i)));" << std::endl;
      ss << "}" << std::endl;
      ss << "} else {" << std::endl;
      ss << "  RETURN_NOT_OK(builder_1_" << i << "_->Append(cached_1_" << i
         << "_->GetView(i)));" << std::endl;
      ss << "}" << std::endl;
    }
    std::string shuffle_str;
    if (cond_check) {
      shuffle_str = R"(
        while (left_it->hasnext() && left_it->value() == right_content) {
            auto tmp = GetArrayItemIdex(left_it);
              if (ConditionCheck(tmp, i)) {
                )" + ss.str() +
                    R"(
                out_length += 1;
                break;
              }
      )";
    } else {
      shuffle_str = R"(
        if (left_it->value() == right_content && left_it->hasnext()) {
              )" + ss.str() +
                    R"(
              out_length += 1;
      )";
    }
    std::string right_value;
    if (right_key_index_list.size() > 1) {
      right_value += "item_content{";
      for (auto i = 0; i < right_key_index_list.size(); i++) {
        right_value += "typed_array_" + std::to_string(i) + "->GetView(i)";
        if (i != right_key_index_list.size() - 1) {
          right_value += ",";
        }
      }
      right_value += "}";
    } else {
      right_value = "typed_array_0->GetView(i)";
    }
    return R"(
      auto right_content = )" +
           right_value + R"(;
             if (!typed_array_0->IsNull(i)) {
  while (left_it->hasnext() && left_it->value() < right_content) {
    left_it->next();
  }
  
  int64_t cur_idx, seg_len, pl; left_it->getpos(&cur_idx, &seg_len, &pl);
  )" + shuffle_str +
           R"(
      left_it->next();
  }
  left_it->setpos(cur_idx, seg_len, pl);
  //if (left_it->value() > right_content && left_it->hasnext() ) {
  //  continue;
  //
  //      }
      }
  )";
  }
  std::string GetExistenceJoin(bool cond_check,
                               const std::vector<int>& left_shuffle_index_list,
                               const std::vector<int>& right_shuffle_index_list,
                               const std::vector<int>& right_key_index_list) {
    std::stringstream right_exist_ss;
    std::stringstream right_not_exist_ss;
    std::stringstream left_valid_ss;
    std::stringstream right_valid_ss;

    right_exist_ss
        << "const bool exist = true; RETURN_NOT_OK(builder_1_exists_->Append(exist));"
        << std::endl;
    right_not_exist_ss << "const bool not_exist = false; "
                          "RETURN_NOT_OK(builder_1_exists_->Append(not_exist));"
                       << std::endl;

    for (auto i : right_shuffle_index_list) {
      right_valid_ss << "RETURN_NOT_OK(builder_1_" << i << "_->Append(cached_1_" << i
                     << "_->GetView(i)));" << std::endl;
    }
    std::string shuffle_str;
    if (cond_check) {
      shuffle_str = R"(
              if (ConditionCheck(tmp, i)) {
                )" + right_valid_ss.str() +
                    right_exist_ss.str() + R"(
                out_length += 1;
              }
      )";
    } else {
      shuffle_str = R"(
              )" + right_valid_ss.str() +
                    right_exist_ss.str() + R"(
              out_length += 1;
      )";
    }
    std::string right_value;
    if (right_key_index_list.size() > 1) {
      right_value += "item_content{";
      for (auto i = 0; i < right_key_index_list.size(); i++) {
        right_value += "typed_array_" + std::to_string(i) + "->GetView(i)";
        if (i != right_key_index_list.size() - 1) {
          right_value += ",";
        }
      }
      right_value += "}";
    } else {
      right_value = "typed_array_0->GetView(i)";
    }
    return R"(
        // existence join
        auto right_content = )" +
           right_value + R"(;
        if (!typed_array_0->IsNull(i)) {
          while (left_it->hasnext() && left_it->value() < right_content) {
            left_it->next();
          }
          int64_t cur_idx, seg_len, pl; left_it->getpos(&cur_idx, &seg_len, &pl);
          if(left_it->hasnext() && left_it->value() == right_content) {
              )" +
           shuffle_str + R"(
            last_match_idx = i;
            while (left_it->hasnext() && left_it->value() == right_content) {
               left_it->next();
            }
          }
          left_it->setpos(cur_idx, seg_len, pl);
          if(left_it->hasnext() && left_it->value() > right_content) {
            if (last_match_idx == i) {
            continue;
            }
            )" +
           right_valid_ss.str() + right_not_exist_ss.str() + R"(
            out_length += 1;
          }
          if (!left_it->hasnext()) {
            )" +
           right_valid_ss.str() + right_not_exist_ss.str() + R"(
            out_length += 1;
          }
        }
  )";
  }
  std::string GetProcessProbe(int join_type, bool cond_check,
                              const std::vector<int>& left_shuffle_index_list,
                              const std::vector<int>& right_shuffle_index_list,
                              const std::vector<int>& right_key_index_list) {
    switch (join_type) {
      case 0: { /*Inner Join*/
        return GetInnerJoin(cond_check, left_shuffle_index_list, right_shuffle_index_list,
                            right_key_index_list);
      } break;
      case 1: { /*Outer Join*/
        return GetOuterJoin(cond_check, left_shuffle_index_list, right_shuffle_index_list,
                            right_key_index_list);
      } break;
      case 2: { /*Anti Join*/
        return GetAntiJoin(cond_check, left_shuffle_index_list, right_shuffle_index_list);
      } break;
      case 3: { /*Semi Join*/
        return GetSemiJoin(cond_check, left_shuffle_index_list, right_shuffle_index_list,
                           right_key_index_list);
      } break;
      case 4: { /*Existence Join*/
        return GetExistenceJoin(cond_check, left_shuffle_index_list,
                                right_shuffle_index_list, right_key_index_list);
      } break;
      case 5: { /*Full outer Join*/
        return GetFullOuterJoin(cond_check, left_shuffle_index_list,
                                right_shuffle_index_list, right_key_index_list);
      } break;
      default:
        std::cout << "ConditionedProbeArraysTypedImpl only support join type: InnerJoin, "
                     "RightJoin"
                  << std::endl;
        throw;
    }
    return "";
  }
  std::string GetConditionCheckFunc(
      const std::shared_ptr<gandiva::Node>& func_node,
      const std::vector<std::shared_ptr<arrow::Field>>& left_field_list,
      const std::vector<std::shared_ptr<arrow::Field>>& right_field_list,
      std::vector<int>* left_out_index_list, std::vector<int>* right_out_index_list) {
    std::shared_ptr<CodeGenNodeVisitor> func_node_visitor;
    int func_count = 0;
    std::vector<std::string> input_list;
    std::vector<gandiva::ExpressionPtr> project_node_list;
    THROW_NOT_OK(MakeCodeGenNodeVisitor(func_node, {left_field_list, right_field_list},
                                        &func_count, &input_list, left_out_index_list,
                                        right_out_index_list, &project_node_list,
                                        &func_node_visitor));

    return R"(
    inline bool ConditionCheck(ArrayItemIndex x, int y) {
      )" + func_node_visitor->GetPrepare() +
           R"(
        return )" +
           func_node_visitor->GetResult() +
           R"(;
    }
  )";
  }
  arrow::Status GetTypedProberCodeGen(
      std::string prefix, bool left, const std::vector<int>& index_list,
      const std::vector<std::shared_ptr<arrow::Field>>& field_list, int exist_index,
      std::vector<std::shared_ptr<TypedProberCodeGenImpl>>* out_list,
      int join_type = -1) {
    for (auto i : index_list) {
      auto field = field_list[i];
      auto codegen = std::make_shared<TypedProberCodeGenImpl>(prefix + std::to_string(i),
                                                              field->type(), left);
      (*out_list).push_back(codegen);
    }
    if (join_type == 4 && exist_index != -1) {
      auto codegen = std::make_shared<TypedProberCodeGenImpl>(prefix + "exists",
                                                              arrow::boolean(), left);
      (*out_list).insert((*out_list).begin() + exist_index, codegen);
    }
    return arrow::Status::OK();
  }
  std::vector<int> MergeKeyIndexList(const std::vector<int>& left_index_list,
                                     const std::vector<int>& right_index_list) {
    std::vector<int> ret = left_index_list;
    for (auto i : right_index_list) {
      if (std::find(left_index_list.begin(), left_index_list.end(), i) ==
          left_index_list.end()) {
        ret.push_back(i);
      }
    }
    std::sort(ret.begin(), ret.end());
    return ret;
  }
  std::string GetKeyCType(const std::vector<int>& key_index_list,
                          const std::vector<std::shared_ptr<arrow::Field>>& field_list) {
    auto field = field_list[key_index_list[0]];
    return GetCTypeString(field->type());
  }
  std::string GetIdArrayStr(bool cond_check, int join_type) {
    std::stringstream ss;
    std::string tuple_str;
    if (cond_check) {
      tuple_str = "idx_to_arrarid_.emplace_back(cur_array_id_);";
    } else {
      tuple_str = "idx_to_arrarid_.emplace_back(cur_array_id_);";
      if (join_type == 2 || join_type == 3) {
        tuple_str = "";
      }
    }
    ss << tuple_str << std::endl;
    return ss.str();
  }
  std::string GetTupleStr(bool multiple_cols, int size) {
    std::stringstream ss;
    std::string tuple_str;
    if (multiple_cols) {
      tuple_str = "std::make_tuple";
    }
    if (multiple_cols) {
      for (int i = 0; i < size; i++) {
        std::string local_tuple =
            "(typed_array_" + std::to_string(i) + "->GetView(cur_id_),";
        tuple_str += local_tuple;
      }
    } else {
      tuple_str += "typed_array_0->GetView(cur_id_),";
    }
    tuple_str.erase(tuple_str.end() - 1, tuple_str.end());
    if (multiple_cols) {
      tuple_str += "))";
    }

    ss << std::endl << "left_list_->emplace_back(" << tuple_str << ");" << std::endl;
    return ss.str();
  }
  std::string GetListStr(bool multiple_cols, int size) {
    std::stringstream ss;
    std::string tuple_str;
    if (multiple_cols) {
      for (int i = 0; i < size; i++) {
        std::string local_tuple = "typed_array_" + std::to_string(i) + ",";
        tuple_str += local_tuple;
      }
    } else {
      tuple_str += "typed_array_0,";
    }
    tuple_str.erase(tuple_str.end() - 1, tuple_str.end());

    ss << std::endl << "left_list_.emplace_back(" << tuple_str << ");" << std::endl;
    return ss.str();
  }
  std::string GetListContentStr(bool multiple_cols, int size) {
    std::stringstream ss;
    std::string tuple_str;
    if (multiple_cols) {
      for (int i = 0; i < size; i++) {
        std::string local_tuple =
            "std::get<" + std::to_string(i) + ">(it)->GetView(segment_len),";
        tuple_str += local_tuple;
      }
      tuple_str.erase(tuple_str.end() - 1, tuple_str.end());
      ss << std::endl << "return {" + tuple_str + "};" << std::endl;
    } else {
      ss << std::endl << "return it->GetView(segment_len);" << std::endl;
    }
    return ss.str();
  }
  std::string GetTypedArray(bool multiple_cols, int idx, std::vector<int> key_list,
                            std::vector<std::shared_ptr<arrow::Field>> field_list) {
    std::stringstream ss;
    if (multiple_cols) {
      for (int i = 0; i < key_list.size(); i++) {
        ss << "auto typed_array"
           << "_" << i << " = std::make_shared<"
           << GetTypeString(field_list[key_list[i]]->type(), "Array") << ">(in["
           << key_list[i] << "]);" << std::endl;
      }

    } else {
      ss << "auto typed_array_0 = std::make_shared<"
         << GetTypeString(field_list[key_list[0]]->type(), "Array") << ">(in[" << idx
         << "]);" << std::endl;
    }
    return ss.str();
  }
  std::string GetTypedArray(bool multiple_cols, std::string index, int i,
                            std::string data_type,
                            std::string evaluate_encode_join_key_str) {
    std::stringstream ss;
    if (multiple_cols) {
      ss << "auto concat_kernel_arr_list = {" << evaluate_encode_join_key_str << "};"
         << std::endl;
      ss << "std::shared_ptr<arrow::Array> hash_in;" << std::endl;
      ss << "RETURN_NOT_OK(hash_kernel_->Evaluate(concat_kernel_arr_list, &hash_in));"
         << std::endl;
      ss << "auto typed_array = std::make_shared<Int32Array>(hash_in);" << std::endl;
    } else {
      ss << "auto typed_array = std::make_shared<" << data_type << ">(in[" << i << "]);"
         << std::endl;
    }
    return ss.str();
  }
  std::string ProduceCodes(
      const std::shared_ptr<gandiva::Node>& func_node, int join_type,
      const std::vector<int>& left_key_index_list,
      const std::vector<int>& right_key_index_list,
      const std::vector<int>& left_shuffle_index_list,
      const std::vector<int>& right_shuffle_index_list,
      const std::vector<std::shared_ptr<arrow::Field>>& left_field_list,
      const std::vector<std::shared_ptr<arrow::Field>>& right_field_list,
      const std::vector<std::pair<int, int>>& result_schema_index_list, int exist_index) {
    std::vector<int> left_cond_index_list;
    std::vector<int> right_cond_index_list;
    bool cond_check = false;
    bool multiple_cols = (left_key_index_list.size() > 1);
    std::string list_tiem_str;

    std::string hash_map_include_str = "";

    std::string hash_map_type_str =
        GetTypeString(left_field_list[left_key_index_list[0]]->type(), "Array");
    std::string item_content_str =
        GetCTypeString(left_field_list[left_key_index_list[0]]->type());
    if (item_content_str == "std::string") {
      item_content_str = "nonstd::sv_lite::string_view";
    }
    list_tiem_str = R"(
typedef  std::shared_ptr<)" +
                    hash_map_type_str +
                    R"(> list_item;
typedef )" + item_content_str +
                    " item_content;";

    std::vector<std::string> tuple_types;
    std::vector<std::string> content_tuple_types;

    if (multiple_cols) {
      list_tiem_str = R"(
        #include <tuple>)";

      for (auto& key : left_key_index_list) {
        tuple_types.push_back("std::shared_ptr<" +
                              GetTypeString(left_field_list[key]->type(), "Array") + ">");
        content_tuple_types.push_back(GetCTypeString(left_field_list[key]->type()));
      }

      std::string tuple_define_str = "std::tuple<";
      for (auto type : tuple_types) {
        tuple_define_str += type;
        tuple_define_str += ",";
      }
      // remove the ending ','
      tuple_define_str.erase(tuple_define_str.end() - 1, tuple_define_str.end());

      std::string content_define_str = "std::tuple<";
      for (auto type : content_tuple_types) {
        if (type == "std::string") {
          type = "nonstd::sv_lite::string_view";
        }

        content_define_str += type;
        content_define_str += ",";
      }
      // remove the ending ','
      content_define_str.erase(content_define_str.end() - 1, content_define_str.end());

      list_tiem_str += R"(
        typedef )" + tuple_define_str +
                       R"(> list_item;
        typedef )" + content_define_str +
                       "> item_content;";
    } else {
      tuple_types.push_back(hash_map_type_str);
    }

    hash_map_include_str += list_tiem_str;

    std::string hash_map_define_str = "std::make_shared<std::vector<list_item>>();";
    // TODO: fix multi columns case
    std::string condition_check_str;
    if (func_node) {
      condition_check_str =
          GetConditionCheckFunc(func_node, left_field_list, right_field_list,
                                &left_cond_index_list, &right_cond_index_list);
      cond_check = true;
    }
    auto process_probe_str =
        GetProcessProbe(join_type, cond_check, left_shuffle_index_list,
                        right_shuffle_index_list, right_key_index_list);
    auto left_cache_index_list =
        MergeKeyIndexList(left_cond_index_list, left_shuffle_index_list);
    auto right_cache_index_list =
        MergeKeyIndexList(right_cond_index_list, right_shuffle_index_list);

    std::vector<std::shared_ptr<TypedProberCodeGenImpl>> left_cache_codegen_list;
    std::vector<std::shared_ptr<TypedProberCodeGenImpl>> left_shuffle_codegen_list;
    std::vector<std::shared_ptr<TypedProberCodeGenImpl>> right_shuffle_codegen_list;
    GetTypedProberCodeGen("0_", true, left_cache_index_list, left_field_list, exist_index,
                          &left_cache_codegen_list);
    GetTypedProberCodeGen("0_", true, left_shuffle_index_list, left_field_list,
                          exist_index, &left_shuffle_codegen_list);
    GetTypedProberCodeGen("1_", false, right_shuffle_index_list, right_field_list,
                          exist_index, &right_shuffle_codegen_list, join_type);
    auto join_key_type_list_define_str =
        GetJoinKeyFieldListDefine(left_key_index_list, left_field_list);
    auto evaluate_cache_insert_str = GetEvaluateCacheInsert(left_cache_index_list);
    auto evaluate_encode_join_key_str = GetEncodeJoinKey(left_key_index_list);
    auto finish_cached_parameter_str = GetFinishCachedParameter(left_cache_index_list);
    auto impl_cached_define_str = GetImplCachedDefine(left_cache_codegen_list);
    auto result_iter_params_str = GetResultIteratorParams(left_cache_index_list);
    auto result_iter_set_str = GetResultIteratorSet(left_cache_index_list);
    auto result_iter_prepare_str =
        GetResultIteratorPrepare(left_shuffle_codegen_list, right_shuffle_codegen_list);
    auto process_right_set_str = GetProcessRightSet(right_cache_index_list);
    auto process_encode_join_key_str = GetEncodeJoinKey(right_key_index_list);
    auto process_finish_str =
        GetProcessFinish(left_shuffle_codegen_list, right_shuffle_codegen_list);
    auto process_out_list_str = GetProcessOutList(
        result_schema_index_list, left_shuffle_codegen_list, right_shuffle_codegen_list);
    auto result_iter_cached_define_str =
        GetResultIterCachedDefine(left_cache_codegen_list, right_shuffle_codegen_list);
    auto evaluate_get_typed_array_str = GetTypedArray(
        multiple_cols, left_key_index_list[0], left_key_index_list, left_field_list);
    auto process_get_typed_array_str = GetTypedArray(
        multiple_cols, right_key_index_list[0], right_key_index_list, right_field_list);
    auto evaluate_get_typed_array_str1 = GetTypedArray(
        multiple_cols, "0_" + std::to_string(left_key_index_list[0]),
        left_key_index_list[0],
        GetTypeString(left_field_list[left_key_index_list[0]]->type(), "Array"),
        evaluate_encode_join_key_str);
    auto process_get_typed_array_str1 = GetTypedArray(
        multiple_cols, "1_" + std::to_string(right_key_index_list[0]),
        right_key_index_list[0],
        GetTypeString(left_field_list[left_key_index_list[0]]->type(), "Array"),
        process_encode_join_key_str);
    auto make_tuple_str = GetTupleStr(multiple_cols, left_key_index_list.size());
    auto make_idarray_str = GetIdArrayStr(cond_check, join_type);
    auto make_list_str = GetListStr(multiple_cols, left_key_index_list.size());
    auto make_list_content_str =
        GetListContentStr(multiple_cols, left_key_index_list.size());

    return BaseCodes() + R"(
#include <numeric>
#include "codegen/arrow_compute/ext/array_item_index.h"
#include "precompile/builder.h"
using namespace sparkcolumnarplugin::precompile;
)" + hash_map_include_str +
           R"(
class FVector {

public:
FVector(std::vector<list_item>* left_list, std::vector<int64_t> segment_info) {
  left_list_ = left_list;
  total_len_ = std::accumulate(segment_info.begin(), segment_info.end(), 0);
  segment_info_ = segment_info;
  it = (*left_list_)[0];
  segment_len = 0;
}

void getpos(int64_t* cur_idx, int64_t* cur_segmeng_len, int64_t *passlen) {
  *cur_idx = idx;
  *cur_segmeng_len = segment_len;
  *passlen = passed_len;
}

void setpos(int64_t cur_idx, int64_t cur_segment_len, int64_t cur_passed_len) {
  idx = cur_idx;
  it = (*left_list_)[idx];
  segment_len = cur_segment_len;
  passed_len = cur_passed_len;
}

item_content value() {
  )" + make_list_content_str +
           R"(}

bool hasnext() {
 return (passed_len <= total_len_-1);
}

void next() {
  segment_len++;
  if (segment_len == segment_info_[idx] && passed_len != total_len_ -1) {
    idx++;
    segment_len = 0;
    it = (*left_list_)[idx];
  }
  passed_len++;
}

public:
std::vector<list_item>* left_list_;
int64_t total_len_;
std::vector<int64_t> segment_info_;

int64_t idx = 0;
int64_t passed_len = 0;
int64_t segment_len = 0;
list_item it;
};


class TypedProberImpl : public CodeGenBase {
 public:
  TypedProberImpl(arrow::compute::FunctionContext *ctx) : ctx_(ctx) {
  }
  ~TypedProberImpl() {}

  arrow::Status Evaluate(const ArrayList& in) override {
    )" + evaluate_cache_insert_str +
           evaluate_get_typed_array_str + make_list_str +
           R"(

    idx_to_arrarid_.push_back(typed_array_0->length());

    return arrow::Status::OK();
  }

  arrow::Status MakeResultIterator(
      std::shared_ptr<arrow::Schema> schema,
      std::shared_ptr<ResultIterator<arrow::RecordBatch>> *out) override {
    *out = std::make_shared<ProberResultIterator>(
        ctx_, schema, &left_list_, &idx_to_arrarid_)" +
           finish_cached_parameter_str + R"(
    );
    return arrow::Status::OK();
  }

private:
  uint64_t cur_array_id_ = 0;
  uint64_t cur_id_ = 0;
  uint64_t idx = 0;
  uint64_t num_items_ = 0;
  arrow::compute::FunctionContext *ctx_;
  std::vector<list_item> left_list_;
  std::vector<int64_t> idx_to_arrarid_;
  )" + impl_cached_define_str +
           R"( 

  class ProberResultIterator : public ResultIterator<arrow::RecordBatch> {
  public:
    ProberResultIterator(
        arrow::compute::FunctionContext *ctx,
        std::shared_ptr<arrow::Schema> schema,
        std::vector<list_item>* left_list,
        std::vector<int64_t> *idx_to_arrarid)" +
           result_iter_params_str + R"(
        )
        : ctx_(ctx), result_schema_(schema), left_list_(left_list), last_pos(0), idx_to_arrarid_(idx_to_arrarid) {
            )" +
           result_iter_set_str + result_iter_prepare_str + R"(

            left_it = std::make_shared<FVector>(left_list_, *idx_to_arrarid_);
    }

    std::string ToString() override { return "ProberResultIterator"; }

    ArrayItemIndex GetArrayItemIdex(std::shared_ptr<FVector> left_it) {
      int64_t local_arrayid, local_seglen, local_pl;
      left_it->getpos(&local_arrayid, &local_seglen, &local_pl);
      return ArrayItemIndex(local_arrayid, local_seglen);
    }

    arrow::Status
    Process(const ArrayList &in, std::shared_ptr<arrow::RecordBatch> *out,
            const std::shared_ptr<arrow::Array> &selection) override {
      uint64_t out_length = 0;
      )" + process_right_set_str +
           process_get_typed_array_str +
           R"(
      auto length = cached_1_0_->length();
      left_it->setpos(last_idx, last_seg, last_pl);
      int last_match_idx = -1;

      for (int i = 0; i < length; i++) {)" +
           process_probe_str + R"(
      }
      )" + process_finish_str +
           R"(
      left_it->getpos(&last_idx, &last_seg, &last_pl);
      *out = arrow::RecordBatch::Make(
          result_schema_, out_length,
          {)" +
           process_out_list_str + R"(});
      //arrow::PrettyPrint(*(*out).get(), 2, &std::cout);
      return arrow::Status::OK();
    }

  private:
    arrow::compute::FunctionContext *ctx_;
    std::shared_ptr<arrow::Schema> result_schema_;
    std::vector<list_item>* left_list_;
    std::shared_ptr<FVector> left_it;
    int64_t last_pos;
    int64_t last_idx = 0;
    int64_t last_seg = 0;
    int64_t last_pl = 0;
    std::vector<int64_t> *idx_to_arrarid_;
)" + result_iter_cached_define_str +
           R"(
      )" + condition_check_str +
           R"(
  };
};

extern "C" void MakeCodeGen(arrow::compute::FunctionContext *ctx,
                            std::shared_ptr<CodeGenBase> *out) {
  *out = std::make_shared<TypedProberImpl>(ctx);
}
    )";
  }
};

arrow::Status ConditionedJoinArraysKernel::Make(
    arrow::compute::FunctionContext* ctx,
    const std::vector<std::shared_ptr<arrow::Field>>& left_key_list,
    const std::vector<std::shared_ptr<arrow::Field>>& right_key_list,
    const std::shared_ptr<gandiva::Node>& func_node, int join_type,
    const std::vector<std::shared_ptr<arrow::Field>>& left_field_list,
    const std::vector<std::shared_ptr<arrow::Field>>& right_field_list,
    const std::shared_ptr<arrow::Schema>& result_schema,
    std::shared_ptr<KernalBase>* out) {
  *out = std::make_shared<ConditionedJoinArraysKernel>(
      ctx, left_key_list, right_key_list, func_node, join_type, left_field_list,
      right_field_list, result_schema);
  return arrow::Status::OK();
}

// TODO: enable on multiple keys
template <typename ArrowType>
class OperatorConditionedJoinArraysKernel : public ConditionedJoinArraysKernel::Impl {
 public:
  OperatorConditionedJoinArraysKernel(
      arrow::compute::FunctionContext* ctx,
      const std::vector<std::shared_ptr<arrow::Field>>& left_key_list,
      const std::vector<std::shared_ptr<arrow::Field>>& right_key_list,
      const std::shared_ptr<gandiva::Node>& func_node, int join_type,
      const std::vector<std::shared_ptr<arrow::Field>>& left_field_list,
      const std::vector<std::shared_ptr<arrow::Field>>& right_field_list,
      const std::shared_ptr<arrow::Schema>& result_schema)
      : ctx_(ctx), result_schema(result_schema) {}
  arrow::Status Evaluate(const ArrayList& in) override {
    col_num_ = result_schema->num_fields();
    for (int i = 0; i < col_num_; i++) {
      cached_[i].push_back(in[i]);
    }
    auto typed_array_0 = std::dynamic_pointer_cast<ArrayType>(in[0]);
    left_list_.emplace_back(typed_array_0);
    idx_to_arrarid_.push_back(typed_array_0->length());
    return arrow::Status::OK();
  }

  arrow::Status MakeResultIterator(
      std::shared_ptr<arrow::Schema> schema,
      std::shared_ptr<ResultIterator<arrow::RecordBatch>>* out) override {
    return arrow::Status::OK();
  }

 private:
  // using ArrowType = typename std::tuple_element<0, std::tuple<Args...> >::type;
  using ArrayType = typename arrow::TypeTraits<ArrowType>::ArrayType;
  std::vector<std::shared_ptr<ArrayType>> left_list_;
  std::vector<int64_t> idx_to_arrarid_;
  arrow::compute::FunctionContext* ctx_;
  std::shared_ptr<CodeGenBase> prober_;
  std::string signature_;
  int col_num_;
  std::vector<arrow::ArrayVector> cached_;
  std::shared_ptr<arrow::Schema> result_schema;

  class FVector {
    using ViewType = decltype(std::declval<ArrayType>().GetView(0));

   public:
    FVector(std::vector<std::shared_ptr<ArrayType>>* left_list,
            std::vector<int64_t> segment_info) {
      left_list_ = left_list;
      total_len_ = std::accumulate(segment_info.begin(), segment_info.end(), 0);
      segment_info_ = segment_info;
      it = (*left_list_)[0];
      segment_len = 0;
    }

    void getpos(int64_t* cur_idx, int64_t* cur_segmeng_len, int64_t* passlen) {
      *cur_idx = idx;
      *cur_segmeng_len = segment_len;
      *passlen = passed_len;
    }

    void setpos(int64_t cur_idx, int64_t cur_segment_len, int64_t cur_passed_len) {
      idx = cur_idx;
      it = (*left_list_)[idx];
      segment_len = cur_segment_len;
      passed_len = cur_passed_len;
    }

    ViewType value() { return it->GetView(segment_len); }

    bool hasnext() { return (passed_len <= total_len_ - 1); }

    void next() {
      segment_len++;
      if (segment_len == segment_info_[idx] && passed_len != total_len_ - 1) {
        idx++;
        segment_len = 0;
        it = (*left_list_)[idx];
      }
      passed_len++;
    }

   public:
    std::vector<std::shared_ptr<ArrayType>>* left_list_;
    int64_t total_len_;
    std::vector<int64_t> segment_info_;

    int64_t idx = 0;
    int64_t passed_len = 0;
    int64_t segment_len = 0;
    std::shared_ptr<ArrayType> it;
  };

  class ProberResultIterator : public ResultIterator<arrow::RecordBatch> {
   public:
    ProberResultIterator(arrow::compute::FunctionContext* ctx,
                         std::shared_ptr<arrow::Schema> schema,
                         std::vector<std::shared_ptr<ArrayType>>* left_list,
                         std::vector<int64_t>* idx_to_arrarid,
                         std::vector<arrow::ArrayVector> cached_in)
        : ctx_(ctx),
          result_schema_(schema),
          left_list_(left_list),
          last_pos(0),
          idx_to_arrarid_(idx_to_arrarid),
          cached_in_(cached_in) {
      AppenderBase::AppenderType appender_type = AppenderBase::left;
      for (int i = 0; i < col_num_; i++) {
        auto field = schema->field(i);
        std::shared_ptr<AppenderBase> appender;
        MakeAppender(ctx_, field->type(), appender_type, &appender);
        appender_list_.push_back(appender);
      }
      for (int i = 0; i < col_num_; i++) {
        arrow::ArrayVector array_vector = cached_in_[i];
        int array_num = array_vector.size();
        for (int array_id = 0; array_id < array_num; array_id++) {
          auto arr = array_vector[array_id];
          appender_list_[i]->AddArray(arr);
        }
      }
      left_it = std::make_shared<FVector>(left_list_, *idx_to_arrarid_);
    }

    ArrayItemIndex GetArrayItemIdex(std::shared_ptr<FVector> left_it) {
      int64_t local_arrayid, local_seglen, local_pl;
      left_it->getpos(&local_arrayid, &local_seglen, &local_pl);
      return ArrayItemIndex(local_arrayid, local_seglen);
    }

    arrow::Status Process(const ArrayList& in, std::shared_ptr<arrow::RecordBatch>* out,
                          const std::shared_ptr<arrow::Array>& selection) override {
      uint64_t out_length = 0;
      auto typed_array_0 = std::dynamic_pointer_cast<ArrayType>(in[0]);
      auto length = typed_array_0->length();
      for (int i = 0; i < length; i++) {
        auto right_content = typed_array_0->GetView(i);
        while (left_it->hasnext() && left_it->value() < right_content) {
          left_it->next();
        }
        while (left_it->hasnext() && left_it->value() == right_content) {
          auto item = GetArrayItemIdex(left_it);

          for (auto& appender : appender_list_) {
            RETURN_NOT_OK(appender->Append(item->array_id, item->id));
          }
          out_length += 1;
        }
      }
      ArrayList arrays;
      for (auto& appender : appender_list_) {
        std::shared_ptr<arrow::Array> out_array;
        RETURN_NOT_OK(appender->Finish(&out_array));
        arrays.push_back(out_array);
        appender->Reset();
      }

      *out = arrow::RecordBatch::Make(result_schema_, out_length, arrays);
      return arrow::Status::OK();
    }

   private:
    arrow::compute::FunctionContext* ctx_;
    std::shared_ptr<arrow::Schema> result_schema_;
    std::vector<std::shared_ptr<ArrayType>>* left_list_;
    std::shared_ptr<FVector> left_it;
    int64_t last_pos;
    int64_t last_idx = 0;
    int64_t last_seg = 0;
    int64_t last_pl = 0;
    std::vector<int64_t>* idx_to_arrarid_;
    std::vector<arrow::ArrayVector> cached_in_;
    std::vector<std::shared_ptr<AppenderBase>> appender_list_;
  };
};

ConditionedJoinArraysKernel::ConditionedJoinArraysKernel(
    arrow::compute::FunctionContext* ctx,
    const std::vector<std::shared_ptr<arrow::Field>>& left_key_list,
    const std::vector<std::shared_ptr<arrow::Field>>& right_key_list,
    const std::shared_ptr<gandiva::Node>& func_node, int join_type,
    const std::vector<std::shared_ptr<arrow::Field>>& left_field_list,
    const std::vector<std::shared_ptr<arrow::Field>>& right_field_list,
    const std::shared_ptr<arrow::Schema>& result_schema) {
  // using ArrayType = decltype(left_field_list[0]->type());
  if (left_key_list.size() == 1 && right_key_list.size() == 1) {
    impl_.reset(new OperatorConditionedJoinArraysKernel<arrow::StringType>(
        ctx, left_key_list, right_key_list, func_node, join_type, left_field_list,
        right_field_list, result_schema));
  } else {
    impl_.reset(new Impl(ctx, left_key_list, right_key_list, func_node, join_type,
                         left_field_list, right_field_list, result_schema));
  }
  kernel_name_ = "ConditionedJoinArraysKernel";
}

arrow::Status ConditionedJoinArraysKernel::Evaluate(const ArrayList& in) {
  return impl_->Evaluate(in);
}

arrow::Status ConditionedJoinArraysKernel::MakeResultIterator(
    std::shared_ptr<arrow::Schema> schema,
    std::shared_ptr<ResultIterator<arrow::RecordBatch>>* out) {
  return impl_->MakeResultIterator(schema, out);
}

std::string ConditionedJoinArraysKernel::GetSignature() { return impl_->GetSignature(); }

}  // namespace extra
}  // namespace arrowcompute
}  // namespace codegen
}  // namespace sparkcolumnarplugin
