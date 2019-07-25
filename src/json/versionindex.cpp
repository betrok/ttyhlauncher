#include <QtCore/QJsonArray>
#include "versionindex.h"
#include "assetdownloadinfo.h"

Ttyh::Json::VersionIndex::VersionIndex(const QJsonObject &jObject)
{
    id = jObject["id"].toString();
    releaseTime = QDateTime::fromString(jObject["releaseTime"].toString(), Qt::ISODate);

    const QString assetIndexKey = "assetIndex";
    if (jObject.contains(assetIndexKey)) {
        auto assetsInfo = AssetDownloadInfo(jObject[assetIndexKey].toObject());
        assetsIndex = QString("%1/%2").arg(assetsInfo.sha1, assetsInfo.id);
    }
    else {
        assetsIndex = jObject["assets"].toString();
    }

    auto jLibraries = jObject["libraries"].toArray();
    foreach(auto jLibraryInfo, jLibraries) {
        libraries << LibraryInfo(jLibraryInfo.toObject());
    }

    mainClass = jObject["mainClass"].toString();

    const QString argumentsKey = "arguments";
    if (jObject.contains(argumentsKey)) {
        auto jArgs = jObject[argumentsKey]["game"].toArray();
        foreach(auto token, jArgs) {
            if (token.isString()) gameArguments << token.toString();
        }
    }
    else {
        gameArguments = jObject["minecraftArguments"].toString().split(' ');
    }
}