/*
	This file is part of EqualizerAPO-XT, a system-wide equalizer.
	Copyright (C) 2026 115dkk
	SPDX-License-Identifier: GPL-2.0-or-later
*/

/*
	Logic moved from Editor/guis/VSTPluginFilterGUI.cpp (Copyright (C) 2017
	Jonas Thedering) and its port in VSTCardEditor.cpp.
*/

#include "VSTPluginSession.h"

#include <QAbstractEventDispatcher>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QWidget>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "services/security/AudioEngineAccess.h"
#include "Editor/guis/VSTPluginFilterGUIDialog.h"
#include "Editor/helpers/PluginLoadQueue.h"
#include "Editor/helpers/VstChunkScan.h"

using std::unordered_map;
using std::wstring;

VSTPluginSession::VSTPluginSession(Row row, std::shared_ptr<VSTPluginLibrary> library, wstring chunkData,
	unordered_map<wstring, float> paramMap, wstring classId)
	: row(row), pluginLibrary(std::move(library)), selectedClassId(std::move(classId)), currentChunkData(std::move(chunkData)),
	currentParamMap(std::move(paramMap))
{
}

VSTPluginSession::~VSTPluginSession()
{
	previewFeeder.stop();
	releasePanelProcessing();
}

const std::shared_ptr<VSTPluginLibrary>& VSTPluginSession::library() const
{
	return pluginLibrary;
}

VSTPluginInstance* VSTPluginSession::instance() const
{
	return effect.get();
}

const wstring& VSTPluginSession::chunkData() const
{
	return currentChunkData;
}

const unordered_map<wstring, float>& VSTPluginSession::paramMap() const
{
	return currentParamMap;
}

const VSTPluginSession::Status& VSTPluginSession::status() const
{
	return currentStatus;
}

bool VSTPluginSession::embedded() const
{
	return isEmbedded;
}

bool VSTPluginSession::autoApplyDialog() const
{
	return autoApply;
}

void VSTPluginSession::setAutoApplyDialog(bool value)
{
	autoApply = value;
}

bool VSTPluginSession::loading() const
{
	return currentStatus.loading;
}

QWidget* VSTPluginSession::createClassSelector(QWidget* parent)
{
	auto* selector = new QWidget(parent);
	auto* layout = new QHBoxLayout(selector);
	layout->setContentsMargins(0, 0, 0, 0);
	auto* label = new QLabel(tr("Effect"), selector);
	auto* combo = new QComboBox(selector);
	combo->setObjectName(QStringLiteral("VSTClassSelector"));
	combo->setAccessibleName(tr("VST3 effect"));
	combo->setToolTip(tr("Choose the effect inside this VST3 module. Changing it resets the plugin state and channel routing."));
	label->setBuddy(combo);
	layout->addWidget(label);
	layout->addWidget(combo, 1);
	selector->hide();
	connect(this, &VSTPluginSession::loadFinished, selector, [this, selector, combo]() {
		QSignalBlocker blocker(combo);
		combo->clear();
		if (!pluginLibrary->isLoadedNow() || !pluginLibrary->isVST3())
		{
			selector->hide();
			return;
		}
		for (const auto& entry : pluginLibrary->getVST3Classes())
		{
			combo->addItem(QString::fromUtf8(entry.info.name), QString::fromStdWString(entry.id));
			combo->setItemData(combo->count() - 1, QString::fromStdWString(entry.id), Qt::ToolTipRole);
		}
		int index = selectedClassId.empty() ? 0 : combo->findData(QString::fromStdWString(selectedClassId));
		if (index < 0)
		{
			combo->addItem(tr("Missing effect: %1").arg(QString::fromStdWString(selectedClassId)),
				QString::fromStdWString(selectedClassId));
			index = combo->count() - 1;
		}
		combo->setCurrentIndex(index);
		selector->setVisible(combo->count() > 1);
	});
	connect(combo, &QComboBox::activated, this, [this, combo](int index) {
		selectClass(combo->itemData(index).toString().toStdWString());
	});
	return selector;
}

void VSTPluginSession::selectClass(const wstring& id)
{
	if (loading() || !pluginLibrary->isVST3() || id == selectedClassId
		|| pluginLibrary->findVST3Class(id) == nullptr)
		return;
	const bool keepEmbed = isEmbedded || intent.embedPending();
	applyEmbed(false);
	previewFeeder.stop();
	releasePanelProcessing();
	intent.replacementStarted();
	pendingDialogParent = nullptr;
	loadGeneration++;
	effect.reset();
	selectedClassId = id;
	currentChunkData.clear();
	currentParamMap.clear();
	replacementPending = false;
	currentStatus = Status();
	emit classChanged();
	requestPlugin();
	if (keepEmbed)
		requestEmbed(true);
}

void VSTPluginSession::initPlugin()
{
	if (effect != nullptr || loading())
		return;

	if (pluginLibrary->getLibPath().empty())
	{
		// The card shows a missing library as a state, not as text.
		currentStatus = Status();
		currentStatus.libraryMissing = true;
		currentStatus.critical = true;
		if (row == Row::Legacy)
			currentStatus.text = QCoreApplication::translate("VSTPluginFilterGUI", "No file selected.");
		emit statusChanged();
		finishPendingReplacement();
		return;
	}

	applyLoadResult(VSTPluginLibrary::load(pluginLibrary->getLibPath()).status);
}

void VSTPluginSession::requestPlugin()
{
	if (effect != nullptr || loading())
		return;

	if (pluginLibrary->getLibPath().empty())
	{
		initPlugin();
		finishLoad();
		return;
	}

	// A library this process holds already needs no file system work. Any
	// other, a missing one included, is judged and loaded on the loader
	// thread: telling whether the file is there reaches the file system,
	// which may be an unreachable share (audit #422 TD-04).
	if (!PluginLoadQueue::isPending(pluginLibrary.get()) && pluginLibrary->isLoadedNow())
	{
		initPlugin();
		finishLoad();
		return;
	}

	currentStatus = Status();
	currentStatus.loading = true;
	emit statusChanged();

	const unsigned generation = loadGeneration;
	QPointer<VSTPluginSession> guard(this);
	PluginLoadQueue::enqueue(pluginLibrary, [guard, generation](int result) {
		if (guard.isNull() || guard->loadGeneration != generation)
			return;
		guard->currentStatus.loading = false;
		guard->applyLoadResult(result);
		guard->finishLoad();
	});
}

int VSTPluginSession::pendingLoads()
{
	return PluginLoadQueue::pendingCount();
}

bool VSTPluginSession::waitForPendingLoads(int timeoutMs)
{
	return PluginLoadQueue::waitForPendingLoads(timeoutMs);
}

void VSTPluginSession::applyLoadResult(int result)
{
	const bool card = row == Row::Card;
	currentStatus = Status();
	if (result < 0)
	{
		currentStatus.critical = true;

		switch (result)
		{
		case AbstractLibrary::FILE_NOT_FOUND:
			currentStatus.libraryMissing = true;
			if (!card)
				currentStatus.text = QCoreApplication::translate("VSTPluginFilterGUI", "File not found.");
			break;
		case AbstractLibrary::LOADING_FAILED:
			currentStatus.text = QCoreApplication::translate("VSTPluginFilterGUI", "Library could not be loaded.");
			break;
		case AbstractLibrary::FUNCTIONS_MISSING:
			currentStatus.text = QCoreApplication::translate("VSTPluginFilterGUI", "Library does not contain needed functions.");
			break;
		case AbstractLibrary::WRONG_ARCHITECTURE:
		{
#ifdef _WIN64
			int bitDepth = 64;
#else
			int bitDepth = 32;
#endif
			currentStatus.text = QCoreApplication::translate("VSTPluginFilterGUI",
				"Library has the wrong architecture. Only %1-bit libraries are supported.").arg(bitDepth);
			break;
		}
		}
	}
	else if (!selectedClassId.empty() && (!pluginLibrary->isVST3()
		|| pluginLibrary->findVST3Class(selectedClassId) == nullptr))
	{
		currentStatus.critical = true;
		currentStatus.text = tr("VST3 effect %1 was not found in this library.").arg(QString::fromStdWString(selectedClassId));
	}
	else
	{
		effect = VSTPluginInstance::create(pluginLibrary, 1, selectedClassId);
		if (effect->initialize())
		{
			if (pluginLibrary->isVST3())
				selectedClassId = pluginLibrary->findVST3Class(selectedClassId)->id;
			effect->setLanguage(QLocale().language() == QLocale::German ? 2 : 1);
			effect->setAutomateFunc([this]() { onAutomate(); });
			currentStatus.text = QString::fromStdWString(effect->getName());
		}
		else
		{
			effect.reset();
			currentStatus.critical = true;
			currentStatus.text = QCoreApplication::translate("VSTPluginFilterGUI", "Plugin crashed during initialization.");
		}
	}

	emit statusChanged();
	finishPendingReplacement();
}

bool VSTPluginSession::libraryDiffers(const QString& writtenPath) const
{
	return QString::fromStdWString(pluginLibrary->getLibPath()) != writtenPath;
}

void VSTPluginSession::replaceLibrary(const QString& writtenPath)
{
	// The embed option outlives the switch: the panel closes without an
	// embedStateChanged (the option stays on), and the new plug-in embeds
	// below, now or once its load lands.
	const bool keepEmbed = isEmbedded || intent.embedPending();
	applyEmbed(false);
	intent.replacementStarted();

	// A replacement that lands before the previous one finished compares
	// against the plug-in the row started from, not the one never loaded.
	if (!replacementPending)
		replacementOldId = effect != nullptr && !effect->isVST3() ? effect->uniqueID() : 0;
	effect.reset();
	replacementPending = true;

	QDir pluginsDir(QString::fromStdWString(VSTPluginLibrary::getDefaultPluginPath()));
	QString path = writtenPath;
	if (path.length() > 0)
		path = QDir::toNativeSeparators(QFileInfo(pluginsDir, writtenPath).absoluteFilePath());
	pluginLibrary = VSTPluginLibrary::getInstance(path.toStdWString());
	selectedClassId.clear();
	loadGeneration++;
	currentStatus = Status();
	requestPlugin();
	if (keepEmbed)
		requestEmbed(true);
}

void VSTPluginSession::finishLoad()
{
	const VSTPanelIntent::Completion done = intent.loadFinished(effect != nullptr);
	if (done.storeLine)
		emit stateChanged();
	emit loadFinished();
	if (done.embed)
		requestEmbed(true);
	else if (done.dropEmbed)
		emit embedStateChanged();
	QWidget* dialogParent = pendingDialogParent;
	pendingDialogParent = nullptr;
	if (done.openDialogUnlessEmbedded && effect != nullptr && !isEmbedded && dialogParent != nullptr)
		openDialog(dialogParent);
}

void VSTPluginSession::setEmbedHost(QWidget* host, std::function<void(bool shown)> showHost)
{
	embedHost = host;
	showEmbedHost = std::move(showHost);
}

void VSTPluginSession::requestEmbed(bool enable)
{
	if (!enable)
		intent.dropEmbed();
	else if (effect == nullptr)
	{
		requestPlugin();
		if (loading())
		{
			// The saved embed preference holds while the library loads.
			intent.parkEmbed();
			emit embedStateChanged();
			return;
		}
	}
	applyEmbed(enable && effect != nullptr);
	emit embedStateChanged();
}

bool VSTPluginSession::embedPending() const
{
	return intent.embedPending();
}

bool VSTPluginSession::requestPanel(QWidget* dialogParent)
{
	// The panel is already on screen inside the row; opening the dialog on
	// top would steal the embedded view's window (startEditing recreates the
	// view for the dialog and the row would keep showing nothing).
	if (isEmbedded)
		return false;

	if (effect == nullptr)
	{
		// Parked before the request: a load that completes at once opens the
		// dialog from finishLoad.
		intent.parkDialog();
		pendingDialogParent = dialogParent;
		requestPlugin();
		return true;
	}

	openDialog(dialogParent);
	return false;
}

void VSTPluginSession::applyEmbed(bool enable)
{
	if (enable == isEmbedded)
		return;
	// The host is shown before the panel embeds into it, and hidden again
	// when embedding failed (reported through the status).
	if (showEmbedHost)
		showEmbedHost(enable);
	if (!setEmbedded(enable, embedHost) && showEmbedHost)
		showEmbedHost(false);
}

void VSTPluginSession::finishPendingReplacement()
{
	if (!replacementPending)
		return;
	replacementPending = false;
	// Only two VST2 instances can use the legacy 32-bit compatibility key.
	if (effect == nullptr || effect->isVST3() || replacementOldId == 0 || effect->uniqueID() != replacementOldId)
	{
		currentChunkData.clear();
		currentParamMap.clear();
	}
	replacementOldId = 0;
}

void VSTPluginSession::openDialog(QWidget* dialogParent)
{
	if (effect == nullptr)
		return;

	effect->writeToEffect(currentChunkData, currentParamMap);

	// Before the dialog's startEditing: the feed prepares the instance for
	// the loopback mix format, which must happen while the processor is
	// still deactivated.
	previewFeeder.start(effect.get());

	VSTPluginFilterGUIDialog dialog(dialogParent, effect.get(), autoApply);
	if (!dialog.isEditorOpen())
	{
		// Same report as a failed embed, instead of an empty dialog.
		previewFeeder.stop();
		reportPanelCrash();
		return;
	}
	acquirePanelProcessing();
	connect(dialog.getApplyButton(), &QPushButton::pressed, this, &VSTPluginSession::applyDialog);
	connect(dialog.getAutoApplyCheckBox(), &QCheckBox::toggled, this, &VSTPluginSession::setAutoApplyDialog);
	connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock,
		this, &VSTPluginSession::onIdle);

	if (dialog.exec() == QDialog::Accepted)
	{
		effect->readFromEffect(currentChunkData, currentParamMap);
		emit stateChanged();
	}
	disconnect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock,
		this, &VSTPluginSession::onIdle);
	previewFeeder.stop();
	releasePanelProcessing();
}

bool VSTPluginSession::setEmbedded(bool enable, QWidget* host)
{
	if (effect == nullptr)
		enable = false;
	if (enable == isEmbedded)
		return isEmbedded;

	isEmbedded = enable;
	if (enable)
	{
		// Before embedPlugin()'s startEditing, for the same deactivation
		// contract as the dialog path.
		previewFeeder.start(effect.get());

		if (embedPlugin(host))
		{
			acquirePanelProcessing();
			// Moving to a display with another scale changes the top-level
			// window's device pixel ratio; eventFilter re-scales the panel.
			embeddedScale = host->devicePixelRatioF();
			scaleWatchWindow = host->window();
			scaleWatchWindow->installEventFilter(this);
			effect->setSizeWindowFunc([this](int width, int height) {
				if (isEmbedded)
					emit sizeRequested(width, height);
			});
			connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock,
				this, &VSTPluginSession::onIdle);
		}
		else
		{
			previewFeeder.stop();
			isEmbedded = false;
			reportPanelCrash();
		}
	}
	else
	{
		previewFeeder.stop();
		if (scaleWatchWindow)
			scaleWatchWindow->removeEventFilter(this);
		scaleWatchWindow = nullptr;
		if (effect != nullptr)
		{
			releasePanelProcessing();
			effect->stopEditing();
			effect->setSizeWindowFunc(nullptr);
		}
		disconnect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock,
			this, &VSTPluginSession::onIdle);
	}
	return isEmbedded;
}

bool VSTPluginSession::eventFilter(QObject* watched, QEvent* event)
{
	if (event->type() == QEvent::DevicePixelRatioChange && watched == scaleWatchWindow.data()
		&& isEmbedded && effect != nullptr)
	{
		// The window's own ratio: the event reports its change, while the
		// embedded native child may not have taken the new screen yet.
		const double scale = scaleWatchWindow->devicePixelRatioF();
		if (!qFuzzyCompare(scale, embeddedScale))
		{
			embeddedScale = scale;
			// The plug-in lays out again and asks for its new size through
			// sizeRequested.
			effect->setEditorScale(scale);
		}
	}
	return QObject::eventFilter(watched, event);
}

QString VSTPluginSession::libraryPermissionWarning() const
{
	if (pluginLibrary->getLibPath().empty()
		|| AudioEngineAccess::isReadableByAudioEngine(pluginLibrary->getLibPath()))
		return QString();
	return QCoreApplication::translate("VSTPluginFilterGUI",
		"The library is not readable by the audio service.\nChange the file permissions or copy the file to the VSTPlugins directory.");
}

// Evaluated from the saved plug-in state alone, never gated on a loaded
// instance: gated on one, the warning appeared when a panel opened and
// silently vanished on the next row rebuild, while the files stayed
// unreadable for the audio service - a real problem reading as a false
// alarm.
QString VSTPluginSession::chunkPermissionWarning() const
{
	const QStringList files = vstChunkUnreadablePaths(currentChunkData);
	if (files.isEmpty())
		return QString();
	return QCoreApplication::translate("VSTPluginFilterGUI", "The plugin seemingly accesses these files not readable by the audio service:\n"
		"%0\n"
		"Change the file permissions or copy the files to the config directory.")
		.arg(files.join("\n"));
}

void VSTPluginSession::applyDialog()
{
	effect->readFromEffect(currentChunkData, currentParamMap);
	emit stateChanged();
}

void VSTPluginSession::onIdle()
{
	if (effect == nullptr)
		return;

	effect->doIdle();

	if (isEmbedded || autoApply)
	{
		if (!lastReadTimer.isValid() || lastReadTimer.elapsed() > 1000)
		{
			wstring newChunkData;
			unordered_map<wstring, float> newParamMap;
			effect->readFromEffect(newChunkData, newParamMap);
			if (newChunkData != currentChunkData || newParamMap != currentParamMap)
			{
				currentChunkData = newChunkData;
				currentParamMap = newParamMap;
				emit stateChanged();
			}
			lastReadTimer.restart();
		}
	}
}

void VSTPluginSession::onAutomate()
{
	if (isEmbedded || autoApply)
	{
		effect->readFromEffect(currentChunkData, currentParamMap);
		emit automated();
	}
}

bool VSTPluginSession::embedPlugin(QWidget* host)
{
	bool result = true;

	__try
	{
		effect->writeToEffect(currentChunkData, currentParamMap);

		HWND hwnd = (HWND)host->winId();
		short width = 0, height = 0;

		// startEditing also fails without an exception (no view, attach
		// refused); unchecked, that embedded its 400x300 placeholder size as
		// an empty frame and reported the panel as open.
		result = effect->startEditing(hwnd, &width, &height, host->devicePixelRatioF());

		if (result)
			host->setFixedSize(width, height);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		result = false;
	}

	return result;
}

void VSTPluginSession::acquirePanelProcessing()
{
	if (effect == nullptr || effect->canProcessNow())
		return;
	effect->startProcessing();
	ownsPanelProcessing = effect->canProcessNow();
}

void VSTPluginSession::releasePanelProcessing()
{
	if (!ownsPanelProcessing || effect == nullptr)
		return;
	effect->stopProcessingSafely();
	ownsPanelProcessing = false;
}

void VSTPluginSession::reportPanelCrash()
{
	currentStatus.critical = true;
	currentStatus.libraryMissing = false;
	currentStatus.text = QCoreApplication::translate("VSTPluginFilterGUI", "Plugin crashed when opening panel.");
	emit statusChanged();
}
