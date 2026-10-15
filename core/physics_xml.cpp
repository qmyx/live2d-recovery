#include "physics_xml.h"
#include <QJsonArray>
#include <QSet>
#include <QUuid>
#include <QXmlStreamWriter>
#include <cmath>
#include <stdexcept>

namespace recovery {
namespace {
[[noreturn]] void bad(const QString& detail) {
    throw std::runtime_error((QStringLiteral("物理配置：") + detail).toUtf8().constData());
}
double value(const QJsonObject& o, const QString& key) {
    const auto v = o.value(key);
    if (!v.isDouble() || !std::isfinite(v.toDouble())) bad(QStringLiteral("缺少有效数值 %1。").arg(key));
    return v.toDouble();
}
int whole(const QJsonObject& o, const QString& key, int maximum) {
    const double v = value(o, key);
    if (v < 0 || v > maximum || std::floor(v) != v) bad(QStringLiteral("整数 %1 超出范围。").arg(key));
    return int(v);
}
void begin(QXmlStreamWriter& w, const QString& tag, const QString& field = {}) {
    w.writeStartElement(tag);
    if (!field.isEmpty()) w.writeAttribute(QStringLiteral("xs.n"), field);
}
void scalar(QXmlStreamWriter& w, const QString& tag, const QString& field, const QString& v) {
    begin(w, tag, field); w.writeCharacters(v); w.writeEndElement();
}
void number(QXmlStreamWriter& w, const QString& field, double v) {
    scalar(w, QStringLiteral("f"), field, QString::number(v, 'g', 10));
}
void vector(QXmlStreamWriter& w, const QString& field, double x, double y) {
    begin(w, QStringLiteral("GVector2"), field);
    number(w, QStringLiteral("x"), x); number(w, QStringLiteral("y"), y); w.writeEndElement();
}
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
void guid(QXmlStreamWriter& w, const QString& tag, const QString& field, const QString& id) {
    begin(w, tag, field); w.writeAttribute(QStringLiteral("uuid"), id);
    w.writeAttribute(QStringLiteral("note"), QString()); w.writeEndElement();
}
void array(QXmlStreamWriter& w, const QString& field, int count) {
    begin(w, QStringLiteral("carray_list"), field); w.writeAttribute(QStringLiteral("count"), QString::number(count));
}
void parameter(QXmlStreamWriter& w, const QString& field, const QJsonObject& source,
               const QHash<QString, QString>& references) {
    const QString id = source.value(QStringLiteral("Id")).toString();
    if (source.value(QStringLiteral("Target")).toString() != QStringLiteral("Parameter") || !references.contains(id))
        bad(QStringLiteral("输入或输出没有对应的模型参数。"));
    begin(w, QStringLiteral("CParameterGuid"), field);
    w.writeAttribute(QStringLiteral("xs.ref"), references.value(id)); w.writeEndElement();
}
void typeAndWeight(QXmlStreamWriter& w, const QJsonObject& item) {
    const double weight = value(item, QStringLiteral("Weight"));
    if (weight < 0 || weight > 100) bad(QStringLiteral("权重必须位于 0 到 100。"));
    number(w, QStringLiteral("weight"), weight);
    const QString kind = item.value(QStringLiteral("Type")).toString();
    const QString mode = kind == QStringLiteral("X") ? QStringLiteral("SRC_TO_X") :
                         kind == QStringLiteral("Y") ? QStringLiteral("SRC_TO_Y") :
                         kind == QStringLiteral("Angle") ? QStringLiteral("SRC_TO_G_ANGLE") : QString();
    if (mode.isEmpty()) bad(QStringLiteral("尚未识别的物理输入类型。"));
    begin(w, QStringLiteral("CPhysicsSourceType"), QStringLiteral("type"));
    w.writeAttribute(QStringLiteral("v"), mode); w.writeEndElement();
    const auto reflect = item.value(QStringLiteral("Reflect"));
    if (!reflect.isBool()) bad(QStringLiteral("缺少反向标志。"));
    scalar(w, QStringLiteral("b"), QStringLiteral("isReverse"), reflect.toBool() ? QStringLiteral("true") : QStringLiteral("false"));
}
}

QStringList physicsXmlImports() {
    return {QStringLiteral("com.live2d.cubism.doc.gameData.physics.CPhysicsController$CPhysicsSourceType"),
            QStringLiteral("com.live2d.cubism.doc.gameData.physics.CPhysicsInput"),
            QStringLiteral("com.live2d.cubism.doc.gameData.physics.CPhysicsOutput"),
            QStringLiteral("com.live2d.cubism.doc.gameData.physics.CPhysicsSettingsSource"),
            QStringLiteral("com.live2d.cubism.doc.gameData.physics.CPhysicsSettingsSourceSet"),
            QStringLiteral("com.live2d.cubism.doc.gameData.physics.CPhysicsVertex"),
            QStringLiteral("com.live2d.cubism.doc.model.id.CPhysicsSettingId"),
            QStringLiteral("com.live2d.graphics3d.type.GVector2"),
            QStringLiteral("com.live2d.type.CPhysicsDataGuid"),
            QStringLiteral("com.live2d.type.CPhysicsSettingsGuid")};
}

QByteArray physicsXml(const QJsonObject& physics, const QHash<QString, QString>& references) {
    const bool present = !physics.isEmpty();
    if (present && physics.value(QStringLiteral("Version")).toInt() != 3) bad(QStringLiteral("只验证过格式 3。"));
    if (present && (!physics.value(QStringLiteral("Meta")).isObject() ||
                    !physics.value(QStringLiteral("PhysicsSettings")).isArray()))
        bad(QStringLiteral("Meta 必须为对象，PhysicsSettings 必须为数组。"));
    const auto meta = physics.value(QStringLiteral("Meta")).toObject();
    const auto settings = physics.value(QStringLiteral("PhysicsSettings")).toArray();
    if (settings.size() > 10000) bad(QStringLiteral("设置数量过大。"));
    int fps = 60;
    if (meta.contains(QStringLiteral("Fps"))) {
        fps = whole(meta, QStringLiteral("Fps"), 1000);
        if (!fps) bad(QStringLiteral("帧率不能为零。"));
    }
    const auto forces = meta.value(QStringLiteral("EffectiveForces")).toObject();
    if (!forces.isEmpty()) {
        const auto gravity = forces.value(QStringLiteral("Gravity")).toObject();
        const auto wind = forces.value(QStringLiteral("Wind")).toObject();
        if (value(gravity, QStringLiteral("X")) != 0 || value(gravity, QStringLiteral("Y")) != -1 ||
            value(wind, QStringLiteral("X")) != 0 || value(wind, QStringLiteral("Y")) != 0)
            bad(QStringLiteral("自定义重力或风的工程映射尚未验证。"));
    }
    QHash<QString, QString> names;
    for (const auto& entry : meta.value(QStringLiteral("PhysicsDictionary")).toArray()) {
        const auto item = entry.toObject(); names.insert(item.value(QStringLiteral("Id")).toString(), item.value(QStringLiteral("Name")).toString());
    }
    QByteArray xml;
    QXmlStreamWriter w(&xml);
    begin(w, QStringLiteral("CPhysicsSettingsSourceSet"), QStringLiteral("physicsSettingsSourceSet"));
    array(w, QStringLiteral("_sourceCubismPhysics"), settings.size());
    QString firstGuid;
    QSet<QString> ids;
    for (const auto& item : settings) {
        if (!item.isObject()) bad(QStringLiteral("设置条目不是对象。"));
        const auto setting = item.toObject();
        const QString id = setting.value(QStringLiteral("Id")).toString();
        if (id.isEmpty() || ids.contains(id)) bad(QStringLiteral("设置 ID 缺失或重复。"));
        ids.insert(id);
        for (const QString& field : {QStringLiteral("Input"), QStringLiteral("Output"), QStringLiteral("Vertices")})
            if (!setting.value(field).isArray()) bad(QStringLiteral("%1 必须为数组。").arg(field));
        const auto vertices = setting.value(QStringLiteral("Vertices")).toArray();
        if (vertices.isEmpty() || vertices.size() > 100000) bad(QStringLiteral("物理顶点数量无效。"));
        begin(w, QStringLiteral("CPhysicsSettingsSource"));
        scalar(w, QStringLiteral("s"), QStringLiteral("name"), names.value(id, id));
        const QString groupGuid = uuid(); if (firstGuid.isEmpty()) firstGuid = groupGuid;
        guid(w, QStringLiteral("CPhysicsSettingsGuid"), QStringLiteral("guid"), groupGuid);
        begin(w, QStringLiteral("CPhysicsSettingId"), QStringLiteral("id"));
        w.writeAttribute(QStringLiteral("idstr"), id); w.writeEndElement();
        const auto inputs = setting.value(QStringLiteral("Input")).toArray();
        array(w, QStringLiteral("inputs"), inputs.size());
        for (const auto& entry : inputs) {
            const auto input = entry.toObject();
            begin(w, QStringLiteral("CPhysicsInput"));
            guid(w, QStringLiteral("CPhysicsDataGuid"), QStringLiteral("guid"), uuid());
            parameter(w, QStringLiteral("source"), input.value(QStringLiteral("Source")).toObject(), references);
            number(w, QStringLiteral("angleScale"), 0);
            vector(w, QStringLiteral("translationScale"), 0, 0);
            typeAndWeight(w, input); w.writeEndElement();
        }
        w.writeEndElement();
        const auto outputs = setting.value(QStringLiteral("Output")).toArray();
        array(w, QStringLiteral("outputs"), outputs.size());
        for (const auto& entry : outputs) {
            const auto output = entry.toObject();
            if (output.value(QStringLiteral("Type")).toString() != QStringLiteral("Angle"))
                bad(QStringLiteral("非 Angle 输出的工程映射尚未验证。"));
            const int vertex = whole(output, QStringLiteral("VertexIndex"), vertices.size() - 1);
            begin(w, QStringLiteral("CPhysicsOutput"));
            guid(w, QStringLiteral("CPhysicsDataGuid"), QStringLiteral("guid"), uuid());
            parameter(w, QStringLiteral("destination"), output.value(QStringLiteral("Destination")).toObject(), references);
            scalar(w, QStringLiteral("i"), QStringLiteral("vertexIndex"), QString::number(vertex));
            vector(w, QStringLiteral("translationScale"), 0, 0);
            number(w, QStringLiteral("angleScale"), value(output, QStringLiteral("Scale")));
            typeAndWeight(w, output); w.writeEndElement();
        }
        w.writeEndElement();
        array(w, QStringLiteral("vertices"), vertices.size());
        for (const auto& entry : vertices) {
            const auto v = entry.toObject(); const auto position = v.value(QStringLiteral("Position")).toObject();
            begin(w, QStringLiteral("CPhysicsVertex"));
            guid(w, QStringLiteral("CPhysicsDataGuid"), QStringLiteral("guid"), uuid());
            vector(w, QStringLiteral("position"), value(position, QStringLiteral("X")), value(position, QStringLiteral("Y")));
            number(w, QStringLiteral("mobility"), value(v, QStringLiteral("Mobility")));
            number(w, QStringLiteral("delay"), value(v, QStringLiteral("Delay")));
            number(w, QStringLiteral("acceleration"), value(v, QStringLiteral("Acceleration")));
            number(w, QStringLiteral("radius"), value(v, QStringLiteral("Radius")));
            w.writeEndElement();
        }
        w.writeEndElement();
        const auto normalization = setting.value(QStringLiteral("Normalization")).toObject();
        for (const QString& axis : {QStringLiteral("Position"), QStringLiteral("Angle")}) {
            const auto range = normalization.value(axis).toObject();
            const double low = value(range, QStringLiteral("Minimum"));
            const double high = value(range, QStringLiteral("Maximum"));
            const double initial = value(range, QStringLiteral("Default"));
            if (low > high || initial < low || initial > high) bad(QStringLiteral("归一化范围无效。"));
            number(w, QStringLiteral("normalized%1ValueMax").arg(axis), high);
            number(w, QStringLiteral("normalized%1ValueMin").arg(axis), low);
            number(w, QStringLiteral("normalized%1DefaultValue").arg(axis), initial);
        }
        w.writeEndElement();
    }
    w.writeEndElement();
    guid(w, QStringLiteral("CPhysicsSettingsGuid"), QStringLiteral("selectedCubismPhysics"), firstGuid.isEmpty() ? uuid() : firstGuid);
    scalar(w, QStringLiteral("i"), QStringLiteral("settingFPS"), QString::number(fps));
    w.writeEndElement();
    if (w.hasError()) bad(QStringLiteral("无法编码工程数据。"));
    return xml;
}
}
