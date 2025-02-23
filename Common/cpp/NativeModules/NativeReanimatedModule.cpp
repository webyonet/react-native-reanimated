#include "NativeReanimatedModule.h"

#ifdef RCT_NEW_ARCH_ENABLED
#include <react/renderer/core/TraitCast.h>
#include <react/renderer/uimanager/UIManagerBinding.h>
#include <react/renderer/uimanager/primitives.h>
#endif

#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>

#ifdef RCT_NEW_ARCH_ENABLED
#include "FabricUtils.h"
#include "PropsRegistry.h"
#include "ReanimatedCommitMarker.h"
#include "ShadowTreeCloner.h"
#endif

#include "EventHandlerRegistry.h"
#include "FeaturesConfig.h"
#include "ReanimatedHiddenHeaders.h"
#include "RuntimeDecorator.h"
#include "Shareables.h"
#include "WorkletEventHandler.h"

#ifdef DEBUG
#include "JSLogger.h"
#endif

using namespace facebook;

namespace reanimated {

NativeReanimatedModule::NativeReanimatedModule(
    const std::shared_ptr<CallInvoker> &jsInvoker,
    const std::shared_ptr<UIScheduler> &uiScheduler,
    const std::shared_ptr<jsi::Runtime> &rt,
    const PlatformDepMethodsHolder &platformDepMethodsHolder)
    : NativeReanimatedModuleSpec(jsInvoker),
      runtimeManager_(std::make_shared<RuntimeManager>(
          rt,
          uiScheduler,
          std::make_shared<JSScheduler>(jsInvoker),
          RuntimeType::UI)),
      eventHandlerRegistry(std::make_unique<EventHandlerRegistry>()),
      requestRender(platformDepMethodsHolder.requestRender),
#ifdef RCT_NEW_ARCH_ENABLED
// nothing
#else
      obtainPropFunction_(platformDepMethodsHolder.obtainPropFunction),
#endif
      animatedSensorModule(platformDepMethodsHolder),
#ifdef RCT_NEW_ARCH_ENABLED
      synchronouslyUpdateUIPropsFunction(
          platformDepMethodsHolder.synchronouslyUpdateUIPropsFunction)
#else
      configurePropsPlatformFunction(
          platformDepMethodsHolder.configurePropsFunction)
#endif
{
  auto requestAnimationFrame = [=](jsi::Runtime &rt, const jsi::Value &fn) {
    auto jsFunction = std::make_shared<jsi::Value>(rt, fn);
    frameCallbacks.push_back([=](double timestamp) {
      runtimeHelper->runOnUIGuarded(*jsFunction, jsi::Value(timestamp));
    });
    maybeRequestRender();
  };

  auto scheduleOnJS = [this](
                          jsi::Runtime &rt,
                          const jsi::Value &remoteFun,
                          const jsi::Value &argsValue) {
    this->scheduleOnJS(rt, remoteFun, argsValue);
  };

  auto makeShareableClone = [this](jsi::Runtime &rt, const jsi::Value &value) {
    return this->makeShareableClone(rt, value, jsi::Value::undefined());
  };

  auto updateDataSynchronously =
      [this](
          jsi::Runtime &rt,
          const jsi::Value &synchronizedDataHolderRef,
          const jsi::Value &newData) {
        return this->updateDataSynchronously(
            rt, synchronizedDataHolderRef, newData);
      };

#ifdef RCT_NEW_ARCH_ENABLED
  auto updateProps = [this](jsi::Runtime &rt, const jsi::Value &operations) {
    this->updateProps(rt, operations);
  };

  auto removeFromPropsRegistry =
      [this](jsi::Runtime &rt, const jsi::Value &viewTags) {
        this->removeFromPropsRegistry(rt, viewTags);
      };

  auto measure = [this](jsi::Runtime &rt, const jsi::Value &shadowNodeValue) {
    return this->measure(rt, shadowNodeValue);
  };

  auto dispatchCommand = [this](
                             jsi::Runtime &rt,
                             const jsi::Value &shadowNodeValue,
                             const jsi::Value &commandNameValue,
                             const jsi::Value &argsValue) {
    this->dispatchCommand(rt, shadowNodeValue, commandNameValue, argsValue);
  };
#endif

  RuntimeDecorator::decorateUIRuntime(
      *runtimeManager_->runtime,
#ifdef RCT_NEW_ARCH_ENABLED
      updateProps,
      removeFromPropsRegistry,
      measure,
      dispatchCommand,
#else
      platformDepMethodsHolder.updatePropsFunction,
      platformDepMethodsHolder.measureFunction,
      platformDepMethodsHolder.scrollToFunction,
      platformDepMethodsHolder.dispatchCommandFunction,
#endif
      requestAnimationFrame,
      scheduleOnJS,
      makeShareableClone,
      updateDataSynchronously,
      platformDepMethodsHolder.getCurrentTime,
      platformDepMethodsHolder.setGestureStateFunction,
      platformDepMethodsHolder.progressLayoutAnimation,
      platformDepMethodsHolder.endLayoutAnimation,
      platformDepMethodsHolder.maybeFlushUIUpdatesQueueFunction);
  onRenderCallback = [this](double timestampMs) {
    this->renderRequested = false;
    this->onRender(timestampMs);
  };

#ifdef RCT_NEW_ARCH_ENABLED
  // nothing
#else
  updatePropsFunction = platformDepMethodsHolder.updatePropsFunction;
#endif
  subscribeForKeyboardEventsFunction =
      platformDepMethodsHolder.subscribeForKeyboardEvents;
  unsubscribeFromKeyboardEventsFunction =
      platformDepMethodsHolder.unsubscribeFromKeyboardEvents;
}

void NativeReanimatedModule::installCoreFunctions(
    jsi::Runtime &rt,
    const jsi::Value &callGuard,
    const jsi::Value &valueUnpacker) {
  if (!runtimeHelper) {
    // initialize runtimeHelper here if not already present. We expect only one
    // instace of the helper to exists.
    runtimeHelper = std::make_shared<JSRuntimeHelper>(
        &rt,
        runtimeManager_->runtime.get(),
        runtimeManager_->uiScheduler_,
        runtimeManager_->jsScheduler_);
  }
  runtimeHelper->callGuard =
      std::make_unique<CoreFunction>(runtimeHelper.get(), callGuard);
  runtimeHelper->valueUnpacker =
      std::make_unique<CoreFunction>(runtimeHelper.get(), valueUnpacker);
#ifdef DEBUG
  // We initialize jsLogger_ here because we need runtimeHelper
  // to be initialized already
  jsLogger_ = std::make_shared<JSLogger>(runtimeHelper);
  layoutAnimationsManager_.setJSLogger(jsLogger_);
#endif
}

NativeReanimatedModule::~NativeReanimatedModule() {
  if (runtimeHelper) {
    runtimeHelper->callGuard = nullptr;
    runtimeHelper->valueUnpacker = nullptr;
    // event handler registry and frame callbacks store some JSI values from UI
    // runtime, so they have to go away before we tear down the runtime
    eventHandlerRegistry.reset();
    frameCallbacks.clear();
    runtimeManager_->runtime.reset();
    // make sure uiRuntimeDestroyed is set after the runtime is deallocated
    runtimeHelper->uiRuntimeDestroyed = true;
  }
}

void NativeReanimatedModule::scheduleOnUI(
    jsi::Runtime &rt,
    const jsi::Value &worklet) {
  auto shareableWorklet = extractShareableOrThrow<ShareableWorklet>(
      rt, worklet, "only worklets can be scheduled to run on UI");
  runtimeManager_->uiScheduler_->scheduleOnUI([=] {
    jsi::Runtime &rt = *runtimeHelper->uiRuntime();
    auto workletValue = shareableWorklet->getJSValue(rt);
    runtimeHelper->runOnUIGuarded(workletValue);
  });
}

void NativeReanimatedModule::scheduleOnJS(
    jsi::Runtime &rt,
    const jsi::Value &remoteFun,
    const jsi::Value &argsValue) {
  auto shareableRemoteFun = extractShareableOrThrow<ShareableRemoteFunction>(
      rt,
      remoteFun,
      "Incompatible object passed to scheduleOnJS. It is only allowed to schedule worklets or functions defined on the React Native JS runtime this way.");
  auto shareableArgs = argsValue.isUndefined()
      ? nullptr
      : extractShareableOrThrow<ShareableArray>(
            rt, argsValue, "args must be an array");
  auto jsRuntime = this->runtimeHelper->rnRuntime();
  runtimeManager_->jsScheduler_->scheduleOnJS([=] {
    jsi::Runtime &rt = *jsRuntime;
    auto remoteFun = shareableRemoteFun->getJSValue(rt);
    if (shareableArgs == nullptr) {
      // fast path for remote function w/o arguments
      remoteFun.asObject(rt).asFunction(rt).call(rt);
    } else {
      auto argsArray = shareableArgs->getJSValue(rt).asObject(rt).asArray(rt);
      auto argsSize = argsArray.size(rt);
      // number of arguments is typically relatively small so it is ok to
      // to use VLAs here, hence disabling the lint rule
      jsi::Value args[argsSize]; // NOLINT(runtime/arrays)
      for (size_t i = 0; i < argsSize; i++) {
        args[i] = argsArray.getValueAtIndex(rt, i);
      }
      remoteFun.asObject(rt).asFunction(rt).call(rt, args, argsSize);
    }
  });
}

jsi::Value NativeReanimatedModule::makeSynchronizedDataHolder(
    jsi::Runtime &rt,
    const jsi::Value &initialShareable) {
  auto dataHolder = std::make_shared<ShareableSynchronizedDataHolder>(
      runtimeHelper, rt, initialShareable);
  return dataHolder->getJSValue(rt);
}

void NativeReanimatedModule::updateDataSynchronously(
    jsi::Runtime &rt,
    const jsi::Value &synchronizedDataHolderRef,
    const jsi::Value &newData) {
  auto dataHolder = extractShareableOrThrow<ShareableSynchronizedDataHolder>(
      rt, synchronizedDataHolderRef);
  dataHolder->set(rt, newData);
}

jsi::Value NativeReanimatedModule::getDataSynchronously(
    jsi::Runtime &rt,
    const jsi::Value &synchronizedDataHolderRef) {
  auto dataHolder = extractShareableOrThrow<ShareableSynchronizedDataHolder>(
      rt, synchronizedDataHolderRef);
  return dataHolder->get(rt);
}

jsi::Value NativeReanimatedModule::makeShareableClone(
    jsi::Runtime &rt,
    const jsi::Value &value,
    const jsi::Value &shouldRetainRemote) {
  std::shared_ptr<Shareable> shareable;
  if (value.isObject()) {
    auto object = value.asObject(rt);
    if (!object.getProperty(rt, "__workletHash").isUndefined()) {
      shareable = std::make_shared<ShareableWorklet>(runtimeHelper, rt, object);
    } else if (!object.getProperty(rt, "__init").isUndefined()) {
      shareable = std::make_shared<ShareableHandle>(runtimeHelper, rt, object);
    } else if (object.isFunction(rt)) {
      auto function = object.asFunction(rt);
      if (function.isHostFunction(rt)) {
        shareable =
            std::make_shared<ShareableHostFunction>(rt, std::move(function));
      } else {
        shareable = std::make_shared<ShareableRemoteFunction>(
            runtimeHelper, rt, std::move(function));
      }
    } else if (object.isArray(rt)) {
      if (shouldRetainRemote.isBool() && shouldRetainRemote.getBool()) {
        shareable = std::make_shared<RetainingShareable<ShareableArray>>(
            runtimeHelper, rt, object.asArray(rt));
      } else {
        shareable = std::make_shared<ShareableArray>(rt, object.asArray(rt));
      }
    } else if (object.isHostObject(rt)) {
      shareable = std::make_shared<ShareableHostObject>(
          runtimeHelper, rt, object.getHostObject(rt));
    } else {
      if (shouldRetainRemote.isBool() && shouldRetainRemote.getBool()) {
        shareable = std::make_shared<RetainingShareable<ShareableObject>>(
            runtimeHelper, rt, object);
      } else {
        shareable = std::make_shared<ShareableObject>(rt, object);
      }
    }
  } else if (value.isString()) {
    shareable = std::make_shared<ShareableString>(value.asString(rt).utf8(rt));
  } else if (value.isUndefined()) {
    shareable = std::make_shared<ShareableScalar>();
  } else if (value.isNull()) {
    shareable = std::make_shared<ShareableScalar>(nullptr);
  } else if (value.isBool()) {
    shareable = std::make_shared<ShareableScalar>(value.getBool());
  } else if (value.isNumber()) {
    shareable = std::make_shared<ShareableScalar>(value.getNumber());
  } else if (value.isSymbol()) {
    // TODO: this is only a placeholder implementation, here we replace symbols
    // with strings in order to make certain objects to be captured. There isn't
    // yet any usecase for using symbols on the UI runtime so it is fine to keep
    // it like this for now.
    shareable =
        std::make_shared<ShareableString>(value.getSymbol(rt).toString(rt));
  } else {
    throw std::runtime_error(
        "[Reanimated] Attempted to convert an unsupported value type.");
  }
  return ShareableJSRef::newHostObject(rt, shareable);
}

jsi::Value NativeReanimatedModule::registerEventHandler(
    jsi::Runtime &rt,
    const jsi::Value &worklet,
    const jsi::Value &eventName,
    const jsi::Value &emitterReactTag) {
  static uint64_t NEXT_EVENT_HANDLER_ID = 1;

  uint64_t newRegistrationId = NEXT_EVENT_HANDLER_ID++;
  auto eventNameStr = eventName.asString(rt).utf8(rt);
  auto handlerShareable = extractShareableOrThrow<ShareableWorklet>(
      rt, worklet, "event handler must be a worklet");
  int emitterReactTagInt = emitterReactTag.asNumber();

  runtimeManager_->uiScheduler_->scheduleOnUI([=] {
    jsi::Runtime &rt = *runtimeHelper->uiRuntime();
    auto handlerFunction = handlerShareable->getJSValue(rt);
    auto handler = std::make_shared<WorkletEventHandler>(
        runtimeHelper,
        newRegistrationId,
        eventNameStr,
        emitterReactTagInt,
        std::move(handlerFunction));
    eventHandlerRegistry->registerEventHandler(std::move(handler));
  });

  return jsi::Value(static_cast<double>(newRegistrationId));
}

void NativeReanimatedModule::unregisterEventHandler(
    jsi::Runtime &,
    const jsi::Value &registrationId) {
  uint64_t id = registrationId.asNumber();
  runtimeManager_->uiScheduler_->scheduleOnUI(
      [=] { eventHandlerRegistry->unregisterEventHandler(id); });
}

jsi::Value NativeReanimatedModule::getViewProp(
    jsi::Runtime &rnRuntime,
    const jsi::Value &viewTag,
    const jsi::Value &propName,
    const jsi::Value &callback) {
  const int viewTagInt = static_cast<int>(viewTag.asNumber());
  std::string propNameStr = propName.asString(rnRuntime).utf8(rnRuntime);
  jsi::Function fun = callback.getObject(rnRuntime).asFunction(rnRuntime);
  std::shared_ptr<jsi::Function> funPtr =
      std::make_shared<jsi::Function>(std::move(fun));

  runtimeManager_->uiScheduler_->scheduleOnUI(
      [&rnRuntime, viewTagInt, funPtr, this, propNameStr]() {
        jsi::Runtime &uiRuntime = *runtimeManager_->runtime;
        const jsi::String propNameValue =
            jsi::String::createFromUtf8(uiRuntime, propNameStr);
        jsi::Value result =
            obtainPropFunction_(uiRuntime, viewTagInt, propNameValue);
        std::string resultStr = result.asString(uiRuntime).utf8(uiRuntime);

        runtimeManager_->jsScheduler_->scheduleOnJS(
            [&rnRuntime, resultStr, funPtr]() {
              const jsi::String resultValue =
                  jsi::String::createFromUtf8(rnRuntime, resultStr);
              funPtr->call(rnRuntime, resultValue);
            });
      });

  return jsi::Value::undefined();
}

jsi::Value NativeReanimatedModule::enableLayoutAnimations(
    jsi::Runtime &,
    const jsi::Value &config) {
  FeaturesConfig::setLayoutAnimationEnabled(config.getBool());
  return jsi::Value::undefined();
}

jsi::Value NativeReanimatedModule::configureProps(
    jsi::Runtime &rt,
    const jsi::Value &uiProps,
    const jsi::Value &nativeProps) {
#ifdef RCT_NEW_ARCH_ENABLED
  (void)uiProps; // unused variable on Fabric
  jsi::Array array = nativeProps.asObject(rt).asArray(rt);
  for (size_t i = 0; i < array.size(rt); ++i) {
    std::string name = array.getValueAtIndex(rt, i).asString(rt).utf8(rt);
    nativePropNames_.insert(name);
  }
#else
  configurePropsPlatformFunction(rt, uiProps, nativeProps);
#endif // RCT_NEW_ARCH_ENABLED

  return jsi::Value::undefined();
}

jsi::Value NativeReanimatedModule::configureLayoutAnimation(
    jsi::Runtime &rt,
    const jsi::Value &viewTag,
    const jsi::Value &type,
    const jsi::Value &sharedTransitionTag,
    const jsi::Value &config) {
  layoutAnimationsManager_.configureAnimation(
      viewTag.asNumber(),
      static_cast<LayoutAnimationType>(type.asNumber()),
      sharedTransitionTag.asString(rt).utf8(rt),
      extractShareableOrThrow<ShareableObject>(
          rt, config, "layout animation config must be an object"));
  return jsi::Value::undefined();
}

bool NativeReanimatedModule::isAnyHandlerWaitingForEvent(
    const std::string &eventName,
    const int emitterReactTag) {
  return eventHandlerRegistry->isAnyHandlerWaitingForEvent(
      eventName, emitterReactTag);
}

void NativeReanimatedModule::maybeRequestRender() {
  if (!renderRequested) {
    renderRequested = true;
    requestRender(onRenderCallback, *runtimeManager_->runtime);
  }
}

void NativeReanimatedModule::onRender(double timestampMs) {
  std::vector<FrameCallback> callbacks = frameCallbacks;
  frameCallbacks.clear();
  for (auto &callback : callbacks) {
    callback(timestampMs);
  }
}

jsi::Value NativeReanimatedModule::registerSensor(
    jsi::Runtime &rt,
    const jsi::Value &sensorType,
    const jsi::Value &interval,
    const jsi::Value &iosReferenceFrame,
    const jsi::Value &sensorDataHandler) {
  return animatedSensorModule.registerSensor(
      rt,
      runtimeHelper,
      sensorType,
      interval,
      iosReferenceFrame,
      sensorDataHandler);
}

void NativeReanimatedModule::unregisterSensor(
    jsi::Runtime &,
    const jsi::Value &sensorId) {
  animatedSensorModule.unregisterSensor(sensorId);
}

void NativeReanimatedModule::cleanupSensors() {
  animatedSensorModule.unregisterAllSensors();
}

#ifdef RCT_NEW_ARCH_ENABLED
bool NativeReanimatedModule::isThereAnyLayoutProp(
    jsi::Runtime &rt,
    const jsi::Object &props) {
  const jsi::Array propNames = props.getPropertyNames(rt);
  for (size_t i = 0; i < propNames.size(rt); ++i) {
    const std::string propName =
        propNames.getValueAtIndex(rt, i).asString(rt).utf8(rt);
    bool isLayoutProp =
        nativePropNames_.find(propName) != nativePropNames_.end();
    if (isLayoutProp) {
      return true;
    }
  }
  return false;
}
#endif // RCT_NEW_ARCH_ENABLED

bool NativeReanimatedModule::handleEvent(
    const std::string &eventName,
    const int emitterReactTag,
    const jsi::Value &payload,
    double currentTime) {
  eventHandlerRegistry->processEvent(
      *runtimeManager_->runtime,
      currentTime,
      eventName,
      emitterReactTag,
      payload);

  // TODO: return true if Reanimated successfully handled the event
  // to avoid sending it to JavaScript
  return false;
}

#ifdef RCT_NEW_ARCH_ENABLED
bool NativeReanimatedModule::handleRawEvent(
    const RawEvent &rawEvent,
    double currentTime) {
  const EventTarget *eventTarget = rawEvent.eventTarget.get();
  if (eventTarget == nullptr) {
    // after app reload scrollview is unmounted and its content offset is set to
    // 0 and view is thrown into recycle pool setting content offset triggers
    // scroll event eventTarget is null though, because it's unmounting we can
    // just ignore this event, because it's an event on unmounted component
    return false;
  }
  const std::string &type = rawEvent.type;
  const ValueFactory &payloadFactory = rawEvent.payloadFactory;

  int tag = eventTarget->getTag();
  std::string eventType = type;
  if (eventType.rfind("top", 0) == 0) {
    eventType = "on" + eventType.substr(3);
  }
  jsi::Runtime &rt = *runtimeManager_->runtime.get();
  jsi::Value payload = payloadFactory(rt);

  auto res = handleEvent(eventType, tag, std::move(payload), currentTime);
  // TODO: we should call performOperations conditionally if event is handled
  // (res == true), but for now handleEvent always returns false. Thankfully,
  // performOperations does not trigger a lot of code if there is nothing to be
  // done so this is fine for now.
  performOperations();
  return res;
}

void NativeReanimatedModule::updateProps(
    jsi::Runtime &rt,
    const jsi::Value &operations) {
  auto array = operations.asObject(rt).asArray(rt);
  size_t length = array.size(rt);
  for (size_t i = 0; i < length; ++i) {
    auto item = array.getValueAtIndex(rt, i).asObject(rt);
    auto shadowNodeWrapper = item.getProperty(rt, "shadowNodeWrapper");
    auto shadowNode = shadowNodeFromValue(rt, shadowNodeWrapper);
    const jsi::Value &updates = item.getProperty(rt, "updates");
    operationsInBatch_.emplace_back(
        shadowNode, std::make_unique<jsi::Value>(rt, updates));

    // TODO: support multiple surfaces
    surfaceId_ = shadowNode->getSurfaceId();
  }
}

void NativeReanimatedModule::performOperations() {
  if (operationsInBatch_.empty() && tagsToRemove_.empty()) {
    // nothing to do
    return;
  }

  auto copiedOperationsQueue = std::move(operationsInBatch_);
  operationsInBatch_ =
      std::vector<std::pair<ShadowNode::Shared, std::unique_ptr<jsi::Value>>>();

  jsi::Runtime &rt = *runtimeManager_->runtime;

  {
    auto lock = propsRegistry_->createLock();

    // remove recently unmounted ShadowNodes from PropsRegistry
    if (!tagsToRemove_.empty()) {
      for (auto tag : tagsToRemove_) {
        propsRegistry_->remove(tag);
      }
      tagsToRemove_.clear();
    }

    // Even if only non-layout props are changed, we need to store the update in
    // PropsRegistry anyway so that React doesn't overwrite it in the next
    // render. Currently, only opacity and transform are treated in a special
    // way but backgroundColor, shadowOpacity etc. would get overwritten (see
    // `_propKeysManagedByAnimated_DO_NOT_USE_THIS_IS_BROKEN`).
    for (const auto &[shadowNode, props] : copiedOperationsQueue) {
      propsRegistry_->update(shadowNode, dynamicFromValue(rt, *props));
    }
  }

  bool hasLayoutUpdates = false;
  for (const auto &[shadowNode, props] : copiedOperationsQueue) {
    if (isThereAnyLayoutProp(rt, props->asObject(rt))) {
      hasLayoutUpdates = true;
      break;
    }
  }

  if (!hasLayoutUpdates) {
    // If there's no layout props to be updated, we can apply the updates
    // directly onto the components and skip the commit.
    for (const auto &[shadowNode, props] : copiedOperationsQueue) {
      Tag tag = shadowNode->getTag();
      synchronouslyUpdateUIPropsFunction(rt, tag, props->asObject(rt));
    }
    return;
  }

  if (propsRegistry_->shouldSkipCommit()) {
    // It may happen that `performOperations` is called on the UI thread
    // while React Native tries to commit a new tree on the JS thread.
    // In this case, we should skip the commit here and let React Native do it.
    // The commit will include the current values from PropsRegistry
    // which will be applied in ReanimatedCommitHook.
    return;
  }

  react_native_assert(uiManager_ != nullptr);
  const auto &shadowTreeRegistry = uiManager_->getShadowTreeRegistry();

  shadowTreeRegistry.visit(surfaceId_, [&](ShadowTree const &shadowTree) {
    // Mark the commit as Reanimated commit so that we can distinguish it
    // in ReanimatedCommitHook.
    ReanimatedCommitMarker commitMarker;

    shadowTree.commit(
        [&](RootShadowNode const &oldRootShadowNode) {
          auto rootNode =
              oldRootShadowNode.ShadowNode::clone(ShadowNodeFragment{});

          ShadowTreeCloner shadowTreeCloner{*uiManager_, surfaceId_};

          for (const auto &[shadowNode, props] : copiedOperationsQueue) {
            const ShadowNodeFamily &family = shadowNode->getFamily();
            react_native_assert(family.getSurfaceId() == surfaceId_);

            auto newRootNode = shadowTreeCloner.cloneWithNewProps(
                rootNode, family, RawProps(rt, *props));

            if (newRootNode == nullptr) {
              // this happens when React removed the component but Reanimated
              // still tries to animate it, let's skip update for this
              // specific component
              continue;
            }
            rootNode = newRootNode;
          }

          auto newRoot = std::static_pointer_cast<RootShadowNode>(rootNode);

          return newRoot;
        },
        {/* default commit options */});
  });
}

void NativeReanimatedModule::removeFromPropsRegistry(
    jsi::Runtime &rt,
    const jsi::Value &viewTags) {
  auto array = viewTags.asObject(rt).asArray(rt);
  for (size_t i = 0, size = array.size(rt); i < size; ++i) {
    tagsToRemove_.push_back(array.getValueAtIndex(rt, i).asNumber());
  }
}

void NativeReanimatedModule::dispatchCommand(
    jsi::Runtime &rt,
    const jsi::Value &shadowNodeValue,
    const jsi::Value &commandNameValue,
    const jsi::Value &argsValue) {
  ShadowNode::Shared shadowNode = shadowNodeFromValue(rt, shadowNodeValue);
  std::string commandName = stringFromValue(rt, commandNameValue);
  folly::dynamic args = commandArgsFromValue(rt, argsValue);
  uiManager_->dispatchCommand(shadowNode, commandName, args);
}

jsi::Value NativeReanimatedModule::measure(
    jsi::Runtime &rt,
    const jsi::Value &shadowNodeValue) {
  // based on implementation from UIManagerBinding.cpp

  auto shadowNode = shadowNodeFromValue(rt, shadowNodeValue);
  auto layoutMetrics = uiManager_->getRelativeLayoutMetrics(
      *shadowNode, nullptr, {/* .includeTransform = */ true});

  if (layoutMetrics == EmptyLayoutMetrics) {
    // Originally, in this case React Native returns `{0, 0, 0, 0, 0, 0}`, most
    // likely due to the type of measure callback function which accepts just an
    // array of numbers (not null). In Reanimated, `measure` returns
    // `MeasuredDimensions | null`.
    return jsi::Value::null();
  }
  auto newestCloneOfShadowNode =
      uiManager_->getNewestCloneOfShadowNode(*shadowNode);

  auto layoutableShadowNode =
      traitCast<LayoutableShadowNode const *>(newestCloneOfShadowNode.get());
  facebook::react::Point originRelativeToParent =
      layoutableShadowNode != nullptr
      ? layoutableShadowNode->getLayoutMetrics().frame.origin
      : facebook::react::Point();

  auto frame = layoutMetrics.frame;

  jsi::Object result(rt);
  result.setProperty(
      rt, "x", jsi::Value(static_cast<double>(originRelativeToParent.x)));
  result.setProperty(
      rt, "y", jsi::Value(static_cast<double>(originRelativeToParent.y)));
  result.setProperty(
      rt, "width", jsi::Value(static_cast<double>(frame.size.width)));
  result.setProperty(
      rt, "height", jsi::Value(static_cast<double>(frame.size.height)));
  result.setProperty(
      rt, "pageX", jsi::Value(static_cast<double>(frame.origin.x)));
  result.setProperty(
      rt, "pageY", jsi::Value(static_cast<double>(frame.origin.y)));
  return result;
}

void NativeReanimatedModule::setUIManager(
    std::shared_ptr<UIManager> uiManager) {
  uiManager_ = uiManager;
}

void NativeReanimatedModule::setPropsRegistry(
    std::shared_ptr<PropsRegistry> propsRegistry) {
  propsRegistry_ = propsRegistry;
}
#endif // RCT_NEW_ARCH_ENABLED

jsi::Value NativeReanimatedModule::subscribeForKeyboardEvents(
    jsi::Runtime &rt,
    const jsi::Value &handlerWorklet,
    const jsi::Value &isStatusBarTranslucent) {
  auto shareableHandler = extractShareableOrThrow<ShareableWorklet>(
      rt, handlerWorklet, "keyboard event handler must be a worklet");
  return subscribeForKeyboardEventsFunction(
      [=](int keyboardState, int height) {
        jsi::Runtime &rt = *runtimeHelper->uiRuntime();
        auto handler = shareableHandler->getJSValue(rt);
        runtimeHelper->runOnUIGuarded(
            handler, jsi::Value(keyboardState), jsi::Value(height));
      },
      isStatusBarTranslucent.getBool());
}

void NativeReanimatedModule::unsubscribeFromKeyboardEvents(
    jsi::Runtime &,
    const jsi::Value &listenerId) {
  unsubscribeFromKeyboardEventsFunction(listenerId.asNumber());
}

} // namespace reanimated
