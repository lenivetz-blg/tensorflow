/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_JIT_XLA_COMPILATION_CACHE_H_
#define TENSORFLOW_COMPILER_JIT_XLA_COMPILATION_CACHE_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/inlined_vector.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "tensorflow/compiler/jit/device_compilation_profiler.h"
#include "tensorflow/compiler/jit/xla_compilation_cache.pb.h"
#include "tensorflow/compiler/jit/xla_compile_util.h"
#include "tensorflow/compiler/tf2xla/xla_compiler.h"
#include "tensorflow/compiler/tf2xla/xla_context.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/xla/service/hlo.pb.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/core/common_runtime/device.h"
#include "tensorflow/core/common_runtime/device_mgr.h"
#include "tensorflow/core/framework/device.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/lib/core/threadpool.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/thread_annotations.h"
#include "tensorflow/core/protobuf/meta_graph.pb.h"

namespace tensorflow {

// The XlaCompilationCache class caches the results of the XlaCompiler class,
// which converts a Tensorflow graph into a compiled XLA compilation.
//
// Since XLA computations must have static shapes, the cache generates a new
// XLA computation for each new set of input shapes.
//
// Currently no cache eviction policy is implemented and the cache grows without
// bound.
class XlaCompilationCache : public ResourceBase {
 public:
  struct Config {
    Config() = default;
    explicit Config(absl::string_view persistent_cache_directory,
                    bool disable_strict_signature_checks,
                    absl::string_view persistance_prefix)
        : persistent_cache_directory(persistent_cache_directory),
          disable_strict_signature_checks(disable_strict_signature_checks),
          persistance_prefix(persistance_prefix) {}

    // If non-empty, JIT-compiled executables are saved to and loaded from the
    // specified file system directory path.
    std::string persistent_cache_directory;

    // Disable strict signature checks for entries loaded into the cache from
    // external sources.
    bool disable_strict_signature_checks = false;

    // The cache persistence prefix to use if serializing/deserialzing entries.
    std::string persistance_prefix;
  };
  XlaCompilationCache(Config config, xla::LocalClient* client,
                      DeviceType device_type);
  ~XlaCompilationCache() override;

  enum class CompileState { kUncompiled, kCompiling, kCompiled };

  enum class CompileScope {
    kOp,
    kFunction,
  };

  // Compiles a function into a XlaCompiler::CompilationResult that can be used
  // to execute an XLA Computation. Compilation results are cached.
  // `function` is the name of a Tensorflow function to compile.
  // `args` is a description of the arguments to the computation.
  //
  // `compile_mode` controls the behavior of the compilation cache on a cache
  // miss.  If `compile_mode` is `kLazy` then, based on some profitability
  // heuristics, the compilation cache may decide not to compile the cluster at
  // this time.  In this case it returns null into both `out_compilation_result`
  // and `out_executable`.  If `compile_mode` is `kStrict` then the compilation
  // cache always attempts the compilation on a cache miss. If compilation mode
  // is 'kAsync' compilation of the cluster happens in the background while the
  // fallback path executes.
  //
  // The result of compilation is written to `*out_compilation_result`, which
  // must be non-null. If `out_executable` is non-null, also builds an
  // xla::LocalExecutable and sets `out_executable` to point to it. The
  // resulting executable pointer may be null if the computation has no
  // non-constant outputs.
  Status Compile(const XlaCompiler::Options& options,
                 const NameAttrList& function,
                 const std::vector<XlaCompiler::Argument>& args,
                 const XlaCompiler::CompileOptions& compile_options,
                 DeviceCompileMode compile_mode,
                 DeviceCompilationProfiler* profiler,
                 const XlaCompiler::CompilationResult** out_compilation_result,
                 xla::LocalExecutable** out_executable);

  // As above, but for a single op.
  Status CompileSingleOp(
      const XlaCompiler::Options& options,
      const std::vector<XlaCompiler::Argument>& args,
      const XlaCompiler::CompileOptions& compile_options, OpKernelContext* ctx,
      DeviceCompilationProfiler* profiler,
      const XlaCompiler::CompilationResult** out_compilation_result,
      xla::LocalExecutable** out_executable);

  struct CompilationResultAndExecutable {
    const XlaCompiler::CompilationResult* compilation_result;
    xla::LocalExecutable* executable;
  };

  // Returns CompilationResultAndExecutable with non-null compilation_result and
  // executable if the signature is already compiled.
  // If the signature has not been compiled yet, this function returns a
  // CompilationResultAndExecutable instance with only nullptrs in it.
  // Non-ok status means something other than the 2 circumstances above
  // happened.
  StatusOr<CompilationResultAndExecutable>
  GetCompilationResultIfAlreadyCompiled(
      const NameAttrList& function,
      absl::Span<const XlaCompiler::Argument> args);

  xla::LocalClient* client() const { return client_; }
  const DeviceType& device_type() const { return device_type_; }

  string DebugString() const override;

  // Describes the types, shapes and any compile-time constant arguments
  // to a kernel. Key that uniquely identifies a compilation output.
  struct Signature {
    string name;

    // List of args (either as a TensorTypeAndShape or as a Tensor value)
    // for compile-time constant arguments to the compilation, ordered by
    // argument number. Tensors must be in host memory.
    using TensorTypeAndShape =
        std::pair<DataType, absl::InlinedVector<int64_t, 4>>;
    absl::InlinedVector<absl::variant<Tensor, TensorTypeAndShape>, 8> args;

    bool operator==(const Signature& other) const;

    struct Hash {
      uint64 operator()(const Signature& signature) const;
    };

    // Returns a human-readable description of the signature.
    string HumanString() const;
  };

  // Builds the signature for a compilation.
  static StatusOr<Signature> BuildSignature(
      const NameAttrList& function,
      absl::Span<const XlaCompiler::Argument> args);

 private:
  // Common implementation of Compile and CompileSingleOp. The `OpKernelContext`
  // parameter is always null for the former.
  Status CompileImpl(
      const XlaCompiler::CompileOptions& compile_options,
      const XlaCompiler::Options& options, const NameAttrList& function,
      const std::vector<XlaCompiler::Argument>& args, CompileScope scope,
      DeviceCompileMode compile_mode, OpKernelContext* ctx,
      DeviceCompilationProfiler* profiler,
      const XlaCompiler::CompilationResult** out_compilation_result,
      xla::LocalExecutable** out_executable);

  // Takes `result` which has been compiled from a Tensorflow subgraph to a
  // XLA computation already, and generates an XLA LocalExecutable `executable`.
  Status BuildExecutable(const XlaCompiler::Options& options,
                         const XlaCompiler::CompilationResult& result,
                         std::unique_ptr<xla::LocalExecutable>* executable);

  // Like BuildExecutable above, except that it generates an XLA
  // AotCompilationResult (instead of LocalExecutable), which can be persisted
  // to later load a LocalExecutable using the LoadExecutable() method below.
  StatusOr<std::unique_ptr<xla::AotCompilationResult>>
  BuildSerializedExecutable(const XlaCompiler::Options& options,
                            const XlaCompiler::CompilationResult& result);

  // Returns an XLA LocalExecutable loaded from a serialized XLA
  // AotCompilationResult.
  StatusOr<std::unique_ptr<xla::LocalExecutable>> LoadExecutable(
      const XlaCompiler::Options& options,
      const XlaCompiler::CompilationResult& result,
      const std::string& serialized_aot_result);

  xla::LocalClient* const client_;
  const DeviceType device_type_;
  bool disable_strict_signature_checks_;
  std::string persistance_prefix_;

  // The value associated with a cache entry.
  struct Entry {
    mutex mu;

    // The current compilation state for this entry.
    CompileState compile_state = CompileState::kUncompiled;

    // The number of times a compilation with this signature has been requested.
    int64_t request_count = 0;

    // Did compilation succeed?
    Status compilation_status TF_GUARDED_BY(mu);

    // Output of the XlaCompiler.
    XlaCompiler::CompilationResult compilation_result TF_GUARDED_BY(mu);

    // The XLA executable compiled from <computation>. May be null if no
    // executable has been built.
    std::unique_ptr<xla::LocalExecutable> executable TF_GUARDED_BY(mu);
  };

  // Returns a cache key proto that identifies an entry in the compilation
  // cache.
  XlaSerializedCacheKey BuildSerializedCacheKey(
      const Signature& sig, const xla::HloModuleProto& hlo_module) const;

  // Serializes the signature and its corresponding entry to a proto message.
  StatusOr<XlaSerializedCacheEntry> SerializeEntry(
      const XlaCompiler::Options& options, const Signature& sig,
      const Entry& entry) TF_EXCLUSIVE_LOCKS_REQUIRED(entry.mu);

  // Checks if the loaded `entry` matches the expected `key` and `hlo_module`.
  Status VerifyLoadedCacheEntry(const XlaSerializedCacheKey& key,
                                const xla::HloModuleProto& hlo_module,
                                const XlaSerializedCacheEntry& entry);

  Status CompileStrict(const Signature& sig,
                       const XlaCompiler::CompileOptions& compile_options,
                       const XlaCompiler::Options& options,
                       const std::vector<XlaCompiler::Argument>& args,
                       const NameAttrList& function, CompileScope scope,
                       OpKernelContext* ctx,
                       DeviceCompilationProfiler* profiler, Entry* entry)
      TF_EXCLUSIVE_LOCKS_REQUIRED(entry->mu);
  Status CompileAsynchronous(const Signature& sig,
                             const XlaCompiler::CompileOptions& compile_options,
                             const XlaCompiler::Options& options,
                             const std::vector<XlaCompiler::Argument>& args,
                             const NameAttrList& function, CompileScope scope,
                             OpKernelContext* ctx,
                             DeviceCompilationProfiler* profiler, Entry* entry);

  // Saves the cache entry in the file directory supplied during the
  // construction of this class. Overwrites existing entries.
  Status SaveSerializedEntry(const XlaSerializedCacheEntry& entry);

  // Tries to load a cache entry given a `key` by searching the file directory
  // supplied during the construction of this class. Returns std::nullopt if no
  // cache entry is found.
  StatusOr<std::optional<XlaSerializedCacheEntry>> TryLoadSerializedEntry(
      const XlaSerializedCacheKey& key);

  mutex compile_cache_mu_;
  absl::flat_hash_map<Signature, std::unique_ptr<Entry>, Signature::Hash> cache_
      TF_GUARDED_BY(compile_cache_mu_);

  // If non-empty, JIT-compiled executables are saved to and loaded from the
  // specified file system directory path.
  std::string persistent_cache_directory_;

  // Pool of threads for asynchronous compilations.
  std::unique_ptr<thread::ThreadPool> async_compiler_threads_;

  TF_DISALLOW_COPY_AND_ASSIGN(XlaCompilationCache);
};

// Generates the ExecutableBuildOptions for compliation from HLO to executable.
xla::ExecutableBuildOptions GetExecutableBuildOptions(
    const XlaCompiler::Options& options,
    const XlaCompiler::CompilationResult& result, int default_device_ordinal);

}  // namespace tensorflow

#endif  // TENSORFLOW_COMPILER_JIT_XLA_COMPILATION_CACHE_H_
