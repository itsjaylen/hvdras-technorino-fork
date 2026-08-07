// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeAccount.hpp"

#include "common/ChatterinoSetting.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "singletons/Settings.hpp"

#include <pajlada/settings/setting.hpp>
#include <QUrlQuery>

#include <algorithm>
#include <limits>

namespace chatterino {

using namespace Qt::Literals;

std::optional<YouTubeAccountData> YouTubeAccountData::loadRaw(
    const std::string &key)
{
    auto channelName =
        QStringSetting::get("/youtubeAccounts/" + key + "/channelName");
    auto channelId =
        QStringSetting::get("/youtubeAccounts/" + key + "/channelId");
    auto clientID =
        QStringSetting::get("/youtubeAccounts/" + key + "/clientID");
    auto clientSecret =
        QStringSetting::get("/youtubeAccounts/" + key + "/clientSecret");
    auto authToken =
        QStringSetting::get("/youtubeAccounts/" + key + "/authToken");
    auto refreshToken =
        QStringSetting::get("/youtubeAccounts/" + key + "/refreshToken");
    auto expiresAtStr =
        QStringSetting::get("/youtubeAccounts/" + key + "/expiresAt");

    if (channelId.isEmpty() || clientID.isEmpty() ||
        clientSecret.isEmpty() || authToken.isEmpty() ||
        refreshToken.isEmpty() || expiresAtStr.isEmpty())
    {
        return std::nullopt;
    }

    QDateTime expiresAt = QDateTime::fromString(expiresAtStr, Qt::ISODate);

    return YouTubeAccountData{
        .channelName = channelName.trimmed(),
        .channelId = channelId.trimmed(),
        .clientID = clientID.trimmed(),
        .clientSecret = clientSecret.trimmed(),
        .authToken = authToken.trimmed(),
        .refreshToken = refreshToken.trimmed(),
        .expiresAt = expiresAt,
    };
}

void YouTubeAccountData::save() const
{
    auto basePath = "/youtubeAccounts/uid" + this->channelId.toStdString();
    QStringSetting::set(basePath + "/channelName", this->channelName);
    QStringSetting::set(basePath + "/channelId", this->channelId);
    QStringSetting::set(basePath + "/clientID", this->clientID);
    QStringSetting::set(basePath + "/clientSecret", this->clientSecret);
    QStringSetting::set(basePath + "/authToken", this->authToken);
    QStringSetting::set(basePath + "/refreshToken", this->refreshToken);
    QStringSetting::set(basePath + "/expiresAt",
                        this->expiresAt.toString(Qt::ISODate));
    std::ignore = getSettings()->requestSave();
}

YouTubeAccount::YouTubeAccount(const YouTubeAccountData &args)
    : Account(ProviderId::YouTube)
    , channelName_(args.channelName)
    , channelId_(args.channelId)
    , clientID_(args.clientID)
    , clientSecret_(args.clientSecret)
    , authToken_(args.authToken)
    , refreshToken_(args.refreshToken)
    , expiresAt_(args.expiresAt)
{
}

YouTubeAccount::~YouTubeAccount() = default;

void YouTubeAccount::save() const
{
    YouTubeAccountData{
        .channelName = this->channelName_,
        .channelId = this->channelId_,
        .clientID = this->clientID_,
        .clientSecret = this->clientSecret_,
        .authToken = this->authToken_,
        .refreshToken = this->refreshToken_,
        .expiresAt = this->expiresAt_,
    }
        .save();
}

bool YouTubeAccount::update(const YouTubeAccountData &data)
{
    bool changed = false;

    if (this->channelName_ != data.channelName)
    {
        changed = true;
        this->channelName_ = data.channelName;
    }
    if (this->channelId_ != data.channelId)
    {
        changed = true;
        this->channelId_ = data.channelId;
    }
    if (this->clientID_ != data.clientID)
    {
        changed = true;
        this->clientID_ = data.clientID;
    }
    if (this->clientSecret_ != data.clientSecret)
    {
        changed = true;
        this->clientSecret_ = data.clientSecret;
    }
    if (this->authToken_ != data.authToken)
    {
        changed = true;
        this->authToken_ = data.authToken;
    }
    if (this->refreshToken_ != data.refreshToken)
    {
        changed = true;
        this->refreshToken_ = data.refreshToken;
    }
    if (this->expiresAt_ != data.expiresAt)
    {
        changed = true;
        this->expiresAt_ = data.expiresAt;
    }

    if (changed)
    {
        this->save();
    }
    return changed;
}

QString YouTubeAccount::toString() const
{
    return this->channelName_;
}

void YouTubeAccount::refreshIfNeeded()
{
    if (this->isAnonymous())
    {
        return;
    }

    auto now = QDateTime::currentDateTimeUtc() + CHECK_REFRESH_INTERVAL +
               std::chrono::seconds{60};
    if (now < this->expiresAt_)
    {
        return;
    }

    qCDebug(chatterinoYoutube) << "Attempting to refresh" << this->channelName()
                               << "expires:" << this->expiresAt_;
    this->doRefresh();
}

void YouTubeAccount::doRefresh()
{
    QUrlQuery payload{
        {"refresh_token"_L1, this->refreshToken_},
        {"client_id"_L1, this->clientID_},
        {"client_secret"_L1, this->clientSecret_},
        {"grant_type"_L1, "refresh_token"_L1},
    };

    auto weak = this->weak_from_this();
    NetworkRequest(u"https://oauth2.googleapis.com/token"_s,
                   NetworkRequestType::Post)
        .header("Content-Type", "application/x-www-form-urlencoded")
        .hideRequestBody()
        .payload(payload.toString(QUrl::FullyEncoded).toUtf8())
        .timeout(20'000)
        .onSuccess([weak](const NetworkResult &res) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }

            const auto json = res.parseJson();
            self->authToken_ = json["access_token"_L1].toString();
            // Google doesn't reliably return a new refresh_token on refresh -
            // only overwrite it if one was actually sent back.
            const auto newRefreshToken = json["refresh_token"_L1].toString();
            if (!newRefreshToken.isEmpty())
            {
                self->refreshToken_ = newRefreshToken;
            }
            auto expiresInSec =
                std::clamp<qint64>(json["expires_in"_L1].toInteger(), 0,
                                   std::numeric_limits<qint32>::max());
            self->expiresAt_ =
                QDateTime::currentDateTimeUtc().addSecs(expiresInSec);
            self->save();
            self->authUpdated.invoke();
            qCDebug(chatterinoYoutube)
                << "[Refresh] Successful, next expiry:" << self->expiresAt_;
        })
        .onError([weak](const NetworkResult &res) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            qCWarning(chatterinoYoutube)
                << "Failed to refresh" << self->channelName()
                << "error:" << res.formatError();
        })
        .execute();
}

}  // namespace chatterino
