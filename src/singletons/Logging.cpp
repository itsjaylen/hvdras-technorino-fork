// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "singletons/Logging.hpp"

#include "messages/Message.hpp"
#include "singletons/Settings.hpp"
#include "singletons/helper/LoggingChannel.hpp"

#include <QDateTime>  // Added
#include <QDir>
#include <QFile>      // Added
#include <QStandardPaths>
#include <QTextStream> // Added

#include <memory>
#include <utility>

namespace chatterino {

Logging::Logging(Settings &settings)
{
    std::ignore = settings.loggedChannels.delayedItemsChanged.connect(
        [this, &settings]() {
            this->threadGuard.guard();

            this->onlyLogListedChannels.clear();

            for (const auto &loggedChannel :
                 *settings.loggedChannels.readOnly())
            {
                this->onlyLogListedChannels.insert(loggedChannel.channelName());
            }
        });
}

void Logging::addMessage(const QString &channelName, MessagePtr message,
                         const QString &platformName, const QString &streamID)
{
    if (platformName.isEmpty())
    {
        return;
    }

    this->threadGuard.guard();

    if (!getSettings()->enableLogging)
    {
        return;
    }

    if (getSettings()->onlyLogListedChannels)
    {
        if (!this->onlyLogListedChannels.contains(channelName))
        {
            return;
        }
    }

    auto platIt = this->loggingChannels_.find(platformName);
    if (platIt == this->loggingChannels_.end())
    {
        auto *channel = new LoggingChannel(channelName, platformName);
        channel->addMessage(message, streamID);
        auto map = std::map<QString, std::unique_ptr<LoggingChannel>>();
        this->loggingChannels_[platformName] = std::move(map);
        auto &ref = this->loggingChannels_.at(platformName);
        ref.emplace(channelName, channel);
        return;
    }
    auto chanIt = platIt->second.find(channelName);
    if (chanIt == platIt->second.end())
    {
        auto *channel = new LoggingChannel(channelName, platformName);
        channel->addMessage(message, streamID);
        platIt->second.emplace(channelName, channel);
    }
    else
    {
        chanIt->second->addMessage(message, streamID);
    }
}

void Logging::closeChannel(const QString &channelName,
                            const QString &platformName)
{
    if (platformName.isEmpty())
    {
        return;
    }

    auto platIt = this->loggingChannels_.find(platformName);
    if (platIt == this->loggingChannels_.end())
    {
        return;
    }
    platIt->second.erase(channelName);
}

// Keep inside the chatterino namespace!
void Logging::logModerationEvent(const QString &channelName, const QString &text)
{
    QString base = getSettings()->logPath.getValue();
    if (base.isEmpty())
    {
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }

    QString dirPath = base + "/ModLogs";
    QDir().mkpath(dirPath);

    QString filePath = dirPath + "/" + channelName + ".log";
    QFile file(filePath);

    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
        out << "[" << timestamp << "] " << text << "\n";
    }
}

}  // namespace chatterino
