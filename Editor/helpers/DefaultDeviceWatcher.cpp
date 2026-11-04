/*
	This file is part of EqualizerAPO-XT.
	SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "DefaultDeviceWatcher.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <atomic>
#include <mutex>
#include <QMetaObject>
#include "platform/windows/ComPtr.h"

namespace
{
struct Delivery
{
	std::mutex mutex;
	QObject* receiver = nullptr;
	std::function<void()> changed;
};

class NotificationClient final : public IMMNotificationClient
{
public:
	explicit NotificationClient(std::shared_ptr<Delivery> delivery) : delivery(std::move(delivery)) {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
	{
		if (result == nullptr) return E_POINTER;
		*result = nullptr;
		if (iid != __uuidof(IUnknown) && iid != __uuidof(IMMNotificationClient)) return E_NOINTERFACE;
		*result = static_cast<IMMNotificationClient*>(this);
		AddRef();
		return S_OK;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG remaining = --references;
		if (remaining == 0) delete this;
		return remaining;
	}
	HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
	{
		// DeviceAPOInfo uses eMultimedia, not the separate communications role.
		if (flow == eRender && role == eMultimedia)
		{
			// Destruction takes this lock before detaching the receiver, so it
			// cannot die between checking the pointer and posting the event.
			std::lock_guard<std::mutex> lock(delivery->mutex);
			if (delivery->receiver != nullptr)
				QMetaObject::invokeMethod(delivery->receiver, delivery->changed, Qt::QueuedConnection);
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
	std::atomic<ULONG> references{1};
	std::shared_ptr<Delivery> delivery;
};
}

struct DefaultDeviceWatcher::Impl
{
	winutil::ComApartment apartment;
	std::shared_ptr<Delivery> delivery = std::make_shared<Delivery>();
	winutil::ComPtr<IMMDeviceEnumerator> enumerator;
	winutil::ComPtr<IMMNotificationClient> client;
	bool registered = false;
};

DefaultDeviceWatcher::DefaultDeviceWatcher(std::function<void()> changed, QObject* parent,
	IMMDeviceEnumerator* enumerator)
	: QObject(parent), impl(std::make_unique<Impl>())
{
	impl->delivery->receiver = this;
	impl->delivery->changed = std::move(changed);
	if (enumerator != nullptr)
	{
		enumerator->AddRef();
		*impl->enumerator.put() = enumerator;
	}
	else if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
		__uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(impl->enumerator.put()))))
		return;
	*impl->client.put() = new NotificationClient(impl->delivery);
	impl->registered = SUCCEEDED(impl->enumerator->RegisterEndpointNotificationCallback(impl->client.get()));
}

DefaultDeviceWatcher::~DefaultDeviceWatcher()
{
	{
		std::lock_guard<std::mutex> lock(impl->delivery->mutex);
		impl->delivery->receiver = nullptr;
	}
	if (impl->registered)
		impl->enumerator->UnregisterEndpointNotificationCallback(impl->client.get());
}

bool DefaultDeviceWatcher::registered() const
{
	return impl->registered;
}
