/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "DefaultVehicleHal_v2_0"

#include <android-base/chrono_utils.h>
#include <assert.h>
#include <utils/Log.h>
#include <utils/SystemClock.h>
#include <vhal_v2_0/RecurrentTimer.h>

#include "VehicleUtils.h"

#include "DefaultVehicleHal.h"

namespace android {
namespace hardware {
namespace automotive {
namespace vehicle {
namespace V2_0 {

namespace impl {

namespace {
constexpr std::chrono::nanoseconds kHeartBeatIntervalNs = 3s;
}  // namespace

DefaultVehicleHal::DefaultVehicleHal(VehiclePropertyStore* propStore, VehicleHalClient* client)
    : mPropStore(propStore), mRecurrentTimer(getTimerAction()), mVehicleClient(client) {
    initStaticConfig();
    mVehicleClient->registerPropertyValueCallback(
            [this](const VehiclePropValue& value, bool updateStatus) {
                onPropertyValue(value, updateStatus);
            });
}

VehicleHal::VehiclePropValuePtr DefaultVehicleHal::get(const VehiclePropValue& requestedPropValue,
                                                       StatusCode* outStatus) {
    auto propId = requestedPropValue.prop;
    ALOGV("get(%d)", propId);

    VehiclePropValuePtr v = nullptr;
    auto internalPropValue = mPropStore->readValueOrNull(requestedPropValue);
    if (internalPropValue != nullptr) {
        v = getValuePool()->obtain(*internalPropValue);
    }

    if (!v) {
        *outStatus = StatusCode::INVALID_ARG;
    } else if (v->status == VehiclePropertyStatus::AVAILABLE) {
        *outStatus = StatusCode::OK;
    } else {
        *outStatus = StatusCode::TRY_AGAIN;
    }
    if (v.get()) {
        v->timestamp = elapsedRealtimeNano();
    }
    return v;
}

std::vector<VehiclePropConfig> DefaultVehicleHal::listProperties() {
    return mPropStore->getAllConfigs();
}

bool DefaultVehicleHal::dump(const hidl_handle& fd, const hidl_vec<hidl_string>& options) {
    return mVehicleClient->dump(fd, options);
}

StatusCode DefaultVehicleHal::checkPropValue(const VehiclePropValue& value,
                                             const VehiclePropConfig* config) {
    int32_t property = value.prop;
    VehiclePropertyType type = getPropType(property);
    switch (type) {
        case VehiclePropertyType::BOOLEAN:
        case VehiclePropertyType::INT32:
            if (value.value.int32Values.size() != 1) {
                return StatusCode::INVALID_ARG;
            }
            break;
        case VehiclePropertyType::INT32_VEC:
            if (value.value.int32Values.size() < 1) {
                return StatusCode::INVALID_ARG;
            }
            break;
        case VehiclePropertyType::INT64:
            if (value.value.int64Values.size() != 1) {
                return StatusCode::INVALID_ARG;
            }
            break;
        case VehiclePropertyType::INT64_VEC:
            if (value.value.int64Values.size() < 1) {
                return StatusCode::INVALID_ARG;
            }
            break;
        case VehiclePropertyType::FLOAT:
            if (value.value.floatValues.size() != 1) {
                return StatusCode::INVALID_ARG;
            }
            break;
        case VehiclePropertyType::FLOAT_VEC:
            if (value.value.floatValues.size() < 1) {
                return StatusCode::INVALID_ARG;
            }
            break;
        case VehiclePropertyType::BYTES:
            // We allow setting an empty bytes array.
            break;
        case VehiclePropertyType::STRING:
            // We allow setting an empty string.
            break;
        case VehiclePropertyType::MIXED:
            if (getPropGroup(property) == VehiclePropertyGroup::VENDOR) {
                // We only checks vendor mixed properties.
                return checkVendorMixedPropValue(value, config);
            }
            break;
        default:
            ALOGW("Unknown property type: %d", type);
            return StatusCode::INVALID_ARG;
    }
    return StatusCode::OK;
}

StatusCode DefaultVehicleHal::checkVendorMixedPropValue(const VehiclePropValue& value,
                                                        const VehiclePropConfig* config) {
    auto configArray = config->configArray;
    // configArray[0], 1 indicates the property has a String value, we allow the string value to
    // be empty.

    size_t int32Count = 0;
    // configArray[1], 1 indicates the property has a Boolean value.
    if (configArray[1] == 1) {
        int32Count++;
    }
    // configArray[2], 1 indicates the property has an Integer value.
    if (configArray[2] == 1) {
        int32Count++;
    }
    // configArray[3], the number indicates the size of Integer[] in the property.
    int32Count += static_cast<size_t>(configArray[3]);
    if (value.value.int32Values.size() != int32Count) {
        return StatusCode::INVALID_ARG;
    }

    size_t int64Count = 0;
    // configArray[4], 1 indicates the property has a Long value.
    if (configArray[4] == 1) {
        int64Count++;
    }
    // configArray[5], the number indicates the size of Long[] in the property.
    int64Count += static_cast<size_t>(configArray[5]);
    if (value.value.int64Values.size() != int64Count) {
        return StatusCode::INVALID_ARG;
    }

    size_t floatCount = 0;
    // configArray[6], 1 indicates the property has a Float value.
    if (configArray[6] == 1) {
        floatCount++;
    }
    // configArray[7], the number indicates the size of Float[] in the property.
    floatCount += static_cast<size_t>(configArray[7]);
    if (value.value.floatValues.size() != floatCount) {
        return StatusCode::INVALID_ARG;
    }

    // configArray[8], the number indicates the size of byte[] in the property.
    if (configArray[8] != 0 && value.value.bytes.size() != static_cast<size_t>(configArray[8])) {
        return StatusCode::INVALID_ARG;
    }
    return StatusCode::OK;
}

StatusCode DefaultVehicleHal::checkValueRange(const VehiclePropValue& value,
                                              const VehiclePropConfig* config) {
    int32_t property = value.prop;
    VehiclePropertyType type = getPropType(property);
    const VehicleAreaConfig* areaConfig;
    if (isGlobalProp(property)) {
        if (config->areaConfigs.size() == 0) {
            return StatusCode::OK;
        }
        areaConfig = &(config->areaConfigs[0]);
    } else {
        for (auto& c : config->areaConfigs) {
            // areaId might contain multiple areas.
            if (c.areaId & value.areaId) {
                areaConfig = &c;
                break;
            }
        }
    }
    switch (type) {
        case VehiclePropertyType::INT32:
            if (areaConfig->minInt32Value == 0 && areaConfig->maxInt32Value == 0) {
                break;
            }
            // We already checked this in checkPropValue.
            assert(value.value.int32Values.size() > 0);
            if (value.value.int32Values[0] < areaConfig->minInt32Value ||
                value.value.int32Values[0] > areaConfig->maxInt32Value) {
                return StatusCode::INVALID_ARG;
            }
            break;
        case VehiclePropertyType::INT64:
            if (areaConfig->minInt64Value == 0 && areaConfig->maxInt64Value == 0) {
                break;
            }
            // We already checked this in checkPropValue.
            assert(value.value.int64Values.size() > 0);
            if (value.value.int64Values[0] < areaConfig->minInt64Value ||
                value.value.int64Values[0] > areaConfig->maxInt64Value) {
                return StatusCode::INVALID_ARG;
            }
            break;
        case VehiclePropertyType::FLOAT:
            if (areaConfig->minFloatValue == 0 && areaConfig->maxFloatValue == 0) {
                break;
            }
            // We already checked this in checkPropValue.
            assert(value.value.floatValues.size() > 0);
            if (value.value.floatValues[0] < areaConfig->minFloatValue ||
                value.value.floatValues[0] > areaConfig->maxFloatValue) {
                return StatusCode::INVALID_ARG;
            }
            break;
        default:
            // We don't check the rest of property types. Additional logic needs to be added if
            // required for real implementation. E.g., you might want to enforce the range
            // checks on vector as well or you might want to check the range for mixed property.
            break;
    }
    return StatusCode::OK;
}

StatusCode DefaultVehicleHal::set(const VehiclePropValue& propValue) {
    if (propValue.status != VehiclePropertyStatus::AVAILABLE) {
        // Android side cannot set property status - this value is the
        // purview of the HAL implementation to reflect the state of
        // its underlying hardware
        return StatusCode::INVALID_ARG;
    }

    int32_t property = propValue.prop;
    const VehiclePropConfig* config = mPropStore->getConfigOrNull(property);
    if (config == nullptr) {
        ALOGW("no config for prop 0x%x", property);
        return StatusCode::INVALID_ARG;
    }
    auto status = checkPropValue(propValue, config);
    if (status != StatusCode::OK) {
        ALOGW("invalid property value: %s", toString(propValue).c_str());
        return status;
    }
    status = checkValueRange(propValue, config);
    if (status != StatusCode::OK) {
        ALOGW("property value out of range: %s", toString(propValue).c_str());
        return status;
    }

    auto currentPropValue = mPropStore->readValueOrNull(propValue);
    if (currentPropValue && currentPropValue->status != VehiclePropertyStatus::AVAILABLE) {
        // do not allow Android side to set() a disabled/error property
        return StatusCode::NOT_AVAILABLE;
    }

    // Send the value to the vehicle server, the server will talk to the (real or emulated) car
    return mVehicleClient->setProperty(propValue, /*updateStatus=*/false);
}

// Parse supported properties list and generate vector of property values to hold current values.
void DefaultVehicleHal::onCreate() {
    auto configs = mVehicleClient->getAllPropertyConfig();

    for (const auto& cfg : configs) {
        int32_t numAreas = isGlobalProp(cfg.prop) ? 0 : cfg.areaConfigs.size();

        for (int i = 0; i < numAreas; i++) {
            int32_t curArea = isGlobalProp(cfg.prop) ? 0 : cfg.areaConfigs[i].areaId;

            // Create a separate instance for each individual zone
            VehiclePropValue prop = {
                    .areaId = curArea,
                    .prop = cfg.prop,
                    .status = VehiclePropertyStatus::UNAVAILABLE,
            };
            // Allow the initial values to set status.
            mPropStore->writeValue(prop, /*updateStatus=*/true);
        }
    }

    mVehicleClient->triggerSendAllValues();
    registerHeartBeatEvent();
}

DefaultVehicleHal::~DefaultVehicleHal() {
    mRecurrentTimer.unregisterRecurrentEvent(static_cast<int32_t>(VehicleProperty::VHAL_HEARTBEAT));
}

void DefaultVehicleHal::registerHeartBeatEvent() {
    mRecurrentTimer.registerRecurrentEvent(kHeartBeatIntervalNs,
                                           static_cast<int32_t>(VehicleProperty::VHAL_HEARTBEAT));
}

VehicleHal::VehiclePropValuePtr DefaultVehicleHal::doInternalHealthCheck() {
    VehicleHal::VehiclePropValuePtr v = nullptr;

    // This is an example of very simple health checking. VHAL is considered healthy if we can read
    // PERF_VEHICLE_SPEED. The more comprehensive health checking is required.
    VehiclePropValue propValue = {
            .prop = static_cast<int32_t>(VehicleProperty::PERF_VEHICLE_SPEED),
    };
    auto internalPropValue = mPropStore->readValueOrNull(propValue);
    if (internalPropValue != nullptr) {
        v = createVhalHeartBeatProp();
    } else {
        ALOGW("VHAL health check failed");
    }
    return v;
}

VehicleHal::VehiclePropValuePtr DefaultVehicleHal::createVhalHeartBeatProp() {
    VehicleHal::VehiclePropValuePtr v = getValuePool()->obtainInt64(uptimeMillis());
    v->prop = static_cast<int32_t>(VehicleProperty::VHAL_HEARTBEAT);
    v->areaId = 0;
    v->status = VehiclePropertyStatus::AVAILABLE;
    return v;
}

void DefaultVehicleHal::onContinuousPropertyTimer(const std::vector<int32_t>& properties) {
    auto& pool = *getValuePool();

    for (int32_t property : properties) {
        VehiclePropValuePtr v;
        if (isContinuousProperty(property)) {
            auto internalPropValue = mPropStore->readValueOrNull(property);
            if (internalPropValue != nullptr) {
                v = pool.obtain(*internalPropValue);
            }
        } else if (property == static_cast<int32_t>(VehicleProperty::VHAL_HEARTBEAT)) {
            // VHAL_HEARTBEAT is not a continuous value, but it needs to be updated periodically.
            // So, the update is done through onContinuousPropertyTimer.
            v = doInternalHealthCheck();
        } else {
            ALOGE("Unexpected onContinuousPropertyTimer for property: 0x%x", property);
            continue;
        }

        if (v.get()) {
            v->timestamp = elapsedRealtimeNano();
            doHalEvent(std::move(v));
        }
    }
}

RecurrentTimer::Action DefaultVehicleHal::getTimerAction() {
    return [this](const std::vector<int32_t>& properties) {
        onContinuousPropertyTimer(properties);
    };
}

StatusCode DefaultVehicleHal::subscribe(int32_t property, float sampleRate) {
    ALOGI("%s propId: 0x%x, sampleRate: %f", __func__, property, sampleRate);

    if (!isContinuousProperty(property)) {
        return StatusCode::INVALID_ARG;
    }

    // If the config does not exist, isContinuousProperty should return false.
    const VehiclePropConfig* config = mPropStore->getConfigOrNull(property);
    if (sampleRate < config->minSampleRate || sampleRate > config->maxSampleRate) {
        ALOGW("sampleRate out of range");
        return StatusCode::INVALID_ARG;
    }

    mRecurrentTimer.registerRecurrentEvent(hertzToNanoseconds(sampleRate), property);
    return StatusCode::OK;
}

StatusCode DefaultVehicleHal::unsubscribe(int32_t property) {
    ALOGI("%s propId: 0x%x", __func__, property);
    if (!isContinuousProperty(property)) {
        return StatusCode::INVALID_ARG;
    }
    // If the event was not registered before, this would do nothing.
    mRecurrentTimer.unregisterRecurrentEvent(property);
    return StatusCode::OK;
}

bool DefaultVehicleHal::isContinuousProperty(int32_t propId) const {
    const VehiclePropConfig* config = mPropStore->getConfigOrNull(propId);
    if (config == nullptr) {
        ALOGW("Config not found for property: 0x%x", propId);
        return false;
    }
    return config->changeMode == VehiclePropertyChangeMode::CONTINUOUS;
}

void DefaultVehicleHal::onPropertyValue(const VehiclePropValue& value, bool updateStatus) {
    VehiclePropValuePtr updatedPropValue = getValuePool()->obtain(value);

    if (mPropStore->writeValue(*updatedPropValue, updateStatus)) {
        doHalEvent(std::move(updatedPropValue));
    }
}

void DefaultVehicleHal::initStaticConfig() {
    auto configs = mVehicleClient->getAllPropertyConfig();
    for (auto&& cfg : configs) {
        mPropStore->registerProperty(cfg, nullptr);
    }
}

}  // namespace impl

}  // namespace V2_0
}  // namespace vehicle
}  // namespace automotive
}  // namespace hardware
}  // namespace android