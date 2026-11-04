/*
	This file is part of EqualizerAPO-XT.
	SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QObject>
#include <functional>
#include <memory>

struct IMMDeviceEnumerator;

// Core Audio calls on arbitrary threads. The callback is delivered on this
// object's Qt thread and is cancelled when the watcher is destroyed.
class DefaultDeviceWatcher final : public QObject
{
public:
	explicit DefaultDeviceWatcher(std::function<void()> changed, QObject* parent = nullptr,
		IMMDeviceEnumerator* enumerator = nullptr);
	~DefaultDeviceWatcher() override;
	bool registered() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
