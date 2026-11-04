/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "EditorLogicTestSupport.h"
#include "Editor/helpers/DefaultDeviceWatcher.h"
#include <QCoreApplication>
#include <QThread>
#include <atomic>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>

namespace
{
class FakeEnumerator final : public IMMDeviceEnumerator
{
public:
	IMMNotificationClient* client = nullptr;
	std::atomic<ULONG> references{1};
	int unregisters = 0;
	bool rejectRegistration = false;
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
	{
		if (result == nullptr) return E_POINTER;
		*result = nullptr;
		if (iid != __uuidof(IUnknown) && iid != __uuidof(IMMDeviceEnumerator)) return E_NOINTERFACE;
		*result = static_cast<IMMDeviceEnumerator*>(this);
		AddRef();
		return S_OK;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
	ULONG STDMETHODCALLTYPE Release() override { return --references; }
	HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow, DWORD, IMMDeviceCollection**) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow, ERole, IMMDevice**) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR, IMMDevice**) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* value) override
	{
		if (rejectRegistration) return E_FAIL;
		client = value;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* value) override
	{
		if (client != value) return E_INVALIDARG;
		client = nullptr;
		++unregisters;
		return S_OK;
	}
};
}

TEST_CASE(EditorLogicTests, testDefaultDeviceNotificationsAreQueuedAndLifetimeSafe)
{
	FakeEnumerator enumerator;
	int calls = 0;
	bool onUiThread = true;
	auto watcher = std::make_unique<DefaultDeviceWatcher>([&]() {
		++calls;
		onUiThread = onUiThread && QThread::currentThread() == QCoreApplication::instance()->thread();
	}, nullptr, &enumerator);
	requireTrue(watcher->registered() && enumerator.client != nullptr, "default device watcher registers");
	auto* client = enumerator.client;
	client->AddRef(); // A Core Audio callback may still own a reference during shutdown.
	std::thread worker([client]() {
		client->OnDefaultDeviceChanged(eCapture, eMultimedia, L"microphone");
		client->OnDefaultDeviceChanged(eRender, eCommunications, L"communications");
		client->OnDefaultDeviceChanged(eRender, eConsole, L"console");
		client->OnDefaultDeviceChanged(eRender, eMultimedia, L"speakers");
		client->OnDefaultDeviceChanged(eRender, eMultimedia, nullptr);
	});
	worker.join();
	expectEqual(calls, 0, "Core Audio callbacks do not touch Qt UI on their worker thread");
	QCoreApplication::processEvents();
	expectEqual(calls, 2, "only the playback multimedia role and removal of its last endpoint are delivered");
	expectTrue(onUiThread, "notifications arrive on the Qt main thread");
	client->OnDefaultDeviceChanged(eRender, eMultimedia, L"queued-before-close");
	watcher.reset();
	client->OnDefaultDeviceChanged(eRender, eMultimedia, L"late-after-close");
	client->Release();
	QCoreApplication::processEvents();
	expectEqual(calls, 2, "queued and late notifications are discarded after destruction");
	expectEqual(enumerator.unregisters, 1, "registration is released exactly once");
	expectEqual(static_cast<int>(enumerator.references.load()), 1, "enumerator reference is balanced");
	enumerator.rejectRegistration = true;
	{
		DefaultDeviceWatcher failed([]() {}, nullptr, &enumerator);
		expectFalse(failed.registered(), "a failed registration is visible to the caller");
	}
	expectEqual(enumerator.unregisters, 1, "failed registrations are not unregistered");
}
