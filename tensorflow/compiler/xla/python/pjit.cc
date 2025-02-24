/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/python/pjit.h"

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/synchronization/notification.h"
#include "tensorflow/compiler/xla/pjrt/lru_cache.h"
#ifdef JAX_ENABLE_IFRT
#include "tensorflow/compiler/xla/python/ifrt/array.h"
#endif
#include "tensorflow/compiler/xla/python/jax_jit.h"
#include "tensorflow/compiler/xla/python/py_array.h"
#include "tensorflow/compiler/xla/python/py_executable.h"
#include "tensorflow/compiler/xla/python/py_values.h"
#include "tensorflow/compiler/xla/python/sharding.h"
#include "tensorflow/compiler/xla/python/status_casters.h"
#include "tensorflow/tsl/profiler/lib/traceme.h"

namespace jax {
namespace {

namespace py = pybind11;

struct PjitCacheEntry {
  std::shared_ptr<xla::PyLoadedExecutable> executable;
  std::vector<py::object> in_shardings;
  std::vector<py::object> out_avals;
  std::vector<py::dtype> out_dtypes;
  std::vector<std::vector<int64_t>> out_shapes;
  std::vector<bool> out_weak_types;
  std::vector<py::object> out_shardings;
  std::vector<bool> out_committed;
  xla::PyTreeDef out_pytree_def;

  // Ensures a single thread performs the compilation for a given executable.
  //
  // The first thread (holding the GIL) will create the CacheEntry associated to
  // a signature and if the object has been inserted already, other threads
  // will wait for the notification.
  absl::Notification compilation_complete;

  bool fall_back_to_python = false;
};

class PjitFunction {
 public:
  PjitFunction(std::string function_name, py::function cache_miss,
               std::vector<int> static_argnums, int executables_cache_size)
      : function_name_(std::move(function_name)),
        cache_miss_(std::move(cache_miss)),
        static_argnums_(std::move(static_argnums)),
        lru_list_(std::make_unique<Cache::LRUList>(executables_cache_size)),
        executables_(std::make_unique<Cache>(lru_list_.get())) {}

  PjitFunction(const PjitFunction&) = delete;
  PjitFunction& operator=(const PjitFunction&) = delete;
  PjitFunction(PjitFunction&&) = default;
  PjitFunction& operator=(PjitFunction&&) = default;

  xla::StatusOr<py::object> Call(py::args args, py::kwargs kwargs);

  using Cache = xla::LRUCache<CallSignature, std::shared_ptr<PjitCacheEntry>>;

 private:
  xla::Status UpdateArgsSignature(const py::args& args,
                                  const py::kwargs& kwargs,
                                  ParsedArgumentsAsBuffers& arguments);

  void PopulateCacheEntry(PjitCacheEntry& cache_entry,
                          const CallSignature& signature,
                          const py::tuple& out_and_fastpath_data);

  std::string function_name_;
  py::function cache_miss_;
  std::vector<int> static_argnums_;

  std::unique_ptr<Cache::LRUList> lru_list_;
  std::unique_ptr<Cache> executables_;
};

// Prepares the input PjRtBuffers from the python arguments. This is equivalent
// to shard_args() in pxla.py but for only a few supported cases.
#ifdef JAX_ENABLE_IFRT
xla::StatusOr<std::vector<xla::ifrt::Array*>> PrepareIfrtInputs(
    const xla::PyLoadedExecutable& executable,
    ParsedArgumentsAsBuffers& arguments) {
#else
xla::StatusOr<std::vector<std::vector<xla::PjRtBuffer*>>> PreparePjRtInputs(
    const xla::PyLoadedExecutable& executable,
    ParsedArgumentsAsBuffers& arguments) {
#endif
  const auto& addressable_devices = executable.AddressableDevices();
  int num_args = arguments.flat_dynamic_args.size();

#ifdef JAX_ENABLE_IFRT
  std::vector<xla::ifrt::Array*> num_args_arrays(num_args);
#else
  std::vector<std::vector<xla::PjRtBuffer*>> num_computation_num_args_buffers(
      addressable_devices.size());

  for (int i = 0; i < addressable_devices.size(); ++i) {
    num_computation_num_args_buffers[i].resize(num_args);
  }
#endif

  for (int i = 0; i < num_args; ++i) {
    const py::object& arg = arguments.flat_dynamic_args[i];

    xla::PyArray py_array = arg;
    const auto& sharding = py_array.sharding();
    auto* cpp_sharding = sharding.cast<jax::Sharding*>();

    // Currently only committed PyArray inputs or uncommitted PyArray on a
    // single device inputs are allowed. This is checked previously in the entry
    // point of PjitFunction::Call().
    DCHECK(py_array.committed() ||
           (!py_array.committed() && cpp_sharding->num_devices() == 1));

    if (sharding.get_type() == jax::PmapSharding::type()) {
      return xla::Unimplemented(
          "Handling PyArray in PmapSharding is not implemented.");
    }

    if (py_array.num_shards() != addressable_devices.size()) {
      return xla::InvalidArgument(
          "Expected PyArray to have %d shards, but got %d",
          addressable_devices.size(), py_array.num_shards());
    }

#ifdef JAX_ENABLE_IFRT
    xla::ifrt::Array* ifrt_array = py_array.ifrt_array();
    // PyArray inputs should have already been checked in
    // `xla::PyArgSignatureOfValue()` called by
    // `PjitFunction::UpdateArgsSignature()`.
    DCHECK(ifrt_array != nullptr) << "PyArray has been unexpectedly deleted.";

    if (cpp_sharding->num_devices() == 1 &&
        ifrt_array->sharding().devices().front() !=
            addressable_devices[0].get()) {
      xla::ifrt::DeviceList::Devices ifrt_devices;
      ifrt_devices.push_back(addressable_devices[0].get());
      auto sharding = xla::ifrt::OpaqueSharding::Create(
          xla::ifrt::DeviceList(std::move(ifrt_devices)));
      TF_ASSIGN_OR_RETURN(
          auto copied_ifrt_array,
          ifrt_array->Reshard(std::move(sharding),
                              xla::ifrt::ArrayCopySemantics::kReuseInput));
      num_args_arrays[i] = copied_ifrt_array.get();
      arguments.ifrt_keep_alive.push_back(std::move(copied_ifrt_array));
    } else {
      num_args_arrays[i] = ifrt_array;
    }
#else
    if (cpp_sharding->num_devices() == 1) {
      for (int j = 0; j < addressable_devices.size(); ++j) {
        auto* to_device = addressable_devices[j].get();
        xla::PjRtBuffer* buffer = py_array.pjrt_buffer(j);

        if (buffer->device() == to_device) {
          num_computation_num_args_buffers[j][i] = buffer;
        } else {
          TF_ASSIGN_OR_RETURN(std::unique_ptr<xla::PjRtBuffer> copied_buffer,
                              buffer->CopyToDevice(to_device));
          num_computation_num_args_buffers[j][i] = copied_buffer.get();
          arguments.keep_alive.push_back(std::move(copied_buffer));
        }
      }
    } else {
      for (int j = 0; j < addressable_devices.size(); ++j) {
        num_computation_num_args_buffers[j][i] = py_array.pjrt_buffer(j);
      }
    }
#endif

    arguments.keep_alive_objects.push_back(arg);
  }

#ifdef JAX_ENABLE_IFRT
  return num_args_arrays;
#else
  return num_computation_num_args_buffers;
#endif
}

xla::StatusOr<py::object> PjitFunction::Call(py::args args, py::kwargs kwargs) {
  tsl::profiler::TraceMe traceme(
      [&] { return absl::StrCat("JaxPjitFunction(", function_name_, ")"); });
  ParsedArgumentsAsBuffers arguments;

  auto status = ParseArguments(args, kwargs, static_argnums_,
                               /*static_argnames=*/{}, arguments);
  if (!status.ok()) {
    VLOG(2) << "ParseArguments failed: " << status;
    return py::object(py::cast<py::tuple>(cache_miss_(*args, **kwargs))[0]);
  }

  // Perform a few checks for the arguments. Currently we are only allowing
  // committed PyArray inputs. For other cases, e.g. Tracers or ShapedArray, it
  // will fallback to python.
  for (const auto& arg : arguments.flat_dynamic_args) {
    if (arg.get_type() != xla::PyArray::type()) {
      VLOG(2) << "Only PyArray arguments are supported in cpp pjit, but got: "
              << py::cast<std::string>(arg.get_type().str());
      return py::object(py::cast<py::tuple>(cache_miss_(*args, **kwargs))[0]);
    }
    xla::PyArray py_array = arg;
    if (!py_array.fastpath_enabled()) {
      return py::object(py::cast<py::tuple>(cache_miss_(*args, **kwargs))[0]);
    }

    // Only allow committed PyArray in cpp pjit for now as the logic on handling
    // sharding for uncommited PyArray is complicated and still under
    // development.
    //
    // TODO(chky): Consider support uncommitted PyArray in cpp when the python
    // side stablizes.
    auto* cpp_sharding = py_array.sharding().cast<jax::Sharding*>();
    if (!py_array.committed() && cpp_sharding->num_devices() > 1) {
      VLOG(2) << "PyArray argument is not committed and number of global "
                 "devices is more than 1; fallback to python.";
      return py::object(py::cast<py::tuple>(cache_miss_(*args, **kwargs))[0]);
    }
  }

  status = UpdateArgsSignature(args, kwargs, arguments);
  if (!status.ok()) {
    VLOG(2) << "UpdateArgsSignature failed: " << status;
    return py::object(py::cast<py::tuple>(cache_miss_(*args, **kwargs))[0]);
  }

  bool inserted = false;
  std::shared_ptr<PjitCacheEntry> cache_entry =
      executables_->GetOrCreateIfAbsent(
          arguments.signature, [&inserted](const CallSignature& unused) {
            inserted = true;
            return std::make_shared<PjitCacheEntry>();
          });

  if (!cache_entry->compilation_complete.HasBeenNotified()) {
    // In case of several threads attempting to compile the executable, only
    // the one that inserted the item will perform the compilation.
    if (inserted) {
      py::object out_and_fastpath_data;
      py::tuple out_tuple;
      VLOG(2) << "Cache miss for " << arguments.signature.DebugString();
      try {
        // Calls Python and may release the GIL. May also throw if
        // compilation/tracing fails.
        out_and_fastpath_data = cache_miss_(*args, **kwargs);
        out_tuple = py::cast<py::tuple>(out_and_fastpath_data);

        PopulateCacheEntry(*cache_entry, arguments.signature, out_tuple);
      } catch (const std::exception& e) {
        LOG(ERROR) << "cache miss fail: " << e.what();
        cache_entry->fall_back_to_python = true;
        cache_entry->compilation_complete.Notify();
        throw;
      }
      cache_entry->compilation_complete.Notify();

      // We have already computed the result in the miss path so we can return
      // it. We are even *required* to do so if there are donated arguments,
      // because any donated buffers will now be invalid.
      return py::object(out_tuple[0]);
    } else {
      // Release the GIL while we wait, making sure the compile thread can
      // lock it.
      py::gil_scoped_release release;
      cache_entry->compilation_complete.WaitForNotification();
    }
  }

  if (cache_entry->fall_back_to_python) {
    VLOG(2) << "cpp pjit fallback to python.";
    return py::object(py::cast<py::tuple>(cache_miss_(*args, **kwargs))[0]);
  }

#ifdef JAX_ENABLE_IFRT
  // A vector of [num_inputs].
  auto num_args_arrays = PrepareIfrtInputs(*cache_entry->executable, arguments);
  if (!num_args_arrays.ok()) {
    VLOG(2) << "Failed to prepare IFRT inputs: " << num_args_arrays.status();
    return py::object(py::cast<py::tuple>(cache_miss_(*args, **kwargs))[0]);
  }

  // A vector of [num_outputs].
  std::vector<std::unique_ptr<xla::ifrt::Array>> output_arrays;
  {
    py::gil_scoped_release gil_release;
    TF_ASSIGN_OR_RETURN(
        auto result, cache_entry->executable->ifrt_executable()->Execute(
                         *num_args_arrays, cache_entry->executable->options(),
                         /*devices=*/std::nullopt));
    output_arrays = std::move(result.outputs);
  }

  auto traceback = xla::Traceback::Get();
  const auto& client = cache_entry->executable->client();

  // Convert the ifrt::Array objects to PyArray.
  int num_outputs = output_arrays.size();
  absl::InlinedVector<py::object, 4> outputs;
  outputs.reserve(num_outputs);
  for (int i = 0; i < num_outputs; ++i) {
    // Creating the PyArray result. In addition to the IFRT arrays, the metadata
    // like `aval` and `sharding` are retrieved from the cache for this
    // function, which are produced by the python path in `cache_miss`.
    xla::PyArray py_array(
        cache_entry->out_avals[i], cache_entry->out_weak_types[i],
        cache_entry->out_dtypes[i], cache_entry->out_shapes[i],
        cache_entry->out_shardings[i], cache_entry->executable->client(),
        traceback, std::move(output_arrays[i]),
        /*committed=*/cache_entry->out_committed.at(i), /*skip_checks=*/true);

    outputs.push_back(std::move(py_array));
  }
#else
  // A vector of [num_devices, num_inputs].
  auto num_computation_num_args_buffers =
      PreparePjRtInputs(*cache_entry->executable, arguments);
  if (!num_computation_num_args_buffers.ok()) {
    VLOG(2) << "Failed to prepare PjRt inputs: "
            << num_computation_num_args_buffers.status();
    return py::object(py::cast<py::tuple>(cache_miss_(*args, **kwargs))[0]);
  }
  int num_computations = num_computation_num_args_buffers->size();

  // A vector of [num_devices, num_outputs].
  std::vector<std::vector<std::unique_ptr<xla::PjRtBuffer>>> output_buffers;
  {
    py::gil_scoped_release gil_release;
    auto pjrt_executable = cache_entry->executable->pjrt_executable();
    TF_ASSIGN_OR_RETURN(
        output_buffers,
        pjrt_executable->Execute(*num_computation_num_args_buffers,
                                 cache_entry->executable->options()));
  }

  auto traceback = xla::Traceback::Get();
  const auto& client = cache_entry->executable->client();

  // Convert the PjRtBuffer objects to PyArray, and invert the order from
  // [num_devices, num_args] to [num_args, num_devices].
  int num_outputs = output_buffers[0].size();
  absl::InlinedVector<py::object, 4> outputs;
  outputs.reserve(num_outputs);
  for (int i = 0; i < num_outputs; ++i) {
    std::vector<std::shared_ptr<xla::PjRtBuffer>> pjrt_buffers;
    pjrt_buffers.reserve(num_computations);

    for (int j = 0; j < num_computations; ++j) {
      pjrt_buffers.push_back(std::move(output_buffers[j][i]));
    }

    // Creating the PyArray result. In addition to the PjRtBuffers, the metadata
    // like `aval` and `sharding` are retrieved from the cache for this
    // function, which are produced by the python path in `cache_miss`.
    xla::PyArray py_array(
        cache_entry->out_avals[i], cache_entry->out_weak_types[i],
        cache_entry->out_dtypes[i], cache_entry->out_shapes[i],
        cache_entry->out_shardings[i], cache_entry->executable->client(),
        traceback, std::move(pjrt_buffers),
        /*committed=*/cache_entry->out_committed.at(i), /*skip_checks=*/true);

    outputs.push_back(std::move(py_array));
  }
#endif

  py::object out = cache_entry->out_pytree_def.Unflatten(outputs);

  // If there is a post-hook function, call it with the inputs and the outputs.
  std::optional<py::object> post_hook = GetPostHook();
  if (post_hook) {
    (*post_hook)(py::cast(this), args, kwargs, out);
  }

  return out;
}

xla::Status PjitFunction::UpdateArgsSignature(
    const py::args& args, const py::kwargs& kwargs,
    ParsedArgumentsAsBuffers& arguments) {
  arguments.signature.function_name = function_name_;

  // Get dynamic argument signatures.
  JitState& global_state = jax::GlobalJitState();
  JitState& tls = jax::ThreadLocalJitState();
  bool jax_enable_x64 = GetEnableX64();

  arguments.signature.jax_enable_x64 = jax_enable_x64;

  auto& dynamic_arg_signatures = arguments.signature.dynamic_arg_signatures;
  dynamic_arg_signatures.reserve(arguments.flat_dynamic_args.size());
  auto& dynamic_arg_shardings = arguments.signature.dynamic_arg_shardings;
  dynamic_arg_shardings.reserve(arguments.flat_dynamic_args.size());

  for (py::handle arg : arguments.flat_dynamic_args) {
    TF_ASSIGN_OR_RETURN(auto signature,
                        xla::PyArgSignatureOfValue(arg, jax_enable_x64));
    arguments.signature.dynamic_arg_signatures.push_back(std::move(signature));

    // It should be already checked previously in the entry point of
    // PjitFunction::Call().
    DCHECK(arg.get_type() == xla::PyArray::type());

    auto py_array = py::reinterpret_borrow<xla::PyArray>(arg);

    arguments.signature.dynamic_arg_shardings.push_back(py_array.sharding());
    arguments.signature.committed_args.push_back(py_array.committed());
  }

  arguments.signature.thread_local_extra_jit_context = tls.extra_jit_context;
  arguments.signature.global_extra_jit_context = global_state.extra_jit_context;

  return xla::OkStatus();
}

void PjitFunction::PopulateCacheEntry(PjitCacheEntry& cache_entry,
                                      const CallSignature& signature,
                                      const py::tuple& out_and_fastpath_data) {
  DCHECK_EQ(out_and_fastpath_data.size(), 2);

  if (out_and_fastpath_data[1].is_none()) {
    LOG(ERROR) << "fastpath_data is none";
    cache_entry.fall_back_to_python = true;
    return;
  }

  py::tuple fastpath_data = py::cast<py::tuple>(out_and_fastpath_data[1]);

  cache_entry.executable = py::cast<std::shared_ptr<xla::PyLoadedExecutable>>(
      fastpath_data.attr("xla_executable"));

  py::list in_shardings = fastpath_data.attr("in_shardings");
  cache_entry.in_shardings.reserve(in_shardings.size());
  for (py::handle sharding : in_shardings) {
    cache_entry.in_shardings.push_back(
        py::reinterpret_borrow<py::object>(sharding));
  }

  py::list out_shardings = fastpath_data.attr("out_shardings");
  cache_entry.out_shardings.reserve(out_shardings.size());
  for (py::handle sharding : out_shardings) {
    cache_entry.out_shardings.push_back(
        py::reinterpret_borrow<py::object>(sharding));
  }

  py::list out_committed = fastpath_data.attr("out_committed");
  cache_entry.out_committed.reserve(out_committed.size());
  for (py::handle c : out_committed) {
    cache_entry.out_committed.push_back(py::cast<bool>(c));
  }

  py::list out_avals = fastpath_data.attr("out_avals");
  cache_entry.out_avals.reserve(out_avals.size());
  cache_entry.out_dtypes.reserve(out_avals.size());
  cache_entry.out_shapes.reserve(out_avals.size());
  cache_entry.out_weak_types.reserve(out_avals.size());
  for (py::handle aval : out_avals) {
    cache_entry.out_avals.push_back(py::reinterpret_borrow<py::object>(aval));
    cache_entry.out_dtypes.push_back(aval.attr("dtype"));
    cache_entry.out_shapes.push_back(
        py::cast<std::vector<int64_t>>(aval.attr("shape")));
    cache_entry.out_weak_types.push_back(
        py::cast<bool>(aval.attr("weak_type")));
  }

  cache_entry.out_pytree_def =
      py::cast<xla::PyTreeDef>(fastpath_data.attr("out_pytree_def"));
}

}  // namespace

void BuildPjitSubmodule(py::module& m) {
  py::class_<PjitFunction>(m, "PjitFunction", py::dynamic_attr())
      .def("__call__", &PjitFunction::Call);

  m.def("pjit", [](std::string function_name, py::function cache_miss,
                   std::vector<int> static_argnums) {
    return PjitFunction(std::move(function_name), std::move(cache_miss),
                        std::move(static_argnums),
                        /*executables_cache_size=*/4096);
  });
}

}  // namespace jax
