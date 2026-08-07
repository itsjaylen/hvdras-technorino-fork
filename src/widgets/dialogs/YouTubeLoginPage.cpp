// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/YouTubeLoginPage.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "singletons/Theme.hpp"
#include "util/HttpServer.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpressionValidator>
#include <QSpacerItem>
#include <QString>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <cstring>
#include <utility>

using namespace Qt::Literals;

namespace {

using namespace chatterino;

// Must match a redirect URI registered on the Google Cloud OAuth client.
const QString REDIRECT_URL = u"http://127.0.0.1:38290"_s;
constexpr uint16_t SERVER_PORT = 38290;

constexpr const char *YOUTUBE_SCOPE =
    "https://www.googleapis.com/auth/youtube.force-ssl";

QByteArray generateRandomBytes(qsizetype size)
{
    assert((size % 4) == 0);
    QByteArray bytes;
    bytes.resize(size);
    auto *gen = QRandomGenerator::system();
    for (qsizetype i = 0; i < bytes.size() / 4; i++)
    {
        quint32 v = gen->generate();
        std::memcpy(bytes.data() + (i * 4), &v, 4);
    }
    return bytes;
}

QString formatAPIError(const NetworkResult &result)
{
    const auto json = result.parseJson();
    auto error = json["error_description"_L1].toString(
        json["error"_L1].toString());
    if (!error.isEmpty())
    {
        return u"Error: " % error % u" (" % result.formatError() % ')';
    }
    return u"Error: " % result.formatError() % u" (no further information)";
}

struct AuthParams {
    QByteArray codeVerifier;
    QByteArray codeChallenge;
    QByteArray state;
};

AuthParams startAuthSession()
{
    auto base64Opts =
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
    auto codeVerifier = generateRandomBytes(64).toBase64(base64Opts);

    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(codeVerifier);
    auto codeChallenge = h.result().toBase64(base64Opts);

    return {
        .codeVerifier = codeVerifier,
        .codeChallenge = codeChallenge,
        .state = generateRandomBytes(32).toBase64(base64Opts),
    };
}

class AuthDialog : public QDialog
{
public:
    AuthDialog(QString clientID, QString clientSecret,
               QWidget *parent = nullptr)
        : QDialog(parent)
        , clientID(std::move(clientID))
        , clientSecret(std::move(clientSecret))
        , authParams(startAuthSession())
        , statusLabel("Waiting...")
    {
        this->setAttribute(Qt::WA_DeleteOnClose);
        this->setWindowTitle("Waiting...");

        QUrlQuery query{
            {"response_type", "code"},
            {"client_id", this->clientID},
            {"redirect_uri", REDIRECT_URL},
            {"scope", QString::fromLatin1(YOUTUBE_SCOPE)},
            {"code_challenge", this->authParams.codeChallenge},
            {"code_challenge_method", "S256"},
            {"state", this->authParams.state},
            // Required to get a refresh_token back at all, and to force the
            // consent screen (and therefore a fresh refresh_token) even if
            // this app was already authorized before.
            {"access_type", "offline"},
            {"prompt", "consent"},
        };
        this->authURL = u"https://accounts.google.com/o/oauth2/v2/auth?" %
                        query.toString(QUrl::FullyEncoded);

        auto *srv = new HttpServer(SERVER_PORT, this);
        srv->setHandler([this](const QString &path) {
            return this->handleRequest(path);
        });

        auto *root = new QVBoxLayout(this);
        root->addWidget(&this->statusLabel, 1, Qt::AlignCenter);
        root->addWidget(new QLabel("This window will close automatically."), 1,
                        Qt::AlignCenter);

        auto *urlButtons = new QWidget;
        auto *urlButtonLayout = new QHBoxLayout(urlButtons);

        auto *openUrl = new QPushButton(u"Log in (Opens in browser)"_s);
        QObject::connect(openUrl, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(this->authURL);
        });
        urlButtonLayout->addWidget(openUrl, 1);

        auto *copyUrl = new QPushButton(u"Copy URL"_s);
        QObject::connect(copyUrl, &QPushButton::clicked, this, [this] {
            qApp->clipboard()->setText(
                this->authURL.toString(QUrl::FullyEncoded));
        });
        urlButtonLayout->addWidget(copyUrl, 1);

        root->addWidget(urlButtons, 1);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        root->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);
    }

    std::pair<unsigned, QByteArray> handleRequest(const QString &path)
    {
        auto queryIdx = path.indexOf('?');
        if (queryIdx < 0)
        {
            return {404, "No query"_ba};
        }
        auto queryStr = path.mid(queryIdx + 1);
        QUrlQuery query(queryStr);
        if (query.hasQueryItem("done"))
        {
            return {200, "You can close this tab now."_ba};
        }

        if (query.hasQueryItem("error"))
        {
            this->statusLabel.setText(u"Error: " %
                                      query.queryItemValue("error"));
            return {400, "Authorization was denied"_ba};
        }

        if (!query.hasQueryItem("code"))
        {
            return {400, "No code"_ba};
        }
        if (query.queryItemValue("state") != this->authParams.state)
        {
            return {400, "State mismatch!"_ba};
        }

        this->requestToken(query.queryItemValue("code"));

        return {
            200,
            "<!DOCTYPE html><html><head></head><body><script>location.search='?done=1'</script></body></html>"_ba,
        };
    }

private:
    void requestToken(const QString &code)
    {
        QUrlQuery payload{
            {"grant_type", "authorization_code"},
            {"client_id", this->clientID},
            {"client_secret", this->clientSecret},
            {"redirect_uri", REDIRECT_URL},
            {"code_verifier", this->authParams.codeVerifier},
            {"code", code},
        };
        NetworkRequest(u"https://oauth2.googleapis.com/token"_s,
                       NetworkRequestType::Post)
            .header("Content-Type", "application/x-www-form-urlencoded")
            .payload(payload.toString(QUrl::FullyEncoded).toUtf8())
            .hideRequestBody()
            .caller(this)
            .onError([this](const NetworkResult &result) {
                auto error = formatAPIError(result);
                qCWarning(chatterinoYoutube)
                    << "Getting token failed" << error;
                this->statusLabel.setText(error);
            })
            .onSuccess([this](const NetworkResult &result) {
                this->getAuthenticatedUser(result.parseJson());
            })
            .execute();
    }

    void getAuthenticatedUser(const QJsonObject &tokenData)
    {
        auto expiresIn = tokenData["expires_in"_L1].toInteger();
        auto expiresAt = QDateTime::currentDateTimeUtc().addSecs(expiresIn);
        auto accessToken = tokenData["access_token"_L1].toString();
        auto refreshToken = tokenData["refresh_token"_L1].toString();

        if (refreshToken.isEmpty())
        {
            this->statusLabel.setText(
                u"Error: Google did not return a refresh token. Try "
                u"revoking this app's access at "
                u"myaccount.google.com/permissions and logging in again."_s);
            return;
        }

        NetworkRequest(
            u"https://www.googleapis.com/youtube/v3/channels?part=snippet&mine=true"_s)
            .header("Authorization", u"Bearer " % accessToken)
            .caller(this)
            .onError([this](const NetworkResult &result) {
                auto error = formatAPIError(result);
                qCWarning(chatterinoYoutube)
                    << "Getting channel failed" << error;
                this->statusLabel.setText(error);
            })
            .onSuccess([this, accessToken, refreshToken,
                        expiresAt](const NetworkResult &result) {
                const auto items = result.parseJson()["items"_L1].toArray();
                if (items.isEmpty())
                {
                    this->statusLabel.setText(
                        u"Error: This Google account has no YouTube channel."_s);
                    return;
                }
                const auto channel = items.at(0).toObject();
                const auto snippet = channel["snippet"_L1].toObject();
                auto handle = snippet["customUrl"_L1].toString();
                if (handle.startsWith(u'@'))
                {
                    handle.remove(0, 1);
                }
                YouTubeAccountData data{
                    .channelName = snippet["title"_L1].toString(),
                    .handle = handle,
                    .channelId = channel["id"_L1].toString(),
                    .clientID = this->clientID,
                    .clientSecret = this->clientSecret,
                    .authToken = accessToken,
                    .refreshToken = refreshToken,
                    .expiresAt = expiresAt,
                };

                data.save();
                getApp()->getAccounts()->youtube.reloadUsers();
                getApp()->getAccounts()->youtube.currentChannelId =
                    data.channelId;
                this->accept();
                this->close();
            })
            .execute();
    }

    QString clientID;
    QString clientSecret;
    AuthParams authParams;
    QUrl authURL;

    QLabel statusLabel;
};

}  // namespace

namespace chatterino {

YouTubeLoginPage::YouTubeLoginPage()
{
    static const QRegularExpression nonEmptyRe{u".+"_s};

    auto *root = new QFormLayout(this);
    this->ui.layout = root;

    this->ui.topLabel = new QLabel();
    this->ui.topLabel->setWordWrap(true);
    this->ui.topLabel->setOpenExternalLinks(true);
    this->ui.topLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    this->ui.topLabel->setText(
        u"YouTube moderation requires your own Google Cloud OAuth "
        u"credentials - there's no way for this app to ship a shared one "
        u"safely. In <a href=\"https://console.cloud.google.com/\">Google "
        u"Cloud Console</a>: create a project, enable the <b>YouTube Data "
        u"API v3</b>, then create an OAuth 2.0 Client ID of type <b>Desktop "
        u"app</b> and paste its Client ID and secret below. While the "
        u"consent screen is in \"Testing\" mode, add your Google account as "
        u"a test user under the consent screen's Audience tab. If your "
        u"channel is a separate Brand Account, pick it (not your personal "
        u"account) at the sign-in screen so access is granted for the "
        u"right channel."_s);
    root->addRow(this->ui.topLabel);
    root->addItem(
        new QSpacerItem(0, 10, QSizePolicy::Minimum, QSizePolicy::Fixed));

    this->ui.clientID = new QLineEdit;
    this->ui.clientID->setPlaceholderText(
        "123456789-abc.apps.googleusercontent.com");
    this->ui.clientID->setValidator(
        new QRegularExpressionValidator(nonEmptyRe, this));
    root->addRow("Client ID:", this->ui.clientID);

    this->ui.clientSecret = new QLineEdit;
    this->ui.clientSecret->setPlaceholderText("GOCSPX-...");
    this->ui.clientSecret->setEchoMode(QLineEdit::Password);
    this->ui.clientSecret->setValidator(
        new QRegularExpressionValidator(nonEmptyRe, this));
    root->addRow("Client Secret:", this->ui.clientSecret);

    auto currentAccount = getApp()->getAccounts()->youtube.current();
    if (!currentAccount->isAnonymous())
    {
        this->ui.clientID->setText(currentAccount->clientID());
        this->ui.clientSecret->setText(currentAccount->clientSecret());
    }

    root->addItem(
        new QSpacerItem(0, 10, QSizePolicy::Minimum, QSizePolicy::Fixed));

    auto *startButton = new QPushButton("Start");
    root->addRow(startButton);
    QObject::connect(startButton, &QPushButton::clicked, this, [this] {
        if (!this->ui.clientID->hasAcceptableInput() ||
            !this->ui.clientSecret->hasAcceptableInput())
        {
            return;
        }
        auto *diag = new AuthDialog(this->ui.clientID->text(),
                                    this->ui.clientSecret->text(), this);
        QObject::connect(diag, &QDialog::accepted, this, [this] {
            this->window()->close();
        });
        diag->show();
    });
}

void YouTubeLoginPage::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setBrush(getTheme()->window.background);
    painter.setPen({});
    painter.drawRect(this->rect());
}

}  // namespace chatterino
