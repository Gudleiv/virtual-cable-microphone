#pragma once

#include "win_headers.h"

#include <objbase.h>
#include <propidl.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace vcmic {

// Minimal intrusive COM smart pointer. WRL/ATL would do, but a 40-line class
// keeps the dependency surface at "Windows SDK only" and the semantics obvious.
template <class T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept {}

    // Takes ownership of an already-AddRef'd pointer.
    explicit ComPtr(T* raw) noexcept : ptr_(raw) {}

    ComPtr(const ComPtr& other) noexcept : ptr_(other.ptr_) {
        if (ptr_) {
            ptr_->AddRef();
        }
    }

    ComPtr(ComPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}

    ~ComPtr() { Reset(); }

    ComPtr& operator=(const ComPtr& other) noexcept {
        ComPtr copy(other);
        Swap(copy);
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = std::exchange(other.ptr_, nullptr);
        }
        return *this;
    }

    void Swap(ComPtr& other) noexcept { std::swap(ptr_, other.ptr_); }

    void Reset() noexcept {
        if (T* p = std::exchange(ptr_, nullptr)) {
            p->Release();
        }
    }

    // Out-parameter for factories: releases any current reference first.
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }

    void** PutVoid() noexcept { return reinterpret_cast<void**>(Put()); }

    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }


private:
    T* ptr_ = nullptr;
};

struct CoTaskMemDeleter {
    void operator()(void* p) const noexcept { ::CoTaskMemFree(p); }
};

// Owns a block the callee allocated with CoTaskMemAlloc (GetId, GetMixFormat...).
template <class T>
using CoTaskMemPtr = std::unique_ptr<T, CoTaskMemDeleter>;

// RAII for IPropertyStore::GetValue results.
class PropVariantHolder {
public:
    PropVariantHolder() noexcept { ::PropVariantInit(&pv_); }
    ~PropVariantHolder() { ::PropVariantClear(&pv_); }

    PropVariantHolder(const PropVariantHolder&) = delete;
    PropVariantHolder& operator=(const PropVariantHolder&) = delete;

    PROPVARIANT* Put() noexcept {
        ::PropVariantClear(&pv_);
        ::PropVariantInit(&pv_);
        return &pv_;
    }

    const PROPVARIANT& Get() const noexcept { return pv_; }

    std::wstring AsString() const {
        if (pv_.vt == VT_LPWSTR && pv_.pwszVal != nullptr) {
            return std::wstring(pv_.pwszVal);
        }
        if (pv_.vt == VT_BSTR && pv_.bstrVal != nullptr) {
            return std::wstring(pv_.bstrVal);
        }
        return std::wstring();
    }

    std::uint32_t AsUInt32(std::uint32_t fallback) const {
        switch (pv_.vt) {
            case VT_UI4:
                return pv_.ulVal;
            case VT_I4:
                return static_cast<std::uint32_t>(pv_.lVal);
            case VT_UINT:
                return pv_.uintVal;
            default:
                return fallback;
        }
    }

private:
    PROPVARIANT pv_{};
};

// CoInitializeEx/CoUninitialize pairing for the calling thread.
// Spec 4.1: multithreaded apartment.
class ComApartment {
public:
    ComApartment() noexcept : hr_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

    ~ComApartment() {
        if (SUCCEEDED(hr_)) {
            ::CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    HRESULT hr() const noexcept { return hr_; }
    bool ok() const noexcept { return SUCCEEDED(hr_); }

private:
    HRESULT hr_ = E_FAIL;
};

}  // namespace vcmic
