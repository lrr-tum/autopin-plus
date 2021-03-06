/*
 * This file is part of Autopin+.
 * Copyright (C) 2015 Technische Universität München - LRR
 *
 * This file is licensed under the GNU General Public License Version 3
 */

#include <AutopinPlus/Autopin.h>
#include <AutopinPlus/Watchdog.h>
#include <AutopinPlus/Logger/External/Main.h>
#include <AutopinPlus/Monitor/ClustSafe/Main.h>
#include <AutopinPlus/Monitor/GPerf/Main.h>
#include <AutopinPlus/Monitor/Random/Main.h>
#include <AutopinPlus/Monitor/PageMigrate/Main.h>
#include <AutopinPlus/Strategy/Autopin1/Main.h>
#include <AutopinPlus/Strategy/Noop/Main.h>
#include <AutopinPlus/Strategy/Compact/Main.h>
#include <AutopinPlus/Strategy/Scatter/Main.h>
#include <AutopinPlus/OS/OSServices.h>
#include <AutopinPlus/OS/SignalDispatcher.h>
#include <QString>
#include <QTimer>
#include <QList>
#include <memory>

namespace AutopinPlus {

std::atomic_int Watchdog::counter(0);

Watchdog::Watchdog(std::unique_ptr<const Configuration> config)
	: context(nullptr), config(std::move(config)), service(nullptr), process(nullptr), strategy(nullptr) {}

Watchdog::~Watchdog() { context->info("Watchdog destroyed"); }

void Watchdog::slot_watchdogRun() {
	createContext();
	createOSServices();
	createPerformanceMonitors();
	createObservedProcess();
	createControlStrategy();
	createDataLoggers();

	// Check for error
	if (context->isError()) return;

	context->info("Initializing environment ...");

	// Setup and initialize os services
	service->init();

	// Setup and initialize performance monitors
	context->info("Initializing performance monitors");
	
	for (auto &elem : monitors) {
		elem->init();
	}

	// Setup and initialize observed process
	process->init();

	// Setup and initialize pinning strategy
	context->info("Initializing control strategy");
	strategy->init();

	// Setup and initialize data loggers
	context->info("Initializing data loggers");
	for (auto &dlogger : loggers) {
		dlogger->init();
	}

	if (context->isError()) return;

	// Setup global Qt connections
	createComponentConnections();

	// Starting observed process
	context->info("Connecting to the observed process ...");
	process->start();
	int npid=process.get()->getPid();
	context->info("Starting control strategy ...");
	
	for (auto &elem : monitors) {
		elem->init();
		elem->setObservedProcessPid(npid);
		
	}
	
	emit sig_watchdogReady();
}

void Watchdog::createContext() {
	std::string name;

	if (config->configOptionExists("Name")) {
		name = config->getConfigOption("Name").toStdString();
	} else {
		name = "Watchdog " + std::to_string(Watchdog::counter);
	}

	context = std::unique_ptr<AutopinContext>(new AutopinContext(name));

	connect(context.get(), SIGNAL(sig_error()), this, SIGNAL(sig_watchdogStop()));

	Watchdog::counter++;
}

void Watchdog::createPerformanceMonitors() {
	QStringList config_monitors = config->getConfigOptionList("PerformanceMonitors");
	QStringList existing_ids;

	if (config_monitors.empty())
		context->report(Error::BAD_CONFIG, "option_missing", "No performance monitor configured");

	for (int i = 0; i < config_monitors.size(); i++) {
		QString current_monitor = config_monitors[i], current_type;

		if (existing_ids.contains(current_monitor))
			context->report(Error::BAD_CONFIG, "inconsistent",
							"The identifier " + current_monitor + " is already assigned to another monitor");

		int numtypes = config->configOptionExists(current_monitor + ".type");

		if (numtypes <= 0) {
			context->report(Error::BAD_CONFIG, "option_missing",
							"Type for monitor \"" + current_monitor + "\" is not specified");
		} else if (numtypes > 1) {
			context->report(Error::BAD_CONFIG, "inconsistent",
							"Specified " + QString::number(numtypes) + " types for monitor " + current_monitor);
		}

		current_type = config->getConfigOption(current_monitor + ".type");

		if (current_type == "clustsafe") {
			monitors.push_back(std::unique_ptr<Monitor::ClustSafe::Main>(
				new Monitor::ClustSafe::Main(current_monitor, *config, *context)));
			continue;
		}

		if (current_type == "gperf") {
			monitors.push_back(
				std::unique_ptr<Monitor::GPerf::Main>(new Monitor::GPerf::Main(current_monitor, *config, *context)));
			continue;
		}

		if (current_type == "random") {
			monitors.push_back(
				std::unique_ptr<Monitor::Random::Main>(new Monitor::Random::Main(current_monitor, *config, *context)));
			continue;
		}
		
		if (current_type == "pagemigrate") {
			monitors.push_back(
				std::unique_ptr<Monitor::PageMigrate::Main>(new Monitor::PageMigrate::Main(current_monitor, *config, *context)));
			continue;
		}

		context->report(Error::UNSUPPORTED, "critical",
						"Performance monitor type \"" + current_type + "\" is not supported");
	}
}

void Watchdog::createControlStrategy() {
	int optcount = config->configOptionExists("ControlStrategy");

	if (optcount <= 0) context->report(Error::BAD_CONFIG, "option_missing", "No control strategy configured");
	if (optcount > 1)
		context->report(Error::BAD_CONFIG, "inconsistent",
						"Specified " + QString::number(optcount) + " control strategies");

	QString strategy_config = config->getConfigOption("ControlStrategy");

	if (strategy_config == "autopin1") {
		strategy = std::unique_ptr<ControlStrategy>(
			new Strategy::Autopin1::Main(*config, *process, *service, monitors, *context));
		return;
	}

	if (strategy_config == "noop") {
		strategy =
			std::unique_ptr<ControlStrategy>(new Strategy::Noop::Main(*config, *process, *service, monitors, *context));
		return;
	}

	if (strategy_config == "compact") {
		strategy = std::unique_ptr<ControlStrategy>(
			new Strategy::Compact::Main(*config, *process, *service, monitors, *context));
		return;
	}

	if (strategy_config == "scatter") {
		strategy = std::unique_ptr<ControlStrategy>(
			new Strategy::Scatter::Main(*config, *process, *service, monitors, *context));
		return;
	}

	context->report(Error::UNSUPPORTED, "critical", "Control strategy \"" + strategy_config + "\" is not supported");
}

void Watchdog::createDataLoggers() {
	for (auto logger : config->getConfigOptionList("DataLoggers")) {
		if (logger == "external") {
			loggers.push_back(
				std::unique_ptr<Logger::External::Main>(new Logger::External::Main(*config, monitors, *context)));
		} else {
			context->report(Error::UNSUPPORTED, "critical", "Data logger \"" + logger + "\" is not supported");
			return;
		}
	}
}

void Watchdog::createOSServices() { service = std::unique_ptr<OS::OSServices>(new OS::OSServices(*context)); }

void Watchdog::createObservedProcess() {
	process = std::unique_ptr<ObservedProcess>(new ObservedProcess(*config, *service, *context));
}

void Watchdog::createComponentConnections() {
	using AutopinPlus::OS::SignalDispatcher;
	// Connection between SignalDispatcher and ObservedProcess
	connect(&SignalDispatcher::getInstance(), SIGNAL(sig_ProcTerminated(int, int)), process.get(),
			SLOT(slot_ProcTerminated(int, int)));

	// Connections between the OSServices and the ObservedProcess
	connect(service.get(), SIGNAL(sig_TaskCreated(int)), process.get(), SLOT(slot_TaskCreated(int)));
	connect(service.get(), SIGNAL(sig_TaskTerminated(int)), process.get(), SLOT(slot_TaskTerminated(int)));
	connect(service.get(), SIGNAL(sig_CommChannel(autopin_msg)), process.get(), SLOT(slot_CommChannel(autopin_msg)));

	// Connections between the ObservedProcess and the ControlStrategy
	connect(process.get(), SIGNAL(sig_TaskCreated(int)), strategy.get(), SLOT(slot_TaskCreated(int)));
	connect(process.get(), SIGNAL(sig_TaskTerminated(int)), strategy.get(), SLOT(slot_TaskTerminated(int)));
	connect(process.get(), SIGNAL(sig_UserMessage(int, double)), strategy.get(), SLOT(slot_UserMessage(int, double)));

	// Connections between the ObservedProcess and this object
	connect(process.get(), SIGNAL(sig_ProcTerminated()), this, SIGNAL(sig_watchdogStop()));

	// Connection between ControlStrategy and this object
	connect(this, SIGNAL(sig_watchdogReady()), strategy.get(), SLOT(slot_watchdogReady()));
}

} // namespace AutopinPlus
