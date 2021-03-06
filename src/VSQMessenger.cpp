//  Copyright (C) 2015-2020 Virgil Security, Inc.
//
//  All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are
//  met:
//
//      (1) Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//
//      (2) Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in
//      the documentation and/or other materials provided with the
//      distribution.
//
//      (3) Neither the name of the copyright holder nor the names of its
//      contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE AUTHOR ''AS IS'' AND ANY EXPRESS OR
//  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
//  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
//  DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
//  INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
//  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
//  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
//  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
//  IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//  POSSIBILITY OF SUCH DAMAGE.
//
//  Lead Maintainer: Virgil Security Inc. <support@virgilsecurity.com>

#include <virgil/iot/qt/VSQIoTKit.h>

#include <VSQMessenger.h>

#include <qxmpp/QXmppMessage.h>
#include <qxmpp/QXmppUtils.h>

#include <QtConcurrent>
#include <QStandardPaths>
#include <QSqlDatabase>
#include <QSqlError>
#include <QtQml>

#include <QDesktopServices>

const QString VSQMessenger::kOrganization = "VirgilSecurity";
const QString VSQMessenger::kApp = "IoTKit Messenger";
const QString VSQMessenger::kUsers = "Users";
const QString VSQMessenger::kProdEnvPrefix = "prod";
const QString VSQMessenger::kStgEnvPrefix = "stg";
const QString VSQMessenger::kDevEnvPrefix = "dev";

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#if defined(VERSION)
const QString VSQMessenger::kVersion = QString(TOSTRING(VERSION));
#else
const QString VSQMessenger::kVersion = "unknown";
#endif

/******************************************************************************/
VSQMessenger::VSQMessenger() {
    _connectToDatabase();

    m_sqlContacts = new VSQSqlContactModel(this);
    m_sqlConversations = new VSQSqlConversationModel(this);

    // Signal connection
    connect(this, SIGNAL(fireReadyToAddContact(QString)), this, SLOT(onAddContactToDB(QString)));
    connect(this, SIGNAL(fireError(QString)), this, SLOT(logout()));

    // Connect XMPP signals
    connect(&m_xmpp, SIGNAL(connected()), this, SLOT(onConnected()));
    connect(&m_xmpp, SIGNAL(disconnected()), this, SLOT(onDisconnected()));
    connect(&m_xmpp, SIGNAL(error(QXmppClient::Error)), this, SLOT(onError(QXmppClient::Error)));
    connect(&m_xmpp, SIGNAL(messageReceived(const QXmppMessage &)), this, SLOT(onMessageReceived(const QXmppMessage &)));
    connect(&m_xmpp, SIGNAL(presenceReceived(const QXmppPresence &)), this, SLOT(onPresenceReceived(const QXmppPresence &)));
    connect(&m_xmpp, SIGNAL(iqReceived(const QXmppIq &)), this, SLOT(onIqReceived(const QXmppIq &)));
    connect(&m_xmpp, SIGNAL(sslErrors(const QList<QSslError> &)), this, SLOT(onSslErrors(const QList<QSslError> &)));
    connect(&m_xmpp, SIGNAL(stateChanged(QXmppClient::State)), this, SLOT(onStateChanged(QXmppClient::State)));
}

/******************************************************************************/
void
VSQMessenger::_connectToDatabase() {
    QSqlDatabase database = QSqlDatabase::database();
    if (!database.isValid()) {
        database = QSqlDatabase::addDatabase("QSQLITE");
        if (!database.isValid())
            qFatal("Cannot add database: %s", qPrintable(database.lastError().text()));
    }

    const QDir writeDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!writeDir.mkpath("."))
        qFatal("Failed to create writable directory at %s", qPrintable(writeDir.absolutePath()));

    // Ensure that we have a writable location on all devices.
    const QString fileName = writeDir.absolutePath() + "/chat-database.sqlite3";
    // When using the SQLite driver, open() will create the SQLite database if it doesn't exist.
    database.setDatabaseName(fileName);
    if (!database.open()) {
        QFile::remove(fileName);
        qFatal("Cannot open database: %s", qPrintable(database.lastError().text()));
    }
}

/******************************************************************************/
Q_DECLARE_METATYPE(QXmppConfiguration)
void
VSQMessenger::_connect(QString userWithEnv, QString userId) {
    const size_t _pass_buf_sz = 512;
    char pass[_pass_buf_sz];
    QString jid = userId + "@" + _xmppURL();

    // Update users list
    _addToUsersList(userWithEnv);

    // Get XMPP password
    if (VS_CODE_OK != vs_messenger_virgil_get_xmpp_pass(pass, _pass_buf_sz)) {
        emit fireError(tr("Cannot get XMPP password"));
        return;
    }

    // Connect to XMPP
    emit fireConnecting();

    conf.setJid(jid);
    conf.setHost(_xmppURL());
    conf.setPassword(QString::fromLatin1(pass));

    qDebug() << "SSL: " << QSslSocket::supportsSsl();

    auto logger = QXmppLogger::getLogger();
    logger->setLoggingType(QXmppLogger::SignalLogging);
    logger->setMessageTypes(QXmppLogger::AnyMessage);

    connect(logger, &QXmppLogger::message, [=](QXmppLogger::MessageType, const QString &text){
        qDebug() << text;
    });

    m_xmpp.setLogger(logger);
    qRegisterMetaType<QXmppConfiguration>("QXmppConfiguration");
    QMetaObject::invokeMethod(&m_xmpp, "connectToServer", Qt::QueuedConnection, Q_ARG(QXmppConfiguration, conf));
}

/******************************************************************************/
QString
VSQMessenger::_prepareLogin(const QString &user) {
    QString userId = user;
    m_envType = _defaultEnv;

    // Check required environment
    QStringList pieces = user.split("@");
    if (2 == pieces.size()) {
        userId = pieces.at(1);
        if (kProdEnvPrefix == pieces.first()) {
            m_envType = PROD;
        } else if (kStgEnvPrefix == pieces.first()) {
            m_envType = STG;
        }
        else if (kDevEnvPrefix == pieces.first()) {
            m_envType = DEV;
        }
    }

    // Initialize Virgil Messenger to work with required environment
    const char *cCABundle = NULL;
    QString caPath = qgetenv("VS_CURL_CA_BUNDLE");
    if (!caPath.isEmpty()) {
        cCABundle = caPath.toStdString().c_str();
    }

    if (VS_CODE_OK != vs_messenger_virgil_init(_virgilURL().toStdString().c_str(), cCABundle)) {
        qCritical() << "Cannot initialize low level messenger";
    }

    // Set current user
    m_user = userId;
    m_sqlContacts->setUser(userId);
    m_sqlConversations->setUser(userId);

    // Inform about user activation
    emit fireCurrentUserChanged();

    return userId;
}

/******************************************************************************/
QString
VSQMessenger::currentUser() {
    return m_user;
}

/******************************************************************************/
QString
VSQMessenger::currentVersion() const {
    return kVersion + "-alpha";
}

/******************************************************************************/
void
VSQMessenger::signIn(QString user) {
    auto userId = _prepareLogin(user);
    QtConcurrent::run([=]() {
        qDebug() << "Trying to Sign In: " << userId;

        vs_messenger_virgil_user_creds_t creds;
        memset(&creds, 0, sizeof (creds));

        // Load User Credentials
        if (!_loadCredentials(userId, creds)) {
            emit fireError(tr("Cannot load user credentials"));
            return;
        }

        // Sign In user, using Virgil Service
        if (VS_CODE_OK != vs_messenger_virgil_sign_in(&creds)) {
            emit fireError(tr("Cannot Sign In user"));
            return;
        }

        // Connect over XMPP
        _connect(user, userId);
    });
}

/******************************************************************************/
void
VSQMessenger::signUp(QString user) {
    auto userId = _prepareLogin(user);
    QtConcurrent::run([=]() {
        qDebug() << "Trying to Sign Up: " << userId;

        vs_messenger_virgil_user_creds_t creds;
        memset(&creds, 0, sizeof (creds));

        // Sign Up user, using Virgil Service
        if (VS_CODE_OK != vs_messenger_virgil_sign_up(userId.toStdString().c_str(), &creds)) {
            emit fireError(tr("Cannot Sign Up user"));
            return;
        }

        // Save credentials
        _saveCredentials(userId, creds);

        // Connect over XMPP
        _connect(user, userId);
    });
}

/******************************************************************************/
void
VSQMessenger::addContact(QString contact) {
    QtConcurrent::run([=]() {
        // Sign Up user, using Virgil Service
        if (VS_CODE_OK != vs_messenger_virgil_search(contact.toStdString().c_str())) {
            auto errorText = tr("User is not registered : ") + contact;
            emit fireInform(errorText);
            return;
        }

        emit fireReadyToAddContact(contact);
    });
}

/******************************************************************************/
void
VSQMessenger::onAddContactToDB(QString contact) {
    m_sqlContacts->addContact(contact);
    emit fireAddedContact(contact);
}

/******************************************************************************/
QString
VSQMessenger::_virgilURL() {
    QString res = qgetenv("VS_MSGR_VIRGIL");

    if (res.isEmpty()) {
        switch (m_envType) {
        case PROD:
            res = "https://messenger.virgilsecurity.com";
            break;

        case STG:
            res = "https://messenger-stg.virgilsecurity.com";
            break;

        case DEV:
            res = "https://messenger-dev.virgilsecurity.com";
            break;
        }
    }

    VS_LOG_DEBUG("Virgil URL: %s", res.toStdString().c_str());
    return res;
}

/******************************************************************************/
QString
VSQMessenger::_xmppURL() {
    QString res = qgetenv("VS_MSGR_XMPP_URL");

    if (res.isEmpty()) {
        switch (m_envType) {
        case PROD:
            res = "xmpp.virgilsecurity.com";
            break;

        case STG:
            res = "xmpp-stg.virgilsecurity.com";
            break;

        case DEV:
            res = "xmpp-dev.virgilsecurity.com";
            break;
        }
    }

    VS_LOG_DEBUG("XMPP URL: %s", res.toStdString().c_str());
    return res;
}

/******************************************************************************/
uint16_t
VSQMessenger::_xmppPort() {
    uint16_t res = 5222;
    QString portStr = qgetenv("VS_MSGR_XMPP_PORT");

    if (!portStr.isEmpty()) {
        bool ok;
        int port = portStr.toInt(&ok);
        if (ok) {
            res = static_cast<uint16_t> (port);
        }
    }

    VS_LOG_DEBUG("XMPP PORT: %d", static_cast<int> (res));

    return res;
}

/******************************************************************************/

void
VSQMessenger::_addToUsersList(const QString &user) {
    // Save known users
    auto knownUsers = usersList();
    knownUsers.removeAll(user);
    knownUsers.push_front(user);
    _saveUsersList(knownUsers);
}

/******************************************************************************/
// TODO: Use SecBox
bool
VSQMessenger::_saveCredentials(const QString &user, const vs_messenger_virgil_user_creds_t &creds) {
    // Save credentials
    QByteArray baCred(reinterpret_cast<const char*>(&creds), sizeof(creds));
    QSettings settings(kOrganization, kApp);
    settings.setValue(user, baCred.toBase64());
    return true;
}

/******************************************************************************/
bool
VSQMessenger::_loadCredentials(const QString &user, vs_messenger_virgil_user_creds_t &creds) {
    QSettings settings(kOrganization, kApp);

    auto credBase64 = settings.value(user, QString("")).toString();
    auto baCred = QByteArray::fromBase64(credBase64.toUtf8());

    if (baCred.size() != sizeof(creds)) {
        VS_LOG_WARNING("Cannot load credentials for : %s", user.toStdString().c_str());
        return false;
    }

    memcpy(&creds, baCred.data(), static_cast<size_t> (baCred.size()));
    return true;
}

/******************************************************************************/
void
VSQMessenger::_saveUsersList(const QStringList &users) {
    QSettings settings(kOrganization, kApp);
    settings.setValue(kUsers, users);
}

/******************************************************************************/
QStringList
VSQMessenger::usersList() {
    QSettings settings(kOrganization, kApp);
    return settings.value(kUsers, QStringList()).toStringList();
}

/******************************************************************************/
void
VSQMessenger::logout() {
    qDebug() << "Logout";
    m_xmpp.disconnectFromServer();
    vs_messenger_virgil_logout();
}

/******************************************************************************/
void
VSQMessenger::deleteUser(QString user) {
    Q_UNUSED(user)
    logout();
}

/******************************************************************************/
VSQSqlContactModel &
VSQMessenger::modelContacts() {
    return *m_sqlContacts;
}

/******************************************************************************/
VSQSqlConversationModel &
VSQMessenger::modelConversations() {
    return *m_sqlConversations;
}

/******************************************************************************/
void
VSQMessenger::onConnected() {
    emit fireReady();
}

/******************************************************************************/
void
VSQMessenger::sendReport() {
    QDesktopServices::openUrl(QUrl("mailto:?to=kutashenko@gmail.com&subject=Virgil Messenger Report&body=Here is some email body text", QUrl::TolerantMode));
}

/******************************************************************************/
void
VSQMessenger::onDisconnected() {
    VS_LOG_DEBUG("onDisconnected");
#if 0
    emit fireError(tr("Disconnected ..."));
#endif
}

/******************************************************************************/
void
VSQMessenger::onError(QXmppClient::Error err) {
    VS_LOG_DEBUG("onError");
    qDebug() << err;
    emit fireError(tr("Connection error ..."));
}

/******************************************************************************/
void
VSQMessenger::onMessageReceived(const QXmppMessage &message) {

    // TODO: Do we need a separate thread here ?

    static const size_t _decryptedMsgSzMax = 10 * 1024;
    uint8_t decryptedMessage[_decryptedMsgSzMax];
    size_t decryptedMessageSz = 0;

    // Get sender
    QString from = message.from();
    QStringList pieces = from.split("@");
    if (pieces.size() < 1) {
        VS_LOG_WARNING("Wrong sender");
        return;
    }
    QString sender = pieces.first();

    // Get encrypted message
    QString msg = message.body();

    // Decrypt message
    // DECRYPTED_MESSAGE_SZ_MAX - 1  - This is required for a Zero-terminated string
    if (VS_CODE_OK !=
            vs_messenger_virgil_decrypt_msg(
                sender.toStdString().c_str(),
                msg.toStdString().c_str(),
                decryptedMessage, decryptedMessageSz - 1,
                &decryptedMessageSz)) {
        VS_LOG_WARNING("Received message cannot be decrypted");
        return;
    }

    // Add Zero termination
    decryptedMessage[decryptedMessageSz] = 0;

    // Get message from JSON
    QByteArray baDecr(reinterpret_cast<char *> (decryptedMessage), static_cast<int> (decryptedMessageSz));
    QJsonDocument jsonMsg(QJsonDocument::fromJson(baDecr));
    QString decryptedString = jsonMsg["payload"]["body"].toString();

    VS_LOG_DEBUG("Received message: <%s>", decryptedString.toStdString().c_str());

    // Add sender to contacts
    m_sqlContacts->addContact(sender);

    // Save message to DB
    m_sqlConversations->receiveMessage(sender, decryptedString);

    // Inform system about new message
    emit fireNewMessage(sender, decryptedString);
}

/******************************************************************************/
void
VSQMessenger::sendMessage(QString to, QString message) {
    static const size_t _encryptedMsgSzMax = 20 * 1024;
    uint8_t encryptedMessage[_encryptedMsgSzMax];
    size_t encryptedMessageSz = 0;

    // Save message to DB
    m_sqlConversations->sendMessage(to, message);

    // Create JSON-formatted message to be sent
    QJsonObject payloadObject;
    payloadObject.insert("body", message);

    QJsonObject mainObject;
    mainObject.insert("type", "text");
    mainObject.insert("payload", payloadObject);

    QJsonDocument doc(mainObject);
    QString internalJson = doc.toJson(QJsonDocument::Compact);

    qDebug() << internalJson;

    // Encrypt message
    if (VS_CODE_OK != vs_messenger_virgil_encrypt_msg(
                     to.toStdString().c_str(),
                     internalJson.toStdString().c_str(),
                     encryptedMessage,
                     _encryptedMsgSzMax,
                     &encryptedMessageSz)) {
        VS_LOG_WARNING("Cannot encrypt message to be sent");
        return;
    }

    // Send encrypted message
    QString toJID = to + "@" + _xmppURL();
    QString encryptedStr = QString::fromLatin1(reinterpret_cast<char*>(encryptedMessage));
    m_xmpp.sendMessage(toJID, encryptedStr);
}

/******************************************************************************/
void
VSQMessenger::onPresenceReceived(const QXmppPresence &presence) {
    Q_UNUSED(presence)
}

/******************************************************************************/
void
VSQMessenger::onIqReceived(const QXmppIq &iq) {
    Q_UNUSED(iq)
}

/******************************************************************************/
void
VSQMessenger::onSslErrors(const QList<QSslError> &errors) {
    Q_UNUSED(errors)
    emit fireError(tr("Secure connection error ..."));
}

/******************************************************************************/
void
VSQMessenger::onStateChanged(QXmppClient::State state) {
    if (QXmppClient::ConnectingState == state) {
        emit fireConnecting();
    }
}

/******************************************************************************/
