/*
 * Copyright 2018 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "android.hardware.media.c2-service"

#include <android-base/logging.h>
#include <dlfcn.h>
#include <cstdlib>
#include <minijail.h>

#include <util/C2InterfaceHelper.h>
#include <C2Component.h>
#include <C2Config.h>

// HIDL
#include <binder/ProcessState.h>
#include <codec2/hidl/1.2/ComponentStore.h>
#include <hidl/HidlTransportSupport.h>

// AIDL
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <codec2/aidl/ComponentStore.h>
#include <codec2/aidl/ParamTypes.h>

// This is the absolute on-device path of the prebuild_etc module
// "android.hardware.media.c2-default-seccomp_policy" in Android.bp.
static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-default-seccomp_policy";

// Additional seccomp permissions can be added in this file.
// This file does not exist by default.
static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-extended-seccomp_policy";

// We want multiple threads to be running so that a blocking operation
// on one codec does not block the other codecs.
// For HIDL: Extra threads may be needed to handle a stacked IPC sequence that
// contains alternating binder and hwbinder calls. (See b/35283480.)
static constexpr int kThreadCount = 8;

class StoreImpl : public C2ComponentStore {
public:
    StoreImpl()
        : mReflectorHelper(std::make_shared<C2ReflectorHelper>()),
          mInterface(mReflectorHelper) {
    }

    virtual ~StoreImpl() override = default;

    virtual C2String getName() const override {
        return "default";
    }

    virtual c2_status_t createComponent(
            C2String /*name*/,
            std::shared_ptr<C2Component>* const /*component*/) override {
        return C2_NOT_FOUND;
    }

    virtual c2_status_t createInterface(
            C2String /* name */,
            std::shared_ptr<C2ComponentInterface>* const /* interface */) override {
        return C2_NOT_FOUND;
    }

    virtual std::vector<std::shared_ptr<const C2Component::Traits>>
            listComponents() override {
        return {};
    }

    virtual c2_status_t copyBuffer(
            std::shared_ptr<C2GraphicBuffer> /* src */,
            std::shared_ptr<C2GraphicBuffer> /* dst */) override {
        return C2_OMITTED;
    }

    virtual c2_status_t query_sm(
        const std::vector<C2Param*>& stackParams,
        const std::vector<C2Param::Index>& heapParamIndices,
        std::vector<std::unique_ptr<C2Param>>* const heapParams) const override {
        return mInterface.query(stackParams, heapParamIndices, C2_MAY_BLOCK, heapParams);
    }

    virtual c2_status_t config_sm(
            const std::vector<C2Param*>& params,
            std::vector<std::unique_ptr<C2SettingResult>>* const failures) override {
        return mInterface.config(params, C2_MAY_BLOCK, failures);
    }

    virtual std::shared_ptr<C2ParamReflector> getParamReflector() const override {
        return mReflectorHelper;
    }

    virtual c2_status_t querySupportedParams_nb(
            std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const override {
        return mInterface.querySupportedParams(params);
    }

    virtual c2_status_t querySupportedValues_sm(
            std::vector<C2FieldSupportedValuesQuery>& fields) const override {
        return mInterface.querySupportedValues(fields, C2_MAY_BLOCK);
    }

private:
    class Interface : public C2InterfaceHelper {
    public:
        Interface(const std::shared_ptr<C2ReflectorHelper> &helper)
            : C2InterfaceHelper(helper) {
            setDerivedInstance(this);

            addParameter(
                DefineParam(mIonUsageInfo, "ion-usage")
                .withDefault(new C2StoreIonUsageInfo())
                .withFields({
                    C2F(mIonUsageInfo, usage).flags(
                            {C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE}),
                    C2F(mIonUsageInfo, capacity).inRange(0, UINT32_MAX, 1024),
                    C2F(mIonUsageInfo, heapMask).any(),
                    C2F(mIonUsageInfo, allocFlags).flags({}),
                    C2F(mIonUsageInfo, minAlignment).equalTo(0)
                })
                .withSetter(SetIonUsage)
                .build());

            addParameter(
                DefineParam(mDmaBufUsageInfo, "dmabuf-usage")
                .withDefault(C2StoreDmaBufUsageInfo::AllocShared(128))
                .withFields({
                    C2F(mDmaBufUsageInfo, m.usage).flags({C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE}),
                    C2F(mDmaBufUsageInfo, m.capacity).inRange(0, UINT32_MAX, 1024),
                    C2F(mDmaBufUsageInfo, m.allocFlags).flags({}),
                    C2F(mDmaBufUsageInfo, m.heapName).any(),
                })
                .withSetter(SetDmaBufUsage)
                .build());
        }

        virtual ~Interface() = default;

    private:
        static C2R SetIonUsage(bool /* mayBlock */, C2P<C2StoreIonUsageInfo> &me) {
            // Vendor's TODO: put appropriate mapping logic
            me.set().heapMask = ~0;
            me.set().allocFlags = 0;
            me.set().minAlignment = 0;
            return C2R::Ok();
        }

        static C2R SetDmaBufUsage(bool /* mayBlock */, C2P<C2StoreDmaBufUsageInfo> &me) {
            // Vendor's TODO: put appropriate mapping logic
            strncpy(me.set().m.heapName, "system", me.v.flexCount());
            me.set().m.allocFlags = 0;
            return C2R::Ok();
        }


        std::shared_ptr<C2StoreIonUsageInfo> mIonUsageInfo;
        std::shared_ptr<C2StoreDmaBufUsageInfo> mDmaBufUsageInfo;
    };
    std::shared_ptr<C2ReflectorHelper> mReflectorHelper;
    Interface mInterface;
};


// [cannon][A17] vendor 底包是 MIUI 13 / Android 12（V13.0.3.0.SJEMIXM，
// SP1A.210812.016），MTK 的 C2 实现是 HIDL @1.2-mediatek，A17 framework 只认
// AIDL store。MTK 自己带了 libcodec2_mtk_c2store.so（C2ComponentStore 实现，
// 直接管理 vdec/venc 插件），这里直接实例化它当 AIDL store 的后端，
// 绕开 HIDL 那一层（A17 已无 HIDL codec2 客户端路径可复用）。
// 没有头文件，用 blob 的构造函数符号 + 8KB 缓冲区（对象大小由 blob 自己决定），
// 只构造不析构（进程常驻，blob 也没导出析构符号）。
static std::shared_ptr<C2ComponentStore> createMtkComponentStore() {
    void* handle = dlopen("/vendor/lib64/libcodec2_mtk_c2store.so", RTLD_NOW);
    if (handle == nullptr) {
        LOG(ERROR) << "cannot dlopen libcodec2_mtk_c2store.so: " << dlerror();
        return nullptr;
    }
    // 优先用 blob 自己导出的工厂函数：它内部 new 出来的对象大小是对的。
    // 之前手写 calloc(8192) + ctor 的做法在视频组件上会出问题
    // （createInterface 直接返回 C2_TIMED_OUT，音频组件却正常）。
    using GetStoreFn = std::shared_ptr<C2ComponentStore> (*)();
    GetStoreFn getStore = reinterpret_cast<GetStoreFn>(
            dlsym(handle, "_ZN7android26GetCodec2MtkComponentStoreEv"));
    if (getStore != nullptr) {
        std::shared_ptr<C2ComponentStore> store = getStore();
        if (store != nullptr) {
            LOG(INFO) << "MTK C2ComponentStore obtained via GetCodec2MtkComponentStore()";
            return store;
        }
        LOG(ERROR) << "GetCodec2MtkComponentStore() returned nullptr";
    } else {
        LOG(ERROR) << "cannot dlsym GetCodec2MtkComponentStore: " << dlerror();
    }

    using CtorFn = void (*)(void*);
    CtorFn ctor = reinterpret_cast<CtorFn>(
            dlsym(handle, "_ZN7android19C2MtkComponentStoreC1Ev"));
    if (ctor == nullptr) {
        LOG(ERROR) << "cannot dlsym C2MtkComponentStore ctor: " << dlerror();
        return nullptr;
    }
    void* raw = std::calloc(1, 8192);
    if (raw == nullptr) {
        return nullptr;
    }
    ctor(raw);
    LOG(INFO) << "MTK C2ComponentStore instantiated as AIDL default store backend";
    return std::shared_ptr<C2ComponentStore>(
            static_cast<C2ComponentStore*>(raw),
            [](C2ComponentStore*) { /* 进程常驻，故意不析构 */ });
}

void runAidlService() {
    ABinderProcess_setThreadPoolMaxThreadCount(kThreadCount);
    ABinderProcess_startThreadPool();

    // Create IComponentStore service.
    using namespace ::aidl::android::hardware::media::c2;
    std::shared_ptr<IComponentStore> store;

    // TODO: Replace this with
    // store = new utils::ComponentStore(
    //         /* implementation of C2ComponentStore */);
    LOG(DEBUG) << "Instantiating Codec2's IComponentStore service...";
    std::shared_ptr<C2ComponentStore> backend = createMtkComponentStore();
    if (backend != nullptr) {
        auto traitsList = backend->listComponents();
        LOG(INFO) << "MTK store lists " << traitsList.size() << " components";
        for (const auto& t : traitsList) {
            LOG(INFO) << "  - " << (t ? t->name : C2String("(null)"));
        }
    }
    if (backend == nullptr) {
        LOG(ERROR) << "falling back to empty store";
        backend = std::make_shared<StoreImpl>();
    }
    store = ::ndk::SharedRefBase::make<utils::ComponentStore>(backend);

    if (store == nullptr) {
        LOG(ERROR) << "Cannot create Codec2's IComponentStore service.";
    } else {
        const std::string serviceName =
            std::string(IComponentStore::descriptor) + "/default";
        binder_exception_t ex = AServiceManager_addService(
                store->asBinder().get(), serviceName.c_str());
        if (ex != EX_NONE) {
            LOG(ERROR) << "Cannot register Codec2's IComponentStore service"
                          " with instance name << \""
                       << serviceName << "\", ex=" << static_cast<int>(ex)
                       << " (" << strerror(-static_cast<int>(ex)) << ")";
        } else {
            LOG(DEBUG) << "Codec2's IComponentStore service registered. "
                          "Instance name: \"" << serviceName << "\".";
        }
    }

    ABinderProcess_joinThreadPool();
}

void runHidlService() {
    using namespace ::android;

    // Enable vndbinder to allow vendor-to-vendor binder calls.
    ProcessState::initWithDriver("/dev/vndbinder");

    ProcessState::self()->startThreadPool();
    hardware::configureRpcThreadpool(kThreadCount, true /* callerWillJoin */);

    // Create IComponentStore service.
    {
        using namespace ::android::hardware::media::c2::V1_2;
        sp<IComponentStore> store;

        // TODO: Replace this with
        // store = new utils::ComponentStore(
        //         /* implementation of C2ComponentStore */);
        LOG(DEBUG) << "Instantiating Codec2's IComponentStore service...";
        store = new utils::ComponentStore(
                std::make_shared<StoreImpl>());

        if (store == nullptr) {
            LOG(ERROR) << "Cannot create Codec2's IComponentStore service.";
        } else {
            constexpr char const* serviceName = "default";
            if (store->registerAsService(serviceName) != OK) {
                LOG(ERROR) << "Cannot register Codec2's IComponentStore service"
                              " with instance name << \""
                           << serviceName << "\".";
            } else {
                LOG(DEBUG) << "Codec2's IComponentStore service registered. "
                              "Instance name: \"" << serviceName << "\".";
            }
        }
    }

    hardware::joinRpcThreadpool();
}

int main(int /* argc */, char** /* argv */) {
    // [cannon][A17] A17 未生成 aconfig 的 IsEnabled()，且 codec2 已全面 AIDL 化，
// 这里固定走 AIDL 分支（否则 AIDL /default store 不会注册，MTK 硬解不可见）。
    const bool aidlEnabled = true;
    LOG(DEBUG) << "android.hardware.media.c2" << (aidlEnabled ? "-V1" : "@1.2")
               << "-service starting...";

    // Set up minijail to limit system calls.
    signal(SIGPIPE, SIG_IGN);
    android::SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);
    if (aidlEnabled) {
        runAidlService();
    } else {
        runHidlService();
    }
    return 0;
}
