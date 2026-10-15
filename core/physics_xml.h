#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QStringList>

namespace recovery {
QByteArray physicsXml(const QJsonObject& physics, const QHash<QString, QString>& parameterReferences);
QStringList physicsXmlImports();
}
