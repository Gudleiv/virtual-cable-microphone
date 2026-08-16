#include "autostart.h"

#include "com.h"
#include "hresult.h"
#include "paths.h"
#include "version.h"

#define SECURITY_WIN32
#include <lmcons.h>
#include <security.h>
#include <secext.h>

#include <taskschd.h>

// The Windows SDK declares this; mingw-w64's taskschd.h stops short of it, and
// CMakeLists offers a mingw cross-build as a syntax check. The guard is the
// macro the generated headers themselves define, so on MSVC none of this is
// even compiled.
#ifndef __ILogonTrigger_INTERFACE_DEFINED__
#define __ILogonTrigger_INTERFACE_DEFINED__
// Defined, not just declared: taskschd.lib carries the symbol on MSVC, and
// there is no import library to take it from here.
static const GUID IID_ILogonTrigger = {
    0x72dade38, 0xfae4, 0x4b3e, {0xba, 0xf4, 0x5d, 0x00, 0x9a, 0xf0, 0x2b, 0x1c}};
MIDL_INTERFACE("72dade38-fae4-4b3e-baf4-5d009af02b1c")
ILogonTrigger : public ITrigger {
public:
    virtual HRESULT STDMETHODCALLTYPE get_Delay(BSTR * pDelay) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Delay(BSTR delay) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_UserId(BSTR * pUser) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UserId(BSTR user) = 0;
};
#endif

#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <format>
#include <iterator>
#include <vector>

namespace vcmic {
namespace {

// The task definition is built through the object model rather than assembled
// as XML: put_XmlText validates against a schema whose element order is easy to
// get subtly wrong, and a typo there fails at registration time on the user's
// machine rather than here.

// Minimal BSTR holder. SysAllocString is the only allocation in this file.
class Bstr {
public:
    Bstr() noexcept = default;
    explicit Bstr(const wchar_t* text) noexcept : value_(::SysAllocString(text)) {}
    explicit Bstr(const std::wstring& text) noexcept : Bstr(text.c_str()) {}

    ~Bstr() {
        if (value_ != nullptr) {
            ::SysFreeString(value_);
        }
    }

    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;

    BSTR Get() const noexcept { return value_; }
    BSTR* Put() noexcept {
        if (value_ != nullptr) {
            ::SysFreeString(value_);
            value_ = nullptr;
        }
        return &value_;
    }

    bool empty() const noexcept { return value_ == nullptr || ::SysStringLen(value_) == 0; }
    std::wstring str() const {
        return value_ == nullptr ? std::wstring()
                                 : std::wstring(value_, ::SysStringLen(value_));
    }

private:
    BSTR value_ = nullptr;
};

class Variant {
public:
    Variant() noexcept { ::VariantInit(&value_); }
    ~Variant() { ::VariantClear(&value_); }

    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;

    // Takes a copy; the caller keeps ownership of its own string.
    void SetString(const std::wstring& text) {
        ::VariantClear(&value_);
        value_.vt = VT_BSTR;
        value_.bstrVal = ::SysAllocString(text.c_str());
    }

    VARIANT& Get() noexcept { return value_; }

private:
    VARIANT value_{};
};

// DOMAIN\user, which is the form the scheduler wants for both the principal and
// the logon trigger. Without it a logon trigger fires for every account on the
// machine, not just this one.
std::wstring CurrentUser() {
    ULONG length = 0;
    ::GetUserNameExW(NameSamCompatible, nullptr, &length);
    if (length > 1) {
        std::vector<wchar_t> buffer(length);
        if (::GetUserNameExW(NameSamCompatible, buffer.data(), &length) != 0) {
            return std::wstring(buffer.data());
        }
    }

    // Domain lookups can fail on a machine that has lost its network; the plain
    // account name still registers a working per-user task.
    wchar_t name[UNLEN + 1] = {};
    DWORD size = static_cast<DWORD>(std::size(name));
    if (::GetUserNameW(name, &size) != 0) {
        return std::wstring(name);
    }
    return std::wstring();
}

const wchar_t* StateName(TASK_STATE state) {
    switch (state) {
        case TASK_STATE_DISABLED:
            return L"Disabled";
        case TASK_STATE_QUEUED:
            return L"Queued";
        case TASK_STATE_READY:
            return L"Ready";
        case TASK_STATE_RUNNING:
            return L"Running";
        case TASK_STATE_UNKNOWN:
        default:
            return L"Unknown";
    }
}

std::wstring FormatDate(DATE date) {
    if (date == 0.0) {
        return std::wstring();
    }
    SYSTEMTIME time{};
    if (::VariantTimeToSystemTime(date, &time) == FALSE) {
        return std::wstring();
    }
    return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}", time.wYear, time.wMonth, time.wDay,
                       time.wHour, time.wMinute, time.wSecond);
}

// Connects to the local scheduler and opens the root folder, which is where a
// task belongs if anybody is ever going to find it in taskschd.msc.
HRESULT OpenRootFolder(ComPtr<ITaskService>& service, ComPtr<ITaskFolder>& folder,
                       std::wstring& error) {
    HRESULT hr = ::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_ITaskService, service.PutVoid());
    if (FAILED(hr)) {
        error = std::format(L"cannot reach the Task Scheduler service: {}", FormatHresult(hr));
        return hr;
    }

    Variant empty;
    hr = service->Connect(empty.Get(), empty.Get(), empty.Get(), empty.Get());
    if (FAILED(hr)) {
        error = std::format(L"Task Scheduler refused the connection: {}", FormatHresult(hr));
        return hr;
    }

    const Bstr root(L"\\");
    hr = service->GetFolder(root.Get(), folder.Put());
    if (FAILED(hr)) {
        error = std::format(L"cannot open the root task folder: {}", FormatHresult(hr));
    }
    return hr;
}

HRESULT ApplySettings(ITaskDefinition* definition, std::vector<std::wstring>& notes,
                      std::wstring& error) {
    ComPtr<ITaskSettings> settings;
    HRESULT hr = definition->get_Settings(settings.Put());
    if (FAILED(hr)) {
        error = std::format(L"get_Settings: {}", FormatHresult(hr));
        return hr;
    }

    // A setting the scheduler quietly declines is worse than one that fails
    // loudly - the task would still register, and then behave in a way nothing
    // in the config explains. Collect them and let the caller say so.
    const auto note = [&notes](const wchar_t* what, HRESULT result) {
        if (FAILED(result)) {
            notes.push_back(std::format(L"{} ({})", what, FormatHresult(result)));
        }
    };

    note(L"Enabled", settings->put_Enabled(VARIANT_TRUE));
    note(L"AllowDemandStart", settings->put_AllowDemandStart(VARIANT_TRUE));
    note(L"MultipleInstances", settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW));

    // Every one of these defaults is wrong for a resident audio mixer.
    //
    // The battery pair stops the task on any laptop the moment it is unplugged.
    // ExecutionTimeLimit defaults to three days, after which the scheduler
    // would kill a perfectly healthy process. And the default task priority of
    // 7 maps to BELOW_NORMAL_PRIORITY_CLASS, which is not what you want under a
    // full-screen game - 5 is the normal class.
    note(L"DisallowStartIfOnBatteries", settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE));
    note(L"StopIfGoingOnBatteries", settings->put_StopIfGoingOnBatteries(VARIANT_FALSE));
    const Bstr no_limit(L"PT0S");
    note(L"ExecutionTimeLimit", settings->put_ExecutionTimeLimit(no_limit.Get()));
    note(L"Priority", settings->put_Priority(5));

    note(L"RunOnlyIfIdle", settings->put_RunOnlyIfIdle(VARIANT_FALSE));
    note(L"StartWhenAvailable", settings->put_StartWhenAvailable(VARIANT_TRUE));
    note(L"RunOnlyIfNetworkAvailable", settings->put_RunOnlyIfNetworkAvailable(VARIANT_FALSE));
    // Suppresses the console window this console application would otherwise
    // flash on screen at every logon.
    note(L"Hidden", settings->put_Hidden(VARIANT_TRUE));
    note(L"WakeToRun", settings->put_WakeToRun(VARIANT_FALSE));
    note(L"AllowHardTerminate", settings->put_AllowHardTerminate(VARIANT_TRUE));

    // If the mixer dies in a way it cannot recover from, the render stream is
    // gone and ShadowPlay records silence. Coming back three times beats not
    // noticing until after the session.
    const Bstr restart_interval(L"PT1M");
    note(L"RestartInterval", settings->put_RestartInterval(restart_interval.Get()));
    note(L"RestartCount", settings->put_RestartCount(3));

    ComPtr<IIdleSettings> idle;
    hr = settings->get_IdleSettings(idle.Put());
    if (SUCCEEDED(hr)) {
        note(L"StopOnIdleEnd", idle->put_StopOnIdleEnd(VARIANT_FALSE));
        note(L"RestartOnIdle", idle->put_RestartOnIdle(VARIANT_FALSE));
    } else {
        note(L"IdleSettings", hr);
    }
    return S_OK;
}

HRESULT ApplyTrigger(ITaskDefinition* definition, const std::wstring& user, std::uint32_t delay_s,
                     std::wstring& error) {
    ComPtr<ITriggerCollection> triggers;
    HRESULT hr = definition->get_Triggers(triggers.Put());
    if (FAILED(hr)) {
        error = std::format(L"get_Triggers: {}", FormatHresult(hr));
        return hr;
    }

    ComPtr<ITrigger> trigger;
    hr = triggers->Create(TASK_TRIGGER_LOGON, trigger.Put());
    if (FAILED(hr)) {
        error = std::format(L"cannot create the logon trigger: {}", FormatHresult(hr));
        return hr;
    }

    ComPtr<ILogonTrigger> logon;
    hr = trigger->QueryInterface(IID_ILogonTrigger, logon.PutVoid());
    if (FAILED(hr)) {
        error = std::format(L"ILogonTrigger: {}", FormatHresult(hr));
        return hr;
    }

    const Bstr id(L"logon");
    logon->put_Id(id.Get());
    logon->put_Enabled(VARIANT_TRUE);
    if (!user.empty()) {
        const Bstr user_id(user);
        logon->put_UserId(user_id.Get());
    }
    if (delay_s > 0) {
        const Bstr delay(std::format(L"PT{}S", delay_s));
        logon->put_Delay(delay.Get());
    }
    return S_OK;
}

HRESULT ApplyAction(ITaskDefinition* definition, const std::wstring& arguments,
                    std::wstring& error) {
    ComPtr<IActionCollection> actions;
    HRESULT hr = definition->get_Actions(actions.Put());
    if (FAILED(hr)) {
        error = std::format(L"get_Actions: {}", FormatHresult(hr));
        return hr;
    }

    ComPtr<IAction> action;
    hr = actions->Create(TASK_ACTION_EXEC, action.Put());
    if (FAILED(hr)) {
        error = std::format(L"cannot create the exec action: {}", FormatHresult(hr));
        return hr;
    }

    ComPtr<IExecAction> exec;
    hr = action->QueryInterface(IID_IExecAction, exec.PutVoid());
    if (FAILED(hr)) {
        error = std::format(L"IExecAction: {}", FormatHresult(hr));
        return hr;
    }

    const std::filesystem::path exe = ExecutablePath();
    if (exe.empty()) {
        error = L"cannot determine this executable's own path";
        return E_FAIL;
    }

    const Bstr path(exe.wstring());
    exec->put_Path(path.Get());
    if (!arguments.empty()) {
        const Bstr args(arguments);
        exec->put_Arguments(args.Get());
    }
    // The config and the log resolve next to the executable, but a scheduled
    // task otherwise starts in system32, and anything relative would land there.
    const Bstr working(exe.parent_path().wstring());
    exec->put_WorkingDirectory(working.Get());
    return S_OK;
}

}  // namespace

HRESULT InstallAutostart(std::uint32_t delay_s, const std::wstring& arguments,
                         std::vector<std::wstring>& notes, std::wstring& error) {
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> folder;
    HRESULT hr = OpenRootFolder(service, folder, error);
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<ITaskDefinition> definition;
    hr = service->NewTask(0, definition.Put());
    if (FAILED(hr)) {
        error = std::format(L"NewTask: {}", FormatHresult(hr));
        return hr;
    }

    ComPtr<IRegistrationInfo> registration;
    if (SUCCEEDED(definition->get_RegistrationInfo(registration.Put()))) {
        const Bstr author(kAppName);
        const Bstr description(std::format(
            L"{} - starts the mixer {} after logon, in the interactive session so that it can see "
            L"the audio endpoints.",
            kAppSummary, delay_s > 0 ? std::format(L"{} s", delay_s) : std::wstring(L"immediately")));
        registration->put_Author(author.Get());
        registration->put_Description(description.Get());
    }

    const std::wstring user = CurrentUser();

    ComPtr<IPrincipal> principal;
    if (SUCCEEDED(definition->get_Principal(principal.Put()))) {
        if (!user.empty()) {
            const Bstr user_id(user);
            principal->put_UserId(user_id.Get());
        }
        // Interactive token, least privilege: the process must land in the
        // user's own session, and it has no reason at all to be elevated.
        principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        principal->put_RunLevel(TASK_RUNLEVEL_LUA);
    }

    hr = ApplySettings(definition.Get(), notes, error);
    if (FAILED(hr)) {
        return hr;
    }
    hr = ApplyTrigger(definition.Get(), user, delay_s, error);
    if (FAILED(hr)) {
        return hr;
    }
    hr = ApplyAction(definition.Get(), arguments, error);
    if (FAILED(hr)) {
        return hr;
    }

    Variant user_variant;
    if (!user.empty()) {
        user_variant.SetString(user);
    }
    Variant empty;

    const Bstr name(kAutostartTaskName);
    ComPtr<IRegisteredTask> registered;
    hr = folder->RegisterTaskDefinition(name.Get(), definition.Get(), TASK_CREATE_OR_UPDATE,
                                        user_variant.Get(), empty.Get(),
                                        TASK_LOGON_INTERACTIVE_TOKEN, empty.Get(),
                                        registered.Put());
    if (FAILED(hr)) {
        error = std::format(L"registering the task failed: {}", FormatHresult(hr));
    }
    return hr;
}

HRESULT RemoveAutostart(std::wstring& error) {
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> folder;
    HRESULT hr = OpenRootFolder(service, folder, error);
    if (FAILED(hr)) {
        return hr;
    }

    const Bstr name(kAutostartTaskName);
    hr = folder->DeleteTask(name.Get(), 0);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        return S_FALSE;  // nothing registered; the caller says so and exits ok
    }
    if (FAILED(hr)) {
        error = std::format(L"deleting the task failed: {}", FormatHresult(hr));
    }
    return hr;
}

HRESULT QueryAutostart(AutostartInfo& info, std::wstring& error) {
    info = AutostartInfo();

    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> folder;
    HRESULT hr = OpenRootFolder(service, folder, error);
    if (FAILED(hr)) {
        return hr;
    }

    const Bstr name(kAutostartTaskName);
    ComPtr<IRegisteredTask> task;
    hr = folder->GetTask(name.Get(), task.Put());
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        return S_OK;  // not installed, which is not a failure
    }
    if (FAILED(hr)) {
        error = std::format(L"cannot read the task: {}", FormatHresult(hr));
        return hr;
    }
    info.installed = true;

    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (SUCCEEDED(task->get_Enabled(&enabled))) {
        info.enabled = enabled != VARIANT_FALSE;
    }

    TASK_STATE state = TASK_STATE_UNKNOWN;
    if (SUCCEEDED(task->get_State(&state))) {
        info.state = StateName(state);
    }

    DATE last_run = 0.0;
    if (SUCCEEDED(task->get_LastRunTime(&last_run))) {
        info.last_run = FormatDate(last_run);
    }
    LONG last_result = 0;
    if (SUCCEEDED(task->get_LastTaskResult(&last_result))) {
        info.last_result = std::format(L"{}", last_result);
    }

    ComPtr<ITaskDefinition> definition;
    if (FAILED(task->get_Definition(definition.Put()))) {
        return S_OK;
    }

    ComPtr<IPrincipal> principal;
    if (SUCCEEDED(definition->get_Principal(principal.Put()))) {
        Bstr user;
        if (SUCCEEDED(principal->get_UserId(user.Put()))) {
            info.user = user.str();
        }
    }

    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    if (SUCCEEDED(definition->get_Actions(actions.Put())) &&
        SUCCEEDED(actions->get_Item(1, action.Put()))) {
        ComPtr<IExecAction> exec;
        if (SUCCEEDED(action->QueryInterface(IID_IExecAction, exec.PutVoid()))) {
            Bstr path;
            if (SUCCEEDED(exec->get_Path(path.Put()))) {
                info.command = path.str();
            }
            Bstr args;
            if (SUCCEEDED(exec->get_Arguments(args.Put()))) {
                info.arguments = args.str();
            }
        }
    }

    ComPtr<ITriggerCollection> triggers;
    ComPtr<ITrigger> trigger;
    if (SUCCEEDED(definition->get_Triggers(triggers.Put())) &&
        SUCCEEDED(triggers->get_Item(1, trigger.Put()))) {
        ComPtr<ILogonTrigger> logon;
        if (SUCCEEDED(trigger->QueryInterface(IID_ILogonTrigger, logon.PutVoid()))) {
            Bstr delay;
            if (SUCCEEDED(logon->get_Delay(delay.Put())) && !delay.empty()) {
                // Only ever written by InstallAutostart, so PT<n>S is the only
                // shape that has to be understood here.
                const std::wstring text = delay.str();
                if (text.size() > 3 && text.compare(0, 2, L"PT") == 0 && text.back() == L'S') {
                    info.delay_s = static_cast<std::uint32_t>(
                        std::wcstoul(text.c_str() + 2, nullptr, 10));
                }
            }
        }
    }
    return S_OK;
}

}  // namespace vcmic
