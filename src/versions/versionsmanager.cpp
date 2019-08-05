#include <QtCore/QStandardPaths>
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>
#include <QtNetwork/QNetworkReply>

#include "storage/fileinfo.h"
#include "json/assetsindex.h"
#include "json/versionindex.h"
#include "json/prefixversionsindex.h"
#include "json/dataindex.h"
#include "utils/network.h"
#include "utils/platform.h"
#include "versionsmanager.h"

namespace Ttyh {
namespace Versions {
using namespace Json;
using namespace Logs;
using namespace Storage;

VersionsManager::VersionsManager(const QString &dirName, QString url,
                                 QSharedPointer<QNetworkAccessManager> nam,
                                 const QSharedPointer<Logger> &logger)
    : storeUrl(std::move(url)),
      nam(std::move(nam)),
      log(logger, "Versions"),
      fetchingPrefixes(false),
      fetchingVersionIndexes(false)
{
    auto pattern = QString("%1/%2");
    auto basePath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);

    dataPath = pattern.arg(basePath, dirName);
    versionsPath = pattern.arg(dataPath, "versions");

    QDir().mkpath(versionsPath);

    indexPath = QString("%1/%2").arg(versionsPath, "prefixes.json");
    QFile indexFile(indexPath);

    if (indexFile.open(QIODevice::ReadOnly)) {
        index = PrefixesIndex(QJsonDocument::fromJson(indexFile.readAll()).object());
    } else {
        log.info("Default version index have been created");
    }

    foreach (auto id, index.prefixes.keys()) {
        if (id.isEmpty())
            continue;

        prefixes.insert(id, Prefix(id, index.prefixes[id].about));
        findLocalVersions(id);
    }

    log.info(QString("Initialized with %1 prefix(es)").arg(index.prefixes.count()));
}

void VersionsManager::findLocalVersions(const QString &prefixId)
{
    Prefix &prefix = prefixes[prefixId];
    auto prefixPath = QString("%1/%2").arg(versionsPath, prefixId);
    auto versions = QDir(prefixPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    foreach (auto versionId, versions) {
        auto versionIndexPath = QString("%1/%2/%2.json").arg(prefixPath, versionId);
        QFile file(versionIndexPath);

        if (!file.open(QIODevice::ReadOnly))
            continue;

        auto versionIndex = VersionIndex(QJsonDocument::fromJson(file.readAll()).object());
        if (versionIndex.id != versionId) {
            auto msg = QString("A version index '%1' contains the wrong version id '%2'");
            log.warning(msg.arg(versionIndexPath, versionIndex.id));
            continue;
        }

        prefix.versions << versionId;
        log.info(QString("Local version is found: '%1/%2'").arg(prefixId, versionId));
    }

    std::sort(prefix.versions.begin(), prefix.versions.end(), std::greater<QString>());

    if (prefix.versions.count() > 1)
        prefix.latestVersionId = prefix.versions[1];
}

void VersionsManager::fetchPrefixes()
{
    if (fetchingPrefixes) {
        log.warning("Failed to start a prefixes fetching! Already in progress!");
        return;
    }

    log.info("Fetching actual prefixes...");
    fetchingPrefixes = true;

    auto reply = makeGetRequest(QString("%1/prefixes.json").arg(storeUrl));

    connect(reply, &QNetworkReply::finished, [=]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            log.error("Failed to get the prefixes index: " + reply->errorString());
            setFetchPrefixesResult(false);
            return;
        }

        auto remoteIndex = PrefixesIndex(QJsonDocument::fromJson(reply->readAll()).object());

        foreach (auto id, remoteIndex.prefixes.keys()) {
            index.prefixes[id] = remoteIndex.prefixes[id];
            prefixFetchQueue << id;

            if (!prefixes.contains(id))
                prefixes.insert(id, Prefix(id, remoteIndex.prefixes[id].about));
        }

        QFile indexFile(indexPath);
        if (indexFile.open(QIODevice::WriteOnly)) {
            indexFile.write(QJsonDocument(index.toJsonObject()).toJson());
        } else {
            log.error("Failed to save the prefixes index file!");
            setFetchPrefixesResult(false);
            return;
        }

        fetchNextPrefixOrFinish();
    });
}

void VersionsManager::fetchNextPrefixOrFinish()
{
    if (prefixFetchQueue.isEmpty()) {
        setFetchPrefixesResult(true);
        return;
    }

    auto prefixId = prefixFetchQueue.dequeue();
    auto reply = makeGetRequest(QString("%1/%2/versions/versions.json").arg(storeUrl, prefixId));

    connect(reply, &QNetworkReply::finished, [=]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            QString msg = "Failed to get the versions index for the prefix '%1': %2";
            log.error(msg.arg(prefixId, reply->errorString()));
            setFetchPrefixesResult(false);
            return;
        }

        auto indexJson = QJsonDocument::fromJson(reply->readAll()).object();
        auto versionsIndex = Json::PrefixVersionsIndex(indexJson);

        Prefix &prefix = prefixes[prefixId];
        prefix.latestVersionId = versionsIndex.latest;

        auto knownVersions = QSet<QString>();
        foreach (auto versionId, prefix.versions) {
            knownVersions << versionId;
        }
        foreach (auto versionId, versionsIndex.versions) {
            if (!knownVersions.contains(versionId))
                prefix.versions << versionId;
        }

        std::sort(prefix.versions.begin(), prefix.versions.end(), std::greater<QString>());

        fetchNextPrefixOrFinish();
    });
}

void VersionsManager::setFetchPrefixesResult(bool result)
{
    if (result) {
        log.info("All prefixes are successfully fetched!");
    }

    prefixFetchQueue.clear();
    fetchingPrefixes = false;

    emit onFetchPrefixesResult(result);
}

void VersionsManager::fetchVersionIndexes(const FullVersionId &version)
{
    if (fetchingVersionIndexes) {
        log.warning("Failed to start a version indexes fetching! Already in progress!");
        return;
    }

    log.info(QString("Fetching actual indexes for the '%1'...").arg(version.toString()));
    fetchingVersionIndexes = true;

    fetchVersionMainIndex(version);
}

void VersionsManager::fetchVersionMainIndex(const FullVersionId &version)
{
    auto locationPattern = QString("%1/%2/%3/%3.json");
    auto reply = makeGetRequest(locationPattern.arg(storeUrl, version.prefix, version.id));

    connect(reply, &QNetworkReply::finished, [=]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            QString msg = "Failed to get the version index '%1': %2";
            log.error(msg.arg(version.toString(), reply->errorString()));
            setFetchVersionIndexesResult(false);
            return;
        }

        QDir().mkpath(QString("%1/%2/%3").arg(versionsPath, version.prefix, version.id));

        QFile file(locationPattern.arg(versionsPath, version.prefix, version.id));
        if (!file.open(QIODevice::WriteOnly)) {
            log.error(QString("Failed to save the version index '%1'").arg(version.toString()));
            setFetchVersionIndexesResult(false);
            return;
        }

        auto data = reply->readAll();
        file.write(data);

        auto versionIndex = VersionIndex(QJsonDocument::fromJson(data).object());
        fetchVersionAssetsIndex(version, versionIndex.assetsIndex);
    });
}

void VersionsManager::fetchVersionAssetsIndex(const FullVersionId &version, const QString &assets)
{
    if (assets.isEmpty()) {
        auto msg = QString("Failed to resolve assets index path for the version '%1'");
        log.error(msg.arg(version.toString()));
        setFetchVersionIndexesResult(false);
        return;
    }

    auto locationPattern = QString("%1/assets/indexes/%2.json");
    auto reply = makeGetRequest(locationPattern.arg(storeUrl, assets));

    connect(reply, &QNetworkReply::finished, [=]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            QString msg = "Failed to get the assets index '%1' (%2): %3";
            log.error(msg.arg(assets, version.toString(), reply->errorString()));
            setFetchVersionIndexesResult(false);
            return;
        }

        QDir().mkpath(QString("%1/assets/indexes").arg(dataPath));

        QFile file(locationPattern.arg(dataPath, assets));
        if (!file.open(QIODevice::WriteOnly)) {
            QString msg = "Failed to save the assets index '%1' (%2)";
            log.error(msg.arg(assets, version.toString()));
            setFetchVersionIndexesResult(false);
            return;
        }

        file.write(reply->readAll());
        fetchVersionDataIndex(version);
    });
}

void VersionsManager::fetchVersionDataIndex(const FullVersionId &version)
{
    auto locationPattern = QString("%1/%2/%3/data.json");
    auto reply = makeGetRequest(locationPattern.arg(storeUrl, version.prefix, version.id));

    connect(reply, &QNetworkReply::finished, [=]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            QString msg = "Failed to get the data index '%1': %2";
            log.error(msg.arg(version.toString(), reply->errorString()));
            setFetchVersionIndexesResult(false);
            return;
        }

        QDir().mkpath(QString("%1/%2/%3").arg(versionsPath, version.prefix, version.id));

        QFile file(locationPattern.arg(versionsPath, version.prefix, version.id));
        if (!file.open(QIODevice::WriteOnly)) {
            log.error(QString("Failed to save the data index '%1'").arg(version.toString()));
            setFetchVersionIndexesResult(false);
            return;
        }

        file.write(reply->readAll());
        setFetchVersionIndexesResult(true);
    });
}

void VersionsManager::setFetchVersionIndexesResult(bool result)
{
    if (result) {
        log.info("All indexes are successfully fetched!");
    }

    fetchingVersionIndexes = false;
    emit onFetchVersionIndexesResult(result);
}

const QHash<QString, Prefix> VersionsManager::getPrefixes() const
{
    return prefixes;
}

QNetworkReply *VersionsManager::makeGetRequest(const QString &url)
{
    log.info(QString("Requesting '%1'...").arg(url));
    auto reply = nam->get(QNetworkRequest(url));
    Utils::Network::createTimeoutTimer(reply);

    return reply;
}

bool VersionsManager::fillVersionFiles(const FullVersionId &version, QList<FileInfo> &files)
{
    log.info(QString("Collecting files for the version '%1'...").arg(version.toString()));

    auto dIndexPath = QString("%1/%2/data.json").arg(versionsPath, version.toString());
    auto dataIndex = loadIndex<Json::DataIndex>(dIndexPath);
    if (!dataIndex.isValid()) {
        log.error("Failed to load data index!");
        return false;
    }

    auto jarLocation = QString("%1/%2/%3/%3.jar");
    auto jarUrl = jarLocation.arg(storeUrl, version.prefix, version.id);
    auto jarPath = jarLocation.arg(versionsPath, version.prefix, version.id);
    files << FileInfo(jarUrl, jarPath, dataIndex.main.hash, dataIndex.main.size);

    auto fileLocation = QString("%1/%2/files/%3");
    foreach (auto fileName, dataIndex.files.keys()) {
        auto url = fileLocation.arg(storeUrl, version.toString(), fileName);
        auto path = fileLocation.arg(versionsPath, version.toString(), fileName);
        auto checkInfo = dataIndex.files[fileName];
        files << FileInfo(url, path, checkInfo.hash, checkInfo.size);
    }

    auto vIndexPath = QString("%1/%2/%3/%3.json").arg(versionsPath, version.prefix, version.id);
    auto versionIndex = loadIndex<Json::VersionIndex>(vIndexPath);
    if (!versionIndex.isValid()) {
        log.error("Failed to load version index!");
        return false;
    }

    auto libLocation = QString("%1/libraries/%2");
    foreach (auto libInfo, versionIndex.libraries) {
        if (!Utils::Platform::isLibraryAllowed(libInfo))
            continue;

        auto libPath = Utils::Platform::getLibraryPath(libInfo);
        if (!dataIndex.libs.contains(libPath)) {
            log.warning(QString("Library '%1' is missing in the data index").arg(libPath));
            continue;
        }

        auto url = libLocation.arg(storeUrl, libPath);
        auto path = libLocation.arg(dataPath, libPath);
        auto checkInfo = dataIndex.libs[libPath];
        files << FileInfo(url, path, checkInfo.hash, checkInfo.size);
    }

    auto aIndexPath = QString("%1/assets/indexes/%2.json").arg(dataPath, versionIndex.assetsIndex);
    auto assetsIndex = loadIndex<Json::AssetsIndex>(aIndexPath);
    if (!assetsIndex.isValid()) {
        log.error("Failed to load version index!");
        return false;
    }

    auto assetLocation = QString("%1/assets/objects/%2");
    foreach (auto asset, assetsIndex.objects) {
        auto name = QString("%1/%2").arg(asset.hash.mid(0, 2), asset.hash);
        auto url = assetLocation.arg(storeUrl, name);
        auto path = assetLocation.arg(dataPath, name);
        files << FileInfo(url, path, asset.hash, asset.size);
    }

    log.info(QString("Need to check %1 files").arg(QString::number(files.count())));
    return true;
}

template<typename T>
const T VersionsManager::loadIndex(const QString &path)
{
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    return T(QJsonDocument::fromJson(file.readAll()).object());
}

}
}
