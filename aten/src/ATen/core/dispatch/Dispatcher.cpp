#include <ATen/core/dispatch/Dispatcher.h>
#include <sstream>

namespace c10 {

namespace detail {
class RegistrationListenerList final {
public:
  void addListener(std::unique_ptr<OpRegistrationListener> listener) {
    listeners_.push_back(std::move(listener));
  }

  void callOnOperatorRegistered(const OperatorHandle& op) {
    for (auto& listener : listeners_) {
      listener->onOperatorRegistered(op);
    }
  }

  void callOnOperatorDeregistered(const OperatorHandle& op) {
    for (auto& listener : listeners_) {
      listener->onOperatorDeregistered(op);
    }
  }
private:
  std::vector<std::unique_ptr<OpRegistrationListener>> listeners_;
};
}

OpRegistrationListener::~OpRegistrationListener() {}

Dispatcher::Dispatcher()
: operators_()
, operatorLookupTable_()
, backendFallbackKernels_()
, backendsWithoutFallthrough_(DispatchKeySet::FULL)
, listeners_(std::make_unique<detail::RegistrationListenerList>())
, mutex_() {}

Dispatcher::~Dispatcher() {}

C10_EXPORT Dispatcher& Dispatcher::singleton() {
  static Dispatcher _singleton;
  return _singleton;
}

c10::optional<OperatorHandle> Dispatcher::findSchema(const OperatorName& overload_name) {
  return operatorLookupTable_.read([&] (const ska::flat_hash_map<OperatorName, OperatorHandle>& operatorLookupTable) -> c10::optional<OperatorHandle> {
    auto found = operatorLookupTable.find(overload_name);
    if (found == operatorLookupTable.end()) {
      return c10::nullopt;
    }
    return found->second;
  });
}

OperatorHandle Dispatcher::findSchemaOrThrow(const char* name, const char* overload_name) {
  return findSchema({name, overload_name}).value();
}

// Postcondition: caller is responsible for disposing of registration when they
// are done
OperatorHandle Dispatcher::findOrRegisterWithSchema_(FunctionSchema&& schema) {
  const auto found = findSchema(schema.operator_name());
  if (found != c10::nullopt) {
    if (found->schema() != schema) {
      TORCH_CHECK(false, "Tried to register multiple operators with the same name and the same overload name but different schemas: ", schema, " vs ", found->schema());
    }
    if (schema.isDefaultAliasAnalysisKind()) {
      // just do nothing and let it pass.
    } else if (found->schema().isDefaultAliasAnalysisKind()) {
      found->operatorIterator_->op.updateSchemaAliasAnalysis(schema.aliasAnalysis());
    } else {
      // TODO: This error message is crappy
      TORCH_CHECK(
        found->schema().aliasAnalysis() == schema.aliasAnalysis(),
        "Tried to register multiple operators with the same schema but different alias analysis kind: ", toString(schema));
    }
    return *found;
  }

  OperatorName op_name = schema.operator_name();
  operators_.emplace_back(std::move(schema));
  OperatorHandle handle(--operators_.end());
  operatorLookupTable_.write([&] (ska::flat_hash_map<OperatorName, OperatorHandle>& operatorLookupTable) {
    operatorLookupTable.emplace(op_name, handle);
  });

  return handle;
}

// Postcondition: caller is responsible for disposing of registration when they
// are done
OperatorHandle Dispatcher::findOrRegisterWithName_(const OperatorName& op_name) {
  const auto found = findSchema(op_name);
  if (found != c10::nullopt) {
    return *found;
  }

  operators_.emplace_back(OperatorName(op_name));
  OperatorHandle handle(--operators_.end());
  operatorLookupTable_.write([&] (ska::flat_hash_map<OperatorName, OperatorHandle>& operatorLookupTable) {
    operatorLookupTable.emplace(op_name, handle);
  });

  return handle;
}


RegistrationHandleRAII Dispatcher::registerDef(FunctionSchema schema) {
  // we need a lock to avoid concurrent writes
  std::lock_guard<std::mutex> lock(mutex_);

  OperatorName op_name = schema.operator_name();

  auto op = findOrRegisterWithSchema_(std::move(schema));

  ++op.operatorIterator_->refcount;
  ++op.operatorIterator_->weak_refcount;
  if (1 == op.operatorIterator_->refcount) {
    // note: call listeners *after* operator is added, i.e. dispatcher is already valid for new op
    listeners_->callOnOperatorRegistered(op);
  }

  return RegistrationHandleRAII([this, op, op_name] {
    deregisterDef_(op, op_name);
  });
}

void Dispatcher::deregisterDef_(const OperatorHandle& op, const OperatorName& op_name) {
  // we need a lock to avoid concurrent writes
  std::lock_guard<std::mutex> lock(mutex_);

  TORCH_INTERNAL_ASSERT(op.schema().operator_name() == op_name);

  // reduce refcount and actually deregister if no references left
  TORCH_INTERNAL_ASSERT(op.operatorIterator_->refcount > 0);
  TORCH_INTERNAL_ASSERT(op.operatorIterator_->weak_refcount > 0);
  --op.operatorIterator_->refcount;
  --op.operatorIterator_->weak_refcount;
  if (0 == op.operatorIterator_->refcount) {
    // note: call listeners *before* operator is removed, i.e. dispatcher is still valid for removed op
    // TODO: check that listeners are not relying on prepareForDeregistration()
    // invariant
    listeners_->callOnOperatorDeregistered(op);
  }
  if (0 == op.operatorIterator_->weak_refcount) {
    // TODO: rename this to "assert deregistration invariants"
    op.operatorIterator_->op.prepareForDeregistration();
    operators_.erase(op.operatorIterator_);
    operatorLookupTable_.write([&] (ska::flat_hash_map<OperatorName, OperatorHandle>& operatorLookupTable) {
      operatorLookupTable.erase(op_name);
    });
  }
}

RegistrationHandleRAII Dispatcher::registerImpl(OperatorName op_name, c10::optional<DispatchKey> dispatch_key, KernelFunction kernel) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto op = findOrRegisterWithName_(op_name);

  auto kernel_handle = op.operatorIterator_->op.registerKernel(dispatch_key, std::move(kernel));

  ++op.operatorIterator_->weak_refcount;

  return RegistrationHandleRAII([this, op, op_name, dispatch_key, kernel_handle] {
    op.operatorIterator_->op.deregisterKernel_(dispatch_key, kernel_handle);
    deregisterImpl_(op, op_name);
  });
}

// NB: This doesn't actually deregister the op, that's handled by the lambda
// above
void Dispatcher::deregisterImpl_(const OperatorHandle& op, const OperatorName& op_name) {
  std::lock_guard<std::mutex> lock(mutex_);

  TORCH_INTERNAL_ASSERT(op.operator_name() == op_name);

  TORCH_INTERNAL_ASSERT(op.operatorIterator_->weak_refcount > 0);
  --op.operatorIterator_->weak_refcount;
  if (0 == op.operatorIterator_->weak_refcount) {
    // TODO: rename this to "assert deregistration invariants"
    op.operatorIterator_->op.prepareForDeregistration();
    operators_.erase(op.operatorIterator_);
    operatorLookupTable_.write([&] (ska::flat_hash_map<OperatorName, OperatorHandle>& operatorLookupTable) {
      operatorLookupTable.erase(op_name);
    });
  }
}

RegistrationHandleRAII Dispatcher::registerFallback(DispatchKey dispatchKey, KernelFunction kernel) {
  auto inserted = backendFallbackKernels_.setKernel(dispatchKey, std::move(kernel));
  TORCH_CHECK(inserted == impl::KernelFunctionTable::SetKernelResult::ADDED_NEW_KERNEL, "Tried to register a backend fallback kernel for ", dispatchKey, " but there was already one registered.");
  if (kernel.isFallthrough()) {
    backendsWithoutFallthrough_ = backendsWithoutFallthrough_.remove(dispatchKey);
  }

  return RegistrationHandleRAII([this, dispatchKey] {
    deregisterFallback_(dispatchKey);
  });
}

void Dispatcher::deregisterFallback_(DispatchKey dispatchKey) {
  auto result = backendFallbackKernels_.removeKernelIfExists(dispatchKey);
  backendsWithoutFallthrough_ = backendsWithoutFallthrough_.add(dispatchKey);
  TORCH_INTERNAL_ASSERT(result == impl::KernelFunctionTable::RemoveKernelIfExistsResult::REMOVED_KERNEL, "Tried to deregister a backend fallback kernel for ", dispatchKey, " but there was none registered.");
}


void Dispatcher::addRegistrationListener(std::unique_ptr<OpRegistrationListener> listener) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto iter = operators_.begin(); iter != operators_.end(); ++iter) {
    listener->onOperatorRegistered(OperatorHandle(iter));
  }

  listeners_->addListener(std::move(listener));
}

[[noreturn]] void Dispatcher::reportError(const DispatchTable& dispatchTable, DispatchKey dispatchKey) {
  if (dispatchKey == DispatchKey::Undefined) {
    TORCH_CHECK(false,
          "There were no tensor arguments to this function (e.g., you passed an "
          "empty list of Tensors), but no fallback function is registered for schema ", dispatchTable.operatorName(),
          ".  This usually means that this function requires a non-empty list of Tensors.  "
          "Available functions are ", dispatchTable.listAllDispatchKeys())
  }

  const std::string dispatchKeyStr = toString(dispatchKey);
  TORCH_CHECK(false, "Could not run '", dispatchTable.operatorName(), "' with arguments",
          " from the '", dispatchKeyStr, "' backend. '",
          dispatchTable.operatorName(), "' is only available for these backends: ",
          dispatchTable.listAllDispatchKeys(), ".");
}

}
