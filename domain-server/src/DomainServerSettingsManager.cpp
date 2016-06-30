//
//  DomainServerSettingsManager.cpp
//  domain-server/src
//
//  Created by Stephen Birarda on 2014-06-24.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <algorithm>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <AccountManager.h>
#include <QTimeZone>

#include <Assignment.h>
#include <HifiConfigVariantMap.h>
#include <HTTPConnection.h>
#include <NLPacketList.h>
#include <NumericalConstants.h>

#include "DomainServerSettingsManager.h"

#define WANT_DEBUG 1


const QString SETTINGS_DESCRIPTION_RELATIVE_PATH = "/resources/describe-settings.json";

const QString DESCRIPTION_SETTINGS_KEY = "settings";
const QString SETTING_DEFAULT_KEY = "default";
const QString DESCRIPTION_NAME_KEY = "name";
const QString SETTING_DESCRIPTION_TYPE_KEY = "type";
const QString DESCRIPTION_COLUMNS_KEY = "columns";

const QString SETTINGS_VIEWPOINT_KEY = "viewpoint";

DomainServerSettingsManager::DomainServerSettingsManager() :
    _descriptionArray(),
    _configMap()
{
    // load the description object from the settings description
    QFile descriptionFile(QCoreApplication::applicationDirPath() + SETTINGS_DESCRIPTION_RELATIVE_PATH);
    descriptionFile.open(QIODevice::ReadOnly);

    QJsonParseError parseError;
    QJsonDocument descriptionDocument = QJsonDocument::fromJson(descriptionFile.readAll(), &parseError);

    if (descriptionDocument.isObject()) {
        QJsonObject descriptionObject = descriptionDocument.object();

        const QString DESCRIPTION_VERSION_KEY = "version";

        if (descriptionObject.contains(DESCRIPTION_VERSION_KEY)) {
            // read the version from the settings description
            _descriptionVersion = descriptionObject[DESCRIPTION_VERSION_KEY].toDouble();

            if (descriptionObject.contains(DESCRIPTION_SETTINGS_KEY)) {
                _descriptionArray = descriptionDocument.object()[DESCRIPTION_SETTINGS_KEY].toArray();
                return;
            }
        }
    }

    static const QString MISSING_SETTINGS_DESC_MSG =
        QString("Did not find settings description in JSON at %1 - Unable to continue. domain-server will quit.\n%2 at %3")
        .arg(SETTINGS_DESCRIPTION_RELATIVE_PATH).arg(parseError.errorString()).arg(parseError.offset);
    static const int MISSING_SETTINGS_DESC_ERROR_CODE = 6;

    QMetaObject::invokeMethod(QCoreApplication::instance(), "queuedQuit", Qt::QueuedConnection,
                              Q_ARG(QString, MISSING_SETTINGS_DESC_MSG),
                              Q_ARG(int, MISSING_SETTINGS_DESC_ERROR_CODE));
}

void DomainServerSettingsManager::processSettingsRequestPacket(QSharedPointer<ReceivedMessage> message) {
    Assignment::Type type;
    message->readPrimitive(&type);

    QJsonObject responseObject = responseObjectForType(QString::number(type));
    auto json = QJsonDocument(responseObject).toJson();

    auto packetList = NLPacketList::create(PacketType::DomainSettings, QByteArray(), true, true);

    packetList->write(json);

    auto nodeList = DependencyManager::get<LimitedNodeList>();
    nodeList->sendPacketList(std::move(packetList), message->getSenderSockAddr());
}

void DomainServerSettingsManager::setupConfigMap(const QStringList& argumentList) {
    _argumentList = argumentList;
    _configMap.loadMasterAndUserConfig(_argumentList);

    // What settings version were we before and what are we using now?
    // Do we need to do any re-mapping?
    QSettings appSettings;
    const QString JSON_SETTINGS_VERSION_KEY = "json-settings/version";
    double oldVersion = appSettings.value(JSON_SETTINGS_VERSION_KEY, 0.0).toDouble();

    if (oldVersion != _descriptionVersion) {
        const QString ALLOWED_USERS_SETTINGS_KEYPATH = "security.allowed_users";
        const QString RESTRICTED_ACCESS_SETTINGS_KEYPATH = "security.restricted_access";
        const QString ALLOWED_EDITORS_SETTINGS_KEYPATH = "security.allowed_editors";
        const QString EDITORS_ARE_REZZERS_KEYPATH = "security.editors_are_rezzers";

        qDebug() << "Previous domain-server settings version was"
            << QString::number(oldVersion, 'g', 8) << "and the new version is"
            << QString::number(_descriptionVersion, 'g', 8) << "- checking if any re-mapping is required";

        // we have a version mismatch - for now handle custom behaviour here since there are not many remappings
        if (oldVersion < 1.0) {
            // This was prior to the introduction of security.restricted_access
            // If the user has a list of allowed users then set their value for security.restricted_access to true

            QVariant* allowedUsers = valueForKeyPath(_configMap.getMergedConfig(), ALLOWED_USERS_SETTINGS_KEYPATH);

            if (allowedUsers
                && allowedUsers->canConvert(QMetaType::QVariantList)
                && reinterpret_cast<QVariantList*>(allowedUsers)->size() > 0) {

                qDebug() << "Forcing security.restricted_access to TRUE since there was an"
                    << "existing list of allowed users.";

                // In the pre-toggle system the user had a list of allowed users, so
                // we need to set security.restricted_access to true
                QVariant* restrictedAccess = valueForKeyPath(_configMap.getUserConfig(),
                                                             RESTRICTED_ACCESS_SETTINGS_KEYPATH,
                                                             true);

                *restrictedAccess = QVariant(true);

                // write the new settings to the json file
                persistToFile();

                // reload the master and user config so that the merged config is right
                _configMap.loadMasterAndUserConfig(_argumentList);
            }
        }

        if (oldVersion < 1.1) {
            static const QString ENTITY_SERVER_SETTINGS_KEY = "entity_server_settings";
            static const QString ENTITY_FILE_NAME_KEY = "persistFilename";
            static const QString ENTITY_FILE_PATH_KEYPATH = ENTITY_SERVER_SETTINGS_KEY + ".persistFilePath";

            // this was prior to change of poorly named entitiesFileName to entitiesFilePath
            QVariant* persistFileNameVariant = valueForKeyPath(_configMap.getMergedConfig(),
                                                               ENTITY_SERVER_SETTINGS_KEY + "." + ENTITY_FILE_NAME_KEY);
            if (persistFileNameVariant && persistFileNameVariant->canConvert(QMetaType::QString)) {
                QString persistFileName = persistFileNameVariant->toString();

                qDebug() << "Migrating persistFilename to persistFilePath for entity-server settings";

                // grab the persistFilePath option, create it if it doesn't exist
                QVariant* persistFilePath = valueForKeyPath(_configMap.getUserConfig(), ENTITY_FILE_PATH_KEYPATH, true);

                // write the migrated value
                *persistFilePath = persistFileName;

                // remove the old setting
                QVariant* entityServerVariant = valueForKeyPath(_configMap.getUserConfig(), ENTITY_SERVER_SETTINGS_KEY);
                if (entityServerVariant && entityServerVariant->canConvert(QMetaType::QVariantMap)) {
                    QVariantMap entityServerMap = entityServerVariant->toMap();
                    entityServerMap.remove(ENTITY_FILE_NAME_KEY);

                    *entityServerVariant = entityServerMap;
                }

                // write the new settings to the json file
                persistToFile();

                // reload the master and user config so that the merged config is right
                _configMap.loadMasterAndUserConfig(_argumentList);
            }

        }

        if (oldVersion < 1.2) {
            // This was prior to the base64 encoding of password for HTTP Basic Authentication.
            // If we have a password in the previous settings file, make it base 64
            static const QString BASIC_AUTH_PASSWORD_KEY_PATH { "security.http_password" };

            QVariant* passwordVariant = valueForKeyPath(_configMap.getUserConfig(), BASIC_AUTH_PASSWORD_KEY_PATH);

            if (passwordVariant && passwordVariant->canConvert(QMetaType::QString)) {
                QString plaintextPassword = passwordVariant->toString();

                qDebug() << "Migrating plaintext password to SHA256 hash in domain-server settings.";

                *passwordVariant = QCryptographicHash::hash(plaintextPassword.toUtf8(), QCryptographicHash::Sha256).toHex();

                // write the new settings to file
                persistToFile();

                // reload the master and user config so the merged config is correct
                _configMap.loadMasterAndUserConfig(_argumentList);
            }
        }

        if (oldVersion < 1.4) {
            // This was prior to the permissions-grid in the domain-server settings page
            bool isRestrictedAccess = valueOrDefaultValueForKeyPath(RESTRICTED_ACCESS_SETTINGS_KEYPATH).toBool();
            QStringList allowedUsers = valueOrDefaultValueForKeyPath(ALLOWED_USERS_SETTINGS_KEYPATH).toStringList();
            QStringList allowedEditors = valueOrDefaultValueForKeyPath(ALLOWED_EDITORS_SETTINGS_KEYPATH).toStringList();
            bool onlyEditorsAreRezzers = valueOrDefaultValueForKeyPath(EDITORS_ARE_REZZERS_KEYPATH).toBool();

            _standardAgentPermissions[NodePermissions::standardNameLocalhost].reset(
                new NodePermissions(NodePermissions::standardNameLocalhost));
            _standardAgentPermissions[NodePermissions::standardNameLocalhost]->setAll(true);
            _standardAgentPermissions[NodePermissions::standardNameAnonymous].reset(
                new NodePermissions(NodePermissions::standardNameAnonymous));
            _standardAgentPermissions[NodePermissions::standardNameLoggedIn].reset(
                new NodePermissions(NodePermissions::standardNameLoggedIn));
            _standardAgentPermissions[NodePermissions::standardNameFriends].reset(
                new NodePermissions(NodePermissions::standardNameFriends));

            if (isRestrictedAccess) {
                // only users in allow-users list can connect
                _standardAgentPermissions[NodePermissions::standardNameAnonymous]->clear(
                    NodePermissions::Permission::canConnectToDomain);
                _standardAgentPermissions[NodePermissions::standardNameLoggedIn]->clear(
                    NodePermissions::Permission::canConnectToDomain);
            } // else anonymous and logged-in retain default of canConnectToDomain = true

            foreach (QString allowedUser, allowedUsers) {
                // even if isRestrictedAccess is false, we have to add explicit rows for these users.
                _agentPermissions[allowedUser].reset(new NodePermissions(allowedUser));
                _agentPermissions[allowedUser]->set(NodePermissions::Permission::canConnectToDomain);
            }

            foreach (QString allowedEditor, allowedEditors) {
                if (!_agentPermissions.contains(allowedEditor)) {
                    _agentPermissions[allowedEditor].reset(new NodePermissions(allowedEditor));
                    if (isRestrictedAccess) {
                        // they can change locks, but can't connect.
                        _agentPermissions[allowedEditor]->clear(NodePermissions::Permission::canConnectToDomain);
                    }
                }
                _agentPermissions[allowedEditor]->set(NodePermissions::Permission::canAdjustLocks);
            }

            QList<QHash<QString, NodePermissionsPointer>> permissionsSets;
            permissionsSets << _standardAgentPermissions.get() << _agentPermissions.get();
            foreach (auto permissionsSet, permissionsSets) {
                foreach (QString userName, permissionsSet.keys()) {
                    if (onlyEditorsAreRezzers) {
                        if (permissionsSet[userName]->can(NodePermissions::Permission::canAdjustLocks)) {
                            permissionsSet[userName]->set(NodePermissions::Permission::canRezPermanentEntities);
                            permissionsSet[userName]->set(NodePermissions::Permission::canRezTemporaryEntities);
                        } else {
                            permissionsSet[userName]->clear(NodePermissions::Permission::canRezPermanentEntities);
                            permissionsSet[userName]->clear(NodePermissions::Permission::canRezTemporaryEntities);
                        }
                    } else {
                        permissionsSet[userName]->set(NodePermissions::Permission::canRezPermanentEntities);
                        permissionsSet[userName]->set(NodePermissions::Permission::canRezTemporaryEntities);
                    }
                }
            }

            packPermissions();
            _standardAgentPermissions.clear();
            _agentPermissions.clear();
        }

        if (oldVersion < 1.5) {
            // This was prior to operating hours, so add default hours
            validateDescriptorsMap();
        }
    }

    unpackPermissions();

    // write the current description version to our settings
    appSettings.setValue(JSON_SETTINGS_VERSION_KEY, _descriptionVersion);
}

QVariantMap& DomainServerSettingsManager::getDescriptorsMap() {
    validateDescriptorsMap();

    static const QString DESCRIPTORS{ "descriptors" };
    return *static_cast<QVariantMap*>(getSettingsMap()[DESCRIPTORS].data());
}

void DomainServerSettingsManager::validateDescriptorsMap() {
    static const QString WEEKDAY_HOURS{ "descriptors.weekday_hours" };
    static const QString WEEKEND_HOURS{ "descriptors.weekend_hours" };
    static const QString UTC_OFFSET{ "descriptors.utc_offset" };

    QVariant* weekdayHours = valueForKeyPath(_configMap.getUserConfig(), WEEKDAY_HOURS, true);
    QVariant* weekendHours = valueForKeyPath(_configMap.getUserConfig(), WEEKEND_HOURS, true);
    QVariant* utcOffset = valueForKeyPath(_configMap.getUserConfig(), UTC_OFFSET, true);

    static const QString OPEN{ "open" };
    static const QString CLOSE{ "close" };
    static const QString DEFAULT_OPEN{ "00:00" };
    static const QString DEFAULT_CLOSE{ "23:59" };
    bool wasMalformed = false;
    if (weekdayHours->isNull()) {
        *weekdayHours = QVariantList{ QVariantMap{ { OPEN, QVariant(DEFAULT_OPEN) }, { CLOSE, QVariant(DEFAULT_CLOSE) } } };
        wasMalformed = true;
    }
    if (weekendHours->isNull()) {
        *weekendHours = QVariantList{ QVariantMap{ { OPEN, QVariant(DEFAULT_OPEN) }, { CLOSE, QVariant(DEFAULT_CLOSE) } } };
        wasMalformed = true;
    }
    if (utcOffset->isNull()) {
        *utcOffset = QVariant(QTimeZone::systemTimeZone().offsetFromUtc(QDateTime::currentDateTime()) / (float)SECS_PER_HOUR);
        wasMalformed = true;
    }

    if (wasMalformed) {
        // write the new settings to file
        persistToFile();

        // reload the master and user config so the merged config is correct
        _configMap.loadMasterAndUserConfig(_argumentList);
    }
}

void DomainServerSettingsManager::packPermissionsForMap(QString mapName,
                                                        NodePermissionsMap& agentPermissions,
                                                        QString keyPath) {
    // find (or create) the "security" section of the settings map
    QVariant* security = valueForKeyPath(_configMap.getUserConfig(), "security");
    if (!security || !security->canConvert(QMetaType::QVariantMap)) {
        security = valueForKeyPath(_configMap.getUserConfig(), "security", true);
        (*security) = QVariantMap();
    }

    // find (or create) whichever subsection of "security" we are packing
    QVariant* permissions = valueForKeyPath(_configMap.getUserConfig(), keyPath);
    if (!permissions || !permissions->canConvert(QMetaType::QVariantList)) {
        permissions = valueForKeyPath(_configMap.getUserConfig(), keyPath, true);
        (*permissions) = QVariantList();
    }

    // convert details for each member of the section
    QVariantList* permissionsList = reinterpret_cast<QVariantList*>(permissions);
    (*permissionsList).clear();
    foreach (QString userName, agentPermissions.keys()) {
        *permissionsList += agentPermissions[userName]->toVariant();
    }
}

void DomainServerSettingsManager::packPermissions() {
    // transfer details from _agentPermissions to _configMap

    // save settings for anonymous / logged-in / localhost
    packPermissionsForMap("standard_permissions", _standardAgentPermissions, AGENT_STANDARD_PERMISSIONS_KEYPATH);

    // save settings for specific users
    packPermissionsForMap("permissions", _agentPermissions, AGENT_PERMISSIONS_KEYPATH);

    // save settings for groups
    packPermissionsForMap("permissions", _groupPermissions, GROUP_PERMISSIONS_KEYPATH);

    // save settings for blacklist groups
    packPermissionsForMap("permissions", _groupForbiddens, GROUP_FORBIDDENS_KEYPATH);

    persistToFile();
    _configMap.loadMasterAndUserConfig(_argumentList);
}

void DomainServerSettingsManager::unpackPermissions() {
    // transfer details from _configMap to _agentPermissions;

    _standardAgentPermissions.clear();
    _agentPermissions.clear();
    _groupPermissions.clear();
    _groupForbiddens.clear();

    bool foundLocalhost = false;
    bool foundAnonymous = false;
    bool foundLoggedIn = false;
    bool foundFriends = false;
    bool needPack = false;

    QVariant* standardPermissions = valueForKeyPath(_configMap.getUserConfig(), AGENT_STANDARD_PERMISSIONS_KEYPATH);
    if (!standardPermissions || !standardPermissions->canConvert(QMetaType::QVariantList)) {
        qDebug() << "failed to extract standard permissions from settings.";
        standardPermissions = valueForKeyPath(_configMap.getUserConfig(), AGENT_STANDARD_PERMISSIONS_KEYPATH, true);
        (*standardPermissions) = QVariantList();
    }
    QVariant* permissions = valueForKeyPath(_configMap.getUserConfig(), AGENT_PERMISSIONS_KEYPATH);
    if (!permissions || !permissions->canConvert(QMetaType::QVariantList)) {
        qDebug() << "failed to extract permissions from settings.";
        permissions = valueForKeyPath(_configMap.getUserConfig(), AGENT_PERMISSIONS_KEYPATH, true);
        (*permissions) = QVariantList();
    }
    QVariant* groupPermissions = valueForKeyPath(_configMap.getUserConfig(), GROUP_PERMISSIONS_KEYPATH);
    if (!groupPermissions || !groupPermissions->canConvert(QMetaType::QVariantList)) {
        qDebug() << "failed to extract group permissions from settings.";
        groupPermissions = valueForKeyPath(_configMap.getUserConfig(), GROUP_PERMISSIONS_KEYPATH, true);
        (*groupPermissions) = QVariantList();
    }
    QVariant* groupForbiddens = valueForKeyPath(_configMap.getUserConfig(), GROUP_FORBIDDENS_KEYPATH);
    if (!groupForbiddens || !groupForbiddens->canConvert(QMetaType::QVariantList)) {
        qDebug() << "failed to extract group forbiddens from settings.";
        groupForbiddens = valueForKeyPath(_configMap.getUserConfig(), GROUP_FORBIDDENS_KEYPATH, true);
        (*groupForbiddens) = QVariantList();
    }

    QList<QVariant> standardPermissionsList = standardPermissions->toList();
    foreach (QVariant permsHash, standardPermissionsList) {
        NodePermissionsPointer perms { new NodePermissions(permsHash.toMap()) };
        QString id = perms->getID();
        foundLocalhost |= (id == NodePermissions::standardNameLocalhost);
        foundAnonymous |= (id == NodePermissions::standardNameAnonymous);
        foundLoggedIn |= (id == NodePermissions::standardNameLoggedIn);
        foundFriends |= (id == NodePermissions::standardNameFriends);
        if (_standardAgentPermissions.contains(id)) {
            qDebug() << "duplicate name in standard permissions table: " << id;
            _standardAgentPermissions[id] |= perms;
            needPack = true;
        } else {
            _standardAgentPermissions[id] = perms;
        }
    }

    QList<QVariant> permissionsList = permissions->toList();
    foreach (QVariant permsHash, permissionsList) {
        NodePermissionsPointer perms { new NodePermissions(permsHash.toMap()) };
        QString id = perms->getID();
        if (_agentPermissions.contains(id)) {
            qDebug() << "duplicate name in permissions table: " << id;
            _agentPermissions[id] |= perms;
            needPack = true;
        } else {
            _agentPermissions[id] = perms;
        }
    }

    QList<QVariant> groupPermissionsList = groupPermissions->toList();
    foreach (QVariant permsHash, groupPermissionsList) {
        NodePermissionsPointer perms { new NodePermissions(permsHash.toMap()) };
        QString id = perms->getID();
        if (_groupPermissions.contains(id)) {
            qDebug() << "duplicate name in group permissions table: " << id;
            _groupPermissions[id] |= perms;
            needPack = true;
        } else {
            _groupPermissions[id] = perms;
        }
        if (perms->isGroup()) {
            // the group-id was cached.  hook-up the id in the id->group hash
            _groupByID[perms->getGroupID()] = _groupPermissions[id];
        }
    }

    QList<QVariant> groupForbiddensList = groupForbiddens->toList();
    foreach (QVariant permsHash, groupForbiddensList) {
        NodePermissionsPointer perms { new NodePermissions(permsHash.toMap()) };
        QString id = perms->getID();
        if (_groupForbiddens.contains(id)) {
            qDebug() << "duplicate name in group forbiddens table: " << id;
            _groupForbiddens[id] |= perms;
            needPack = true;
        } else {
            _groupForbiddens[id] = perms;
        }
        if (perms->isGroup()) {
            // the group-id was cached.  hook-up the id in the id->group hash
            _groupByID[perms->getGroupID()] = _groupForbiddens[id];
        }
    }

    // if any of the standard names are missing, add them
    if (!foundLocalhost) {
        NodePermissionsPointer perms { new NodePermissions(NodePermissions::standardNameLocalhost) };
        perms->setAll(true);
        _standardAgentPermissions[perms->getID()] = perms;
        needPack = true;
    }
    if (!foundAnonymous) {
        NodePermissionsPointer perms { new NodePermissions(NodePermissions::standardNameAnonymous) };
        _standardAgentPermissions[perms->getID()] = perms;
        needPack = true;
    }
    if (!foundLoggedIn) {
        NodePermissionsPointer perms { new NodePermissions(NodePermissions::standardNameLoggedIn) };
        _standardAgentPermissions[perms->getID()] = perms;
        needPack = true;
    }
    if (!foundFriends) {
        NodePermissionsPointer perms { new NodePermissions(NodePermissions::standardNameFriends) };
        _standardAgentPermissions[perms->getID()] = perms;
        needPack = true;
    }

    if (needPack) {
        packPermissions();
    }

    // attempt to retrieve any missing group-IDs
    requestMissingGroupIDs();


    #ifdef WANT_DEBUG
    qDebug() << "--------------- permissions ---------------------";
    QList<QHash<QString, NodePermissionsPointer>> permissionsSets;
    permissionsSets << _standardAgentPermissions.get() << _agentPermissions.get()
                    << _groupPermissions.get() << _groupForbiddens.get();
    foreach (auto permissionSet, permissionsSets) {
        QHashIterator<QString, NodePermissionsPointer> i(permissionSet);
        while (i.hasNext()) {
            i.next();
            NodePermissionsPointer perms = i.value();
            if (perms->isGroup()) {
                qDebug() << i.key() << perms->getGroupID() << perms;
            } else {
                qDebug() << i.key() << perms;
            }
        }
    }
    #endif
}

NodePermissions DomainServerSettingsManager::getStandardPermissionsForName(const QString& name) const {
    if (_standardAgentPermissions.contains(name)) {
        return *(_standardAgentPermissions[name].get());
    }
    NodePermissions nullPermissions;
    nullPermissions.setAll(false);
    return nullPermissions;
}

NodePermissions DomainServerSettingsManager::getPermissionsForName(const QString& name) const {
    if (_agentPermissions.contains(name)) {
        return *(_agentPermissions[name].get());
    }
    NodePermissions nullPermissions;
    nullPermissions.setAll(false);
    return nullPermissions;
}

NodePermissions DomainServerSettingsManager::getPermissionsForGroup(const QString& groupname) const {
    if (_groupPermissions.contains(groupname)) {
        return *(_groupPermissions[groupname].get());
    }
    NodePermissions nullPermissions;
    nullPermissions.setAll(false);
    return nullPermissions;
}

NodePermissions DomainServerSettingsManager::getPermissionsForGroup(const QUuid& groupID) const {
    if (!_groupByID.contains(groupID)) {
        NodePermissions nullPermissions;
        nullPermissions.setAll(false);
        return nullPermissions;
    }
    QString groupName = _groupByID[groupID]->getID();
    return getPermissionsForGroup(groupName);
}


NodePermissions DomainServerSettingsManager::getForbiddensForGroup(const QString& groupname) const {
    if (_groupForbiddens.contains(groupname)) {
        return *(_groupForbiddens[groupname].get());
    }
    NodePermissions nullForbiddens;
    nullForbiddens.setAll(false);
    return nullForbiddens;
}

NodePermissions DomainServerSettingsManager::getForbiddensForGroup(const QUuid& groupID) const {
    if (!_groupByID.contains(groupID)) {
        NodePermissions nullForbiddens;
        nullForbiddens.setAll(false);
        return nullForbiddens;
    }
    QString groupName = _groupByID[groupID]->getID();
    return getForbiddensForGroup(groupName);
}



QVariant DomainServerSettingsManager::valueOrDefaultValueForKeyPath(const QString& keyPath) {
    const QVariant* foundValue = valueForKeyPath(_configMap.getMergedConfig(), keyPath);

    if (foundValue) {
        return *foundValue;
    } else {
        int dotIndex = keyPath.indexOf('.');

        QString groupKey = keyPath.mid(0, dotIndex);
        QString settingKey = keyPath.mid(dotIndex + 1);

        foreach(const QVariant& group, _descriptionArray.toVariantList()) {
            QVariantMap groupMap = group.toMap();

            if (groupMap[DESCRIPTION_NAME_KEY].toString() == groupKey) {
                foreach(const QVariant& setting, groupMap[DESCRIPTION_SETTINGS_KEY].toList()) {
                    QVariantMap settingMap = setting.toMap();
                    if (settingMap[DESCRIPTION_NAME_KEY].toString() == settingKey) {
                        return settingMap[SETTING_DEFAULT_KEY];
                    }
                }

                return QVariant();
            }
        }
    }

    return QVariant();
}

bool DomainServerSettingsManager::handlePublicHTTPRequest(HTTPConnection* connection, const QUrl &url) {
    if (connection->requestOperation() == QNetworkAccessManager::GetOperation && url.path() == SETTINGS_PATH_JSON) {
        // this is a GET operation for our settings

        // check if there is a query parameter for settings affecting a particular type of assignment
        const QString SETTINGS_TYPE_QUERY_KEY = "type";
        QUrlQuery settingsQuery(url);
        QString typeValue = settingsQuery.queryItemValue(SETTINGS_TYPE_QUERY_KEY);

        if (!typeValue.isEmpty()) {
            QJsonObject responseObject = responseObjectForType(typeValue);

            connection->respond(HTTPConnection::StatusCode200, QJsonDocument(responseObject).toJson(), "application/json");

            return true;
        } else {
            return false;
        }
    }

    return false;
}

bool DomainServerSettingsManager::handleAuthenticatedHTTPRequest(HTTPConnection *connection, const QUrl &url) {
    if (connection->requestOperation() == QNetworkAccessManager::PostOperation && url.path() == SETTINGS_PATH_JSON) {
        // this is a POST operation to change one or more settings
        QJsonDocument postedDocument = QJsonDocument::fromJson(connection->requestContent());
        QJsonObject postedObject = postedDocument.object();

        qDebug() << "DomainServerSettingsManager postedObject -" << postedObject;

        // we recurse one level deep below each group for the appropriate setting
        bool restartRequired = recurseJSONObjectAndOverwriteSettings(postedObject);

        // store whatever the current _settingsMap is to file
        persistToFile();

        // return success to the caller
        QString jsonSuccess = "{\"status\": \"success\"}";
        connection->respond(HTTPConnection::StatusCode200, jsonSuccess.toUtf8(), "application/json");

        // defer a restart to the domain-server, this gives our HTTPConnection enough time to respond
        if (restartRequired) {
            const int DOMAIN_SERVER_RESTART_TIMER_MSECS = 1000;
            QTimer::singleShot(DOMAIN_SERVER_RESTART_TIMER_MSECS, qApp, SLOT(restart()));
        } else {
            unpackPermissions();
            emit updateNodePermissions();
        }

        return true;
    } else if (connection->requestOperation() == QNetworkAccessManager::GetOperation && url.path() == SETTINGS_PATH_JSON) {
        // setup a JSON Object with descriptions and non-omitted settings
        const QString SETTINGS_RESPONSE_DESCRIPTION_KEY = "descriptions";
        const QString SETTINGS_RESPONSE_VALUE_KEY = "values";
        const QString SETTINGS_RESPONSE_LOCKED_VALUES_KEY = "locked";

        QJsonObject rootObject;
        rootObject[SETTINGS_RESPONSE_DESCRIPTION_KEY] = _descriptionArray;
        rootObject[SETTINGS_RESPONSE_VALUE_KEY] = responseObjectForType("", true);
        rootObject[SETTINGS_RESPONSE_LOCKED_VALUES_KEY] = QJsonDocument::fromVariant(_configMap.getMasterConfig()).object();

        connection->respond(HTTPConnection::StatusCode200, QJsonDocument(rootObject).toJson(), "application/json");
    }

    return false;
}

QJsonObject DomainServerSettingsManager::responseObjectForType(const QString& typeValue, bool isAuthenticated) {
    QJsonObject responseObject;

    if (!typeValue.isEmpty() || isAuthenticated) {
        // convert the string type value to a QJsonValue
        QJsonValue queryType = typeValue.isEmpty() ? QJsonValue() : QJsonValue(typeValue.toInt());

        const QString AFFECTED_TYPES_JSON_KEY = "assignment-types";

        // enumerate the groups in the description object to find which settings to pass
        foreach(const QJsonValue& groupValue, _descriptionArray) {
            QJsonObject groupObject = groupValue.toObject();
            QString groupKey = groupObject[DESCRIPTION_NAME_KEY].toString();
            QJsonArray groupSettingsArray = groupObject[DESCRIPTION_SETTINGS_KEY].toArray();

            QJsonObject groupResponseObject;

            foreach(const QJsonValue& settingValue, groupSettingsArray) {
                const QString VALUE_HIDDEN_FLAG_KEY = "value-hidden";

                QJsonObject settingObject = settingValue.toObject();

                if (!settingObject[VALUE_HIDDEN_FLAG_KEY].toBool()) {
                    QJsonArray affectedTypesArray = settingObject[AFFECTED_TYPES_JSON_KEY].toArray();
                    if (affectedTypesArray.isEmpty()) {
                        affectedTypesArray = groupObject[AFFECTED_TYPES_JSON_KEY].toArray();
                    }

                    if (affectedTypesArray.contains(queryType) ||
                        (queryType.isNull() && isAuthenticated)) {
                        // this is a setting we should include in the responseObject

                        QString settingName = settingObject[DESCRIPTION_NAME_KEY].toString();

                        // we need to check if the settings map has a value for this setting
                        QVariant variantValue;

                        if (!groupKey.isEmpty()) {
                             QVariant settingsMapGroupValue = _configMap.getMergedConfig().value(groupKey);

                            if (!settingsMapGroupValue.isNull()) {
                                variantValue = settingsMapGroupValue.toMap().value(settingName);
                            }
                        } else {
                            variantValue = _configMap.getMergedConfig().value(settingName);
                        }

                        QJsonValue result;

                        if (variantValue.isNull()) {
                            // no value for this setting, pass the default
                            if (settingObject.contains(SETTING_DEFAULT_KEY)) {
                                result = settingObject[SETTING_DEFAULT_KEY];
                            } else {
                                // users are allowed not to provide a default for string values
                                // if so we set to the empty string
                                result = QString("");
                            }

                        } else {
                            result = QJsonValue::fromVariant(variantValue);
                        }

                        if (!groupKey.isEmpty()) {
                            // this belongs in the group object
                            groupResponseObject[settingName] = result;
                        } else {
                            // this is a value that should be at the root
                            responseObject[settingName] = result;
                        }
                    }
                }
            }

            if (!groupKey.isEmpty() && !groupResponseObject.isEmpty()) {
                // set this group's object to the constructed object
                responseObject[groupKey] = groupResponseObject;
            }
        }
    }


    return responseObject;
}

void DomainServerSettingsManager::updateSetting(const QString& key, const QJsonValue& newValue, QVariantMap& settingMap,
                                                const QJsonObject& settingDescription) {
    if (newValue.isString()) {
        if (newValue.toString().isEmpty()) {
            // this is an empty value, clear it in settings variant so the default is sent
            settingMap.remove(key);
        } else {
            // make sure the resulting json value has the right type
            QString settingType = settingDescription[SETTING_DESCRIPTION_TYPE_KEY].toString();
            const QString INPUT_DOUBLE_TYPE = "double";
            const QString INPUT_INTEGER_TYPE = "int";

            if (settingType == INPUT_DOUBLE_TYPE) {
                settingMap[key] = newValue.toString().toDouble();
            } else if (settingType == INPUT_INTEGER_TYPE) {
                settingMap[key] = newValue.toString().toInt();
            } else {
                QString sanitizedValue = newValue.toString();

                // we perform special handling for viewpoints here
                // we do not want them to be prepended with a slash
                if (key == SETTINGS_VIEWPOINT_KEY && !sanitizedValue.startsWith('/')) {
                    sanitizedValue.prepend('/');
                }

                settingMap[key] = sanitizedValue;
            }
        }
    } else if (newValue.isBool()) {
        settingMap[key] = newValue.toBool();
    } else if (newValue.isObject()) {
        if (!settingMap.contains(key)) {
            // we don't have a map below this key yet, so set it up now
            settingMap[key] = QVariantMap();
        }

        QVariant& possibleMap = settingMap[key];

        if (!possibleMap.canConvert(QMetaType::QVariantMap)) {
            // if this isn't a map then we need to make it one, otherwise we're about to crash
            qDebug() << "Value at" << key << "was not the expected QVariantMap while updating DS settings"
                << "- removing existing value and making it a QVariantMap";
            possibleMap = QVariantMap();
        }

        QVariantMap& thisMap = *reinterpret_cast<QVariantMap*>(possibleMap.data());
        foreach(const QString childKey, newValue.toObject().keys()) {

            QJsonObject childDescriptionObject = settingDescription;

            // is this the key? if so we have the description already
            if (key != settingDescription[DESCRIPTION_NAME_KEY].toString()) {
                // otherwise find the description object for this childKey under columns
                foreach(const QJsonValue& column, settingDescription[DESCRIPTION_COLUMNS_KEY].toArray()) {
                    if (column.isObject()) {
                        QJsonObject thisDescription = column.toObject();
                        if (thisDescription[DESCRIPTION_NAME_KEY] == childKey) {
                            childDescriptionObject = column.toObject();
                            break;
                        }
                    }
                }
            }

            QString sanitizedKey = childKey;

            if (key == SETTINGS_PATHS_KEY && !sanitizedKey.startsWith('/')) {
                // We perform special handling for paths here.
                // If we got sent a path without a leading slash then we add it.
                sanitizedKey.prepend("/");
            }

            updateSetting(sanitizedKey, newValue.toObject()[childKey], thisMap, childDescriptionObject);
        }

        if (settingMap[key].toMap().isEmpty()) {
            // we've cleared all of the settings below this value, so remove this one too
            settingMap.remove(key);
        }
    } else if (newValue.isArray()) {
        // we just assume array is replacement
        // TODO: we still need to recurse here with the description in case values in the array have special types
        settingMap[key] = newValue.toArray().toVariantList();
    }

    sortPermissions();
}

QJsonObject DomainServerSettingsManager::settingDescriptionFromGroup(const QJsonObject& groupObject, const QString& settingName) {
    foreach(const QJsonValue& settingValue, groupObject[DESCRIPTION_SETTINGS_KEY].toArray()) {
        QJsonObject settingObject = settingValue.toObject();
        if (settingObject[DESCRIPTION_NAME_KEY].toString() == settingName) {
            return settingObject;
        }
    }

    return QJsonObject();
}

bool DomainServerSettingsManager::recurseJSONObjectAndOverwriteSettings(const QJsonObject& postedObject) {
    auto& settingsVariant = _configMap.getUserConfig();
    bool needRestart = false;

    // Iterate on the setting groups
    foreach(const QString& rootKey, postedObject.keys()) {
        QJsonValue rootValue = postedObject[rootKey];

        if (!settingsVariant.contains(rootKey)) {
            // we don't have a map below this key yet, so set it up now
            settingsVariant[rootKey] = QVariantMap();
        }

        QVariantMap* thisMap = &settingsVariant;

        QJsonObject groupDescriptionObject;

        // we need to check the description array to see if this is a root setting or a group setting
        foreach(const QJsonValue& groupValue, _descriptionArray) {
            if (groupValue.toObject()[DESCRIPTION_NAME_KEY] == rootKey) {
                // we matched a group - keep this since we'll use it below to update the settings
                groupDescriptionObject = groupValue.toObject();

                // change the map we will update to be the map for this group
                thisMap = reinterpret_cast<QVariantMap*>(settingsVariant[rootKey].data());

                break;
            }
        }

        if (groupDescriptionObject.isEmpty()) {
            // this is a root value, so we can call updateSetting for it directly
            // first we need to find our description value for it

            QJsonObject matchingDescriptionObject;

            foreach(const QJsonValue& groupValue, _descriptionArray) {
                // find groups with root values (they don't have a group name)
                QJsonObject groupObject = groupValue.toObject();
                if (!groupObject.contains(DESCRIPTION_NAME_KEY)) {
                    // this is a group with root values - check if our setting is in here
                    matchingDescriptionObject = settingDescriptionFromGroup(groupObject, rootKey);

                    if (!matchingDescriptionObject.isEmpty()) {
                        break;
                    }
                }
            }

            if (!matchingDescriptionObject.isEmpty()) {
                updateSetting(rootKey, rootValue, *thisMap, matchingDescriptionObject);
                if (rootKey != "security") {
                    needRestart = true;
                }
            } else {
                qDebug() << "Setting for root key" << rootKey << "does not exist - cannot update setting.";
            }
        } else {
            // this is a group - iterate on the settings in the group
            foreach(const QString& settingKey, rootValue.toObject().keys()) {
                // make sure this particular setting exists and we have a description object for it
                QJsonObject matchingDescriptionObject = settingDescriptionFromGroup(groupDescriptionObject, settingKey);

                // if we matched the setting then update the value
                if (!matchingDescriptionObject.isEmpty()) {
                    QJsonValue settingValue = rootValue.toObject()[settingKey];
                    updateSetting(settingKey, settingValue, *thisMap, matchingDescriptionObject);
                    if (rootKey != "security") {
                        needRestart = true;
                    }
                } else {
                    qDebug() << "Could not find description for setting" << settingKey << "in group" << rootKey <<
                        "- cannot update setting.";
                }
            }
        }

        if (settingsVariant[rootKey].toMap().empty()) {
            // we've cleared all of the settings below this value, so remove this one too
            settingsVariant.remove(rootKey);
        }
    }

    // re-merge the user and master configs after a settings change
    _configMap.mergeMasterAndUserConfigs();

    return needRestart;
}

// Compare two members of a permissions list
bool permissionVariantLessThan(const QVariant &v1, const QVariant &v2) {
    if (!v1.canConvert(QMetaType::QVariantMap) ||
        !v2.canConvert(QMetaType::QVariantMap)) {
        return v1.toString() < v2.toString();
    }
    QVariantMap m1 = v1.toMap();
    QVariantMap m2 = v2.toMap();

    if (!m1.contains("permissions_id") ||
        !m2.contains("permissions_id")) {
        return v1.toString() < v2.toString();
    }
    return m1["permissions_id"].toString() < m2["permissions_id"].toString();
}

void DomainServerSettingsManager::sortPermissions() {
    // sort the permission-names
    QVariant* standardPermissions = valueForKeyPath(_configMap.getUserConfig(), AGENT_STANDARD_PERMISSIONS_KEYPATH);
    if (standardPermissions && standardPermissions->canConvert(QMetaType::QVariantList)) {
        QList<QVariant>* standardPermissionsList = reinterpret_cast<QVariantList*>(standardPermissions);
        std::sort((*standardPermissionsList).begin(), (*standardPermissionsList).end(), permissionVariantLessThan);
    }
    QVariant* permissions = valueForKeyPath(_configMap.getUserConfig(), AGENT_PERMISSIONS_KEYPATH);
    if (permissions && permissions->canConvert(QMetaType::QVariantList)) {
        QList<QVariant>* permissionsList = reinterpret_cast<QVariantList*>(permissions);
        std::sort((*permissionsList).begin(), (*permissionsList).end(), permissionVariantLessThan);
    }
}

void DomainServerSettingsManager::persistToFile() {
    sortPermissions();

    // make sure we have the dir the settings file is supposed to live in
    QFileInfo settingsFileInfo(_configMap.getUserConfigFilename());

    if (!settingsFileInfo.dir().exists()) {
        settingsFileInfo.dir().mkpath(".");
    }

    QFile settingsFile(_configMap.getUserConfigFilename());

    if (settingsFile.open(QIODevice::WriteOnly)) {
        settingsFile.write(QJsonDocument::fromVariant(_configMap.getUserConfig()).toJson());
    } else {
        qCritical("Could not write to JSON settings file. Unable to persist settings.");
    }
}

void DomainServerSettingsManager::requestMissingGroupIDs() {
    if (!DependencyManager::get<AccountManager>()->hasAuthEndpoint()) {
        // can't yet.
        return;
    }

    QHashIterator<QString, NodePermissionsPointer> i_permissions(_groupPermissions.get());
    while (i_permissions.hasNext()) {
        i_permissions.next();
        NodePermissionsPointer perms = i_permissions.value();
        if (!perms->getGroupID().isNull()) {
            // we already know this group's ID
            continue;
        }

        // make a call to metaverse api to turn the group name into a group ID
        getGroupID(perms->getID());
    }
    QHashIterator<QString, NodePermissionsPointer> i_forbiddens(_groupForbiddens.get());
    while (i_forbiddens.hasNext()) {
        i_forbiddens.next();
        NodePermissionsPointer perms = i_forbiddens.value();
        if (!perms->getGroupID().isNull()) {
            // we already know this group's ID
            continue;
        }

        // make a call to metaverse api to turn the group name into a group ID
        getGroupID(perms->getID());
    }
}

NodePermissionsPointer DomainServerSettingsManager::lookupGroupByID(const QUuid& id) {
    if (_groupByID.contains(id)) {
        return _groupByID[id];
    }
    return nullptr;
}

void DomainServerSettingsManager::getGroupID(const QString& groupname) {
    JSONCallbackParameters callbackParams;
    callbackParams.jsonCallbackReceiver = this;
    callbackParams.jsonCallbackMethod = "getGroupIDJSONCallback";
    callbackParams.errorCallbackReceiver = this;
    callbackParams.errorCallbackMethod = "getGroupIDErrorCallback";

    const QString GET_GROUP_ID_PATH = "api/v1/groups/name/%1";

    qDebug() << "************* Requesting group ID for group named" << groupname;

    DependencyManager::get<AccountManager>()->sendRequest(GET_GROUP_ID_PATH.arg(groupname),
                                                          AccountManagerAuth::Required,
                                                          QNetworkAccessManager::GetOperation, callbackParams);
}

void DomainServerSettingsManager::getGroupIDJSONCallback(QNetworkReply& requestReply) {
    // {
    //     "data":{
    //         "groups":[{
    //             "description":null,
    //             "id":"fd55479a-265d-4990-854e-3d04214ad1b0",
    //             "is_list":false,
    //             "membership":{
    //                 "permissions":{
    //                     "custom_1=":false,
    //                     "custom_2=":false,
    //                     "custom_3=":false,
    //                     "custom_4=":false,
    //                     "del_group=":true,
    //                     "invite_member=":true,
    //                     "kick_member=":true,
    //                     "list_members=":true,
    //                     "mv_group=":true,
    //                     "query_members=":true,
    //                     "rank_member=":true
    //                 },
    //                 "rank":{
    //                     "name=":"owner",
    //                     "order=":0
    //                 }
    //             },
    //             "name":"Blerg Blah"
    //         }]
    //     },
    //     "status":"success"
    // }
    QJsonObject jsonObject = QJsonDocument::fromJson(requestReply.readAll()).object();
    if (jsonObject["status"].toString() == "success") {
        QJsonArray groups = jsonObject["data"].toObject()["groups"].toArray();
        for (int i = 0; i < groups.size(); i++) {
            QJsonObject group = groups.at(i).toObject();
            QString groupName = group["name"].toString();
            QUuid groupID = QUuid(group["id"].toString());

            bool found = false;
            if (_groupPermissions.contains(groupName)) {
                qDebug() << "ID for group:" << groupName << "is" << groupID;
                _groupPermissions[groupName]->setGroupID(groupID);
                _groupByID[groupID] = _groupPermissions[groupName];
                found = true;
            }
            if (_groupForbiddens.contains(groupName)) {
                qDebug() << "ID for group:" << groupName << "is" << groupID;
                _groupForbiddens[groupName]->setGroupID(groupID);
                _groupByID[groupID] = _groupForbiddens[groupName];
                found = true;
            }

            if (found) {
                packPermissions();
            } else {
                qDebug() << "DomainServerSettingsManager::getGroupIDJSONCallback got response for unknown group:" << groupName;
            }
        }
    } else {
        qDebug() << "getGroupID api call returned:" << QJsonDocument(jsonObject).toJson(QJsonDocument::Compact);
    }
}

void DomainServerSettingsManager::getGroupIDErrorCallback(QNetworkReply& requestReply) {
    qDebug() << "******************** getGroupID api call failed:" << requestReply.error();
}

void DomainServerSettingsManager::recordGroupMembership(const QString& name, const QUuid groupID, bool isMember) {
    _groupMembership[name][groupID] = isMember;
}

bool DomainServerSettingsManager::isGroupMember(const QString& name, const QUuid& groupID) {
    return _groupMembership[name][groupID];
}

QList<QUuid> DomainServerSettingsManager::getGroupIDs() {
    QList<QUuid> result;
    foreach (QString groupName, _groupPermissions.keys()) {
        if (_groupPermissions[groupName]->isGroup()) {
            result << _groupPermissions[groupName]->getGroupID();
        }
    }
    return result;
}

QList<QUuid> DomainServerSettingsManager::getBlacklistGroupIDs() {
    QList<QUuid> result;
    foreach (QString groupName, _groupForbiddens.keys()) {
        if (_groupForbiddens[groupName]->isGroup()) {
            result << _groupForbiddens[groupName]->getGroupID();
        }
    }
    return result;
}
