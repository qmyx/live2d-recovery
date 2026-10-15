#include "moc_reader.h"
#include "physics_xml.h"
#include "display_xml.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace recovery {
namespace {

[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toUtf8().constData());
}

QByteArray readBytes(const QString& path, qint64 maximum) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("无法读取文件：%1").arg(QFileInfo(path).fileName()));
    if (file.size() < 1 || file.size() > maximum) fail(QStringLiteral("文件大小超出支持范围。"));
    const QByteArray bytes = file.readAll();
    if (bytes.size() != file.size()) fail(QStringLiteral("文件读取不完整。"));
    return bytes;
}

QJsonObject readJson(const QString& path) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(readBytes(path, 32 * 1024 * 1024), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        fail(QStringLiteral("JSON 文件格式不正确：%1").arg(QFileInfo(path).fileName()));
    return doc.object();
}

// The reader uses a bounded section directory. The MOC layout is structure of
// arrays: entries are file offsets, and indexes inside sections are element
// indexes (not addresses). No input bytes are executed or loaded as a library.
class Moc {
public:
    explicit Moc(QByteArray bytes) : data(std::move(bytes)) {
        if (data.size() < 0x2a0 || data.first(4) != "MOC3") fail(QStringLiteral("不是有效的 MOC3 文件。"));
        if (quint8(data[4]) != 5) fail(QStringLiteral("当前独立解析器仅验证过 MOC3 格式 5。"));
        if (quint8(data[5]) != 0) fail(QStringLiteral("暂不支持大端序 MOC3 文件。"));
        offsets.reserve(152);
        for (int i = 0; i < 152; ++i) {
            const auto value = rawU32(64 + 4 * i);
            if (value < 0x2a0 || value > quint64(data.size()) || (value & 3))
                fail(QStringLiteral("MOC3 数据段位置无效。"));
            if (!offsets.isEmpty() && value < offsets.last()) fail(QStringLiteral("MOC3 数据段顺序无效。"));
            offsets.append(value);
        }
        for (int i = 0; i < 35; ++i) {
            const int value = integer(0, i);
            if (value < 0 || value > 16000000) fail(QStringLiteral("MOC3 数据数量无效。"));
            counts.append(value);
        }
        if (counts[1] != counts[2] + counts[3]) fail(QStringLiteral("变形器数量不一致。"));
        if (counts[0] > 100000 || counts[1] > 100000 || counts[4] > 100000 || counts[5] > 100000)
            fail(QStringLiteral("模型对象数量超出支持范围。"));
    }

    void range(int section, qint64 first, qint64 count, int size) const {
        if (section < 0 || section >= offsets.size() || first < 0 || count < 0 || size < 1)
            fail(QStringLiteral("MOC3 数组范围无效。"));
        const quint64 end = section + 1 < offsets.size() ? offsets[section + 1] : quint64(data.size());
        const quint64 capacity = (end - offsets[section]) / size;
        if (quint64(first) > capacity || quint64(count) > capacity - quint64(first))
            fail(QStringLiteral("MOC3 数据段 %1 的数组越界。 ").arg(section));
    }
    int integer(int section, qint64 index) const {
        range(section, index, 1, 4);
        return qint32(rawU32(offsets[section] + index * 4));
    }
    float number(int section, qint64 index) const {
        const quint32 bits = quint32(integer(section, index));
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value)) fail(QStringLiteral("MOC3 包含非有限浮点数。"));
        return value;
    }
    quint8 byte(int section, qint64 index) const {
        range(section, index, 1, 1);
        return quint8(data[qsizetype(offsets[section] + index)]);
    }
    quint16 index16(int section, qint64 index) const {
        range(section, index, 1, 2);
        return qFromLittleEndian<quint16>(data.constData() + offsets[section] + index * 2);
    }
    QString id(int section, int index) const {
        range(section, index, 1, 64);
        const auto value = data.mid(offsets[section] + qint64(index) * 64, 64);
        const int zero = value.indexOf('\0');
        if (zero < 1) fail(QStringLiteral("MOC3 对象标识符无效。"));
        return QString::fromUtf8(value.first(zero));
    }
    void index(int value, int count, bool optional = false) const {
        if ((optional && value == -1) || (value >= 0 && value < count)) return;
        fail(QStringLiteral("MOC3 对象引用超出范围。"));
    }
    int count(int i) const { return counts.at(i); }
    QVector<QPointF> points(int start, int count) const {
        if (start < 0 || count < 0 || qint64(start) + qint64(count) * 2 > counts[10])
            fail(QStringLiteral("关键形顶点范围无效。"));
        requestedPoints += quint64(count);
        if (requestedPoints > 16000000) fail(QStringLiteral("模型解码后的顶点总量超出支持范围。"));
        range(71, start, qint64(count) * 2, 4);
        QVector<QPointF> result;
        result.reserve(count);
        for (int i = 0; i < count; ++i)
            result.append(QPointF(number(71, qint64(start) + 2 * i), number(71, qint64(start) + 2 * i + 1)));
        return result;
    }
private:
    quint32 rawU32(qint64 offset) const {
        if (offset < 0 || offset > data.size() - 4) fail(QStringLiteral("MOC3 文件被截断。"));
        return qFromLittleEndian<quint32>(data.constData() + offset);
    }
    QByteArray data;
    QVector<quint32> offsets;
    QVector<int> counts;
    mutable quint64 requestedPoints = 0;
};

void validateTree(const QVector<int>& parents) {
    QVector<quint8> state(parents.size(), 0);
    for (int i = 0; i < parents.size(); ++i) {
        int current = i;
        QVector<int> path;
        while (current != -1 && state[current] == 0) {
            state[current] = 1;
            path.append(current);
            current = parents[current];
        }
        if (current != -1 && state[current] == 1) fail(QStringLiteral("模型父级引用存在循环。"));
        for (int item : path) state[item] = 2;
    }
}

void uniqueId(QSet<QString>& ids, const QString& id) {
    if (ids.contains(id)) fail(QStringLiteral("模型包含重复对象标识符。"));
    ids.insert(id);
}

} // namespace

Model readModel(const QString& modelJson) {
    Model model;
    model.inputFile = QFileInfo(modelJson).absoluteFilePath();
    const auto manifest = readJson(model.inputFile);
    const auto references = manifest.value(QStringLiteral("FileReferences")).toObject();
    const QDir directory(QFileInfo(model.inputFile).absolutePath());
    const QString mocName = references.value(QStringLiteral("Moc")).toString();
    if (mocName.isEmpty()) fail(QStringLiteral("模型描述文件缺少 Moc 引用。"));
    model.displayName = QFileInfo(mocName).completeBaseName();
    const auto textures = references.value(QStringLiteral("Textures")).toArray();
    if (textures.isEmpty() || textures.size() > 1024) fail(QStringLiteral("模型贴图列表为空或超出范围。"));
    for (const auto& texture : textures) {
        const QString name = texture.toString();
        const QString path = directory.absoluteFilePath(name);
        if (name.isEmpty() || !QFileInfo(path).isFile()) fail(QStringLiteral("模型引用的贴图不存在。"));
        model.textures.append(QFileInfo(path).absoluteFilePath());
    }
    QHash<QString, QString> displayNames;
    const QString displayInfo = references.value(QStringLiteral("DisplayInfo")).toString();
    if (!displayInfo.isEmpty()) {
        const auto cdi = readJson(directory.absoluteFilePath(displayInfo));
        model.displayInfo = cdi;
        for (const QString key : {QStringLiteral("Parameters"), QStringLiteral("Parts")}) {
            for (const auto& value : cdi.value(key).toArray()) {
                const auto item = value.toObject();
                const QString id = item.value(QStringLiteral("Id")).toString();
                const QString name = item.value(QStringLiteral("Name")).toString();
                if (!id.isEmpty() && !name.isEmpty()) displayNames.insert(id, name);
            }
        }
    }
    const QString physics = references.value(QStringLiteral("Physics")).toString();
    if (!physics.isEmpty()) model.physics = readJson(directory.absoluteFilePath(physics));
    for (const QString key : {QStringLiteral("UserData"), QStringLiteral("MotionSync")}) {
        const auto value = references.value(key);
        if (value.isUndefined() || value.isNull()) continue;
        if (!value.isString()) fail(QStringLiteral("模型附属文件 %1 的引用不是路径。 ").arg(key));
        if (!value.toString().isEmpty()) model.unsupported.append(QStringLiteral("%1 附属数据尚未转换为工程字段").arg(key));
    }
    if (references.contains(QStringLiteral("Motions"))) {
        const auto value = references.value(QStringLiteral("Motions"));
        if (!value.isObject()) fail(QStringLiteral("动作文件列表格式无效。"));
        if (!value.toObject().isEmpty()) model.observations.append(QStringLiteral("动作文件是独立资源，不转换为 CMO3；请保留并单独使用对应文件。"));
    }
    if (references.contains(QStringLiteral("Expressions"))) {
        const auto value = references.value(QStringLiteral("Expressions"));
        if (!value.isArray()) fail(QStringLiteral("表情文件列表格式无效。"));
        if (!value.toArray().isEmpty()) model.observations.append(QStringLiteral("表情文件是独立资源，不转换为 CMO3；请保留并单独使用对应文件。"));
    }
    if (references.contains(QStringLiteral("Pose"))) {
        const auto value = references.value(QStringLiteral("Pose"));
        if (!value.isString()) fail(QStringLiteral("姿势文件引用格式无效。"));
        if (!value.toString().isEmpty()) model.observations.append(QStringLiteral("姿势切换配置是独立资源，不转换为 CMO3；请保留并单独使用对应文件。"));
    }
    const auto displayName = [&displayNames](const QString& id) { return displayNames.value(id, id); };
    const Moc moc(readBytes(directory.absoluteFilePath(mocName), 512ll * 1024 * 1024));
    model.pixelsPerUnit = moc.number(1, 0);
    model.canvasOrigin = QPointF(moc.number(1, 1), moc.number(1, 2));
    model.canvasWidth = moc.number(1, 3);
    model.canvasHeight = moc.number(1, 4);
    if (model.pixelsPerUnit <= 0 || model.canvasWidth <= 0 || model.canvasHeight <= 0)
        fail(QStringLiteral("模型画布信息无效。"));

    QSet<QString> ids;
    QVector<int> bindingOwners(moc.count(13), -1);
    for (int i = 0; i < moc.count(5); ++i) {
        Parameter item;
        item.id = moc.id(50, i);
        uniqueId(ids, item.id);
        item.name = displayName(item.id);
        item.maximum = moc.number(51, i);
        item.minimum = moc.number(52, i);
        item.initial = moc.number(53, i);
        item.type = moc.integer(114, i);
        const int repeat = moc.integer(54, i);
        if (repeat != 0 && repeat != 1) fail(QStringLiteral("参数循环标志无效。"));
        item.repeat = repeat != 0;
        // This field has only been verified with the standard precision code.
        if (moc.integer(55, i) != 3)
            fail(QStringLiteral("参数包含尚未验证的精度编码。"));
        if (item.minimum > item.maximum || item.initial < item.minimum || item.initial > item.maximum)
            fail(QStringLiteral("模型参数范围无效。"));
        const int begin = moc.integer(56, i), length = moc.integer(57, i);
        if (length < 0 || length > moc.count(13) || (length > 0 && (begin < 0 || begin > moc.count(13) - length)))
            fail(QStringLiteral("参数键值引用无效。"));
        for (int j = 0; j < length; ++j) {
            if (bindingOwners[begin + j] != -1) fail(QStringLiteral("参数键值重复归属。"));
            bindingOwners[begin + j] = i;
        }
        model.parameters.append(item);
    }
    if (manifest.contains(QStringLiteral("Groups"))) {
        if (!manifest.value(QStringLiteral("Groups")).isArray()) fail(QStringLiteral("效果参数组格式无效。"));
        QSet<QString> groupNames;
        for (const auto& value : manifest.value(QStringLiteral("Groups")).toArray()) {
            if (!value.isObject()) fail(QStringLiteral("效果参数组条目不是对象。"));
            const auto group = value.toObject();
            const QString name = group.value(QStringLiteral("Name")).toString();
            if (name.isEmpty() || groupNames.contains(name)) fail(QStringLiteral("效果参数组名称缺失或重复。"));
            groupNames.insert(name);
            if (group.value(QStringLiteral("Target")).toString() != QStringLiteral("Parameter"))
                fail(QStringLiteral("效果参数组必须引用 Parameter。"));
            if (!group.value(QStringLiteral("Ids")).isArray()) fail(QStringLiteral("效果参数组缺少参数列表。"));
            QSet<QString> parameters;
            for (const auto& parameter : group.value(QStringLiteral("Ids")).toArray()) {
                if (!parameter.isString() || !ids.contains(parameter.toString()) || parameters.contains(parameter.toString()))
                    fail(QStringLiteral("效果参数组包含无效或重复的参数引用。"));
                parameters.insert(parameter.toString());
            }
            if (name != QStringLiteral("EyeBlink") && name != QStringLiteral("LipSync"))
                model.unsupported.append(QStringLiteral("尚未支持的效果参数组：%1").arg(name));
            model.effectGroups.append(group);
        }
    }
    if (!model.physics.isEmpty()) {
        QHash<QString, QString> referencesForValidation;
        for (const auto& parameter : model.parameters) referencesForValidation.insert(parameter.id, parameter.id);
        try {
            physicsXml(model.physics, referencesForValidation);
        } catch (const std::exception& error) {
            model.unsupported.append(QString::fromUtf8(error.what()));
        }
    }
    const auto bindings = [&](int group, int expectedForms) {
        moc.index(group, moc.count(12));
        const int first = moc.integer(73, group), length = moc.integer(74, group);
        if (first < 0 || length < 0 || first > moc.count(11) - length || length > 16)
            fail(QStringLiteral("关键形参数组范围无效。"));
        QVector<Binding> result;
        qint64 product = 1;
        QSet<int> used;
        for (int i = 0; i < length; ++i) {
            const int slot = moc.integer(72, first + i);
            moc.index(slot, moc.count(13));
            Binding binding;
            binding.parameter = bindingOwners[slot];
            moc.index(binding.parameter, model.parameters.size());
            if (used.contains(binding.parameter)) fail(QStringLiteral("关键形重复绑定同一参数。"));
            used.insert(binding.parameter);
            const int start = moc.integer(75, slot), count = moc.integer(76, slot);
            if (start < 0 || count < 1 || start > moc.count(14) - count)
                fail(QStringLiteral("参数键值范围无效。"));
            for (int j = 0; j < count; ++j) {
                const float key = moc.number(77, start + j);
                if (!binding.keys.isEmpty() && key <= binding.keys.last()) fail(QStringLiteral("参数键值未严格递增。"));
                binding.keys.append(key);
            }
            product *= count;
            if (product > 16000000) fail(QStringLiteral("关键形组合数量超出范围。"));
            result.append(binding);
        }
        if (product != expectedForms) fail(QStringLiteral("关键形数量与参数键值组合不一致。"));
        return result;
    };
    const auto colors = [&](Form& form, int map, int index) {
        const int multiply = moc.integer(map, index), screen = moc.integer(map + 1, index);
        moc.index(multiply, moc.count(23));
        moc.index(screen, moc.count(24));
        for (int channel = 0; channel < 3; ++channel) {
            form.multiplyColor[channel] = moc.number(108 + channel, multiply);
            form.screenColor[channel] = moc.number(111 + channel, screen);
        }
    };

    ids.clear();
    QVector<int> parents;
    for (int i = 0; i < moc.count(0); ++i) {
        Part part;
        part.id = moc.id(3, i);
        uniqueId(ids, part.id);
        part.name = displayName(part.id);
        part.parent = moc.integer(9, i);
        moc.index(part.parent, moc.count(0), true);
        parents.append(part.parent);
        model.parts.append(part);
        if (moc.integer(4, i) != 0 && !model.unsupported.contains(QStringLiteral("部件关键形")))
            model.unsupported.append(QStringLiteral("部件关键形"));
    }
    validateTree(parents);

    ids.clear();
    parents.clear();
    QSet<int> warpIndexes, rotationIndexes;
    for (int i = 0; i < moc.count(1); ++i) {
        Deformer item;
        item.id = moc.id(11, i);
        uniqueId(ids, item.id);
        item.name = displayName(item.id);
        item.part = moc.integer(15, i);
        item.parent = moc.integer(16, i);
        moc.index(item.part, model.parts.size(), true);
        moc.index(item.parent, moc.count(1), true);
        parents.append(item.parent);
        const int type = moc.integer(17, i), local = moc.integer(18, i);
        if (moc.integer(102, i) != 0)
            fail(QStringLiteral("变形器包含尚未验证的扩展标志。"));
        if (type == 0) {
            item.kind = Deformer::Kind::Warp;
            moc.index(local, moc.count(2));
            if (moc.integer(101, local) != 1)
                fail(QStringLiteral("曲面变形器包含尚未验证的变换模式。"));
            if (warpIndexes.contains(local)) fail(QStringLiteral("变形器索引重复。"));
            warpIndexes.insert(local);
            const int begin = moc.integer(20, local), count = moc.integer(21, local);
            if (begin < 0 || count < 1 || begin > moc.count(7) - count) fail(QStringLiteral("曲面关键形范围无效。"));
            const int points = moc.integer(22, local);
            item.rows = moc.integer(23, local);
            item.columns = moc.integer(24, local);
            if (item.rows < 1 || item.columns < 1 || item.rows > 1024 || item.columns > 1024 ||
                qint64(item.rows + 1) * (item.columns + 1) != points) fail(QStringLiteral("曲面网格维数无效。"));
            item.bindings = bindings(moc.integer(19, local), count);
            item.forms.reserve(count);
            for (int j = 0; j < count; ++j) {
                Form form;
                form.opacity = moc.number(59, begin + j);
                form.points = moc.points(moc.integer(60, begin + j), points);
                colors(form, 137, begin + j);
                item.forms.append(std::move(form));
            }
        } else if (type == 1) {
            item.kind = Deformer::Kind::Rotation;
            moc.index(local, moc.count(3));
            if (rotationIndexes.contains(local)) fail(QStringLiteral("变形器索引重复。"));
            rotationIndexes.insert(local);
            const int begin = moc.integer(26, local), count = moc.integer(27, local);
            if (begin < 0 || count < 1 || begin > moc.count(8) - count) fail(QStringLiteral("旋转关键形范围无效。"));
            item.baseAngle = moc.number(28, local);
            item.bindings = bindings(moc.integer(25, local), count);
            for (int j = 0; j < count; ++j) {
                Form form;
                form.opacity = moc.number(61, begin + j);
                form.angle = moc.number(62, begin + j);
                form.origin = QPointF(moc.number(63, begin + j), moc.number(64, begin + j));
                form.scale = moc.number(65, begin + j);
                const int rx = moc.integer(66, begin + j), ry = moc.integer(67, begin + j);
                if ((rx != 0 && rx != 1) || (ry != 0 && ry != 1)) fail(QStringLiteral("旋转反射标志无效。"));
                form.reflectX = rx != 0;
                form.reflectY = ry != 0;
                colors(form, 139, begin + j);
                item.forms.append(std::move(form));
            }
        } else fail(QStringLiteral("尚未识别的变形器类型。"));
        model.deformers.append(std::move(item));
    }
    validateTree(parents);

    ids.clear();
    for (int i = 0; i < moc.count(4); ++i) {
        Mesh item;
        item.id = moc.id(33, i);
        uniqueId(ids, item.id);
        item.name = displayName(item.id);
        item.part = moc.integer(39, i);
        item.deformer = moc.integer(40, i);
        item.texture = moc.integer(41, i);
        item.flags = moc.byte(42, i);
        if (item.flags & ~0x0f) fail(QStringLiteral("网格包含尚未支持的绘制标志。"));
        moc.index(item.part, model.parts.size(), true);
        moc.index(item.deformer, model.deformers.size(), true);
        moc.index(item.texture, model.textures.size());
        const int points = moc.integer(43, i), uv = moc.integer(44, i);
        if (points < 0 || points > 65536 || uv < 0 || qint64(uv) + qint64(points) * 2 > moc.count(15))
            fail(QStringLiteral("网格顶点或 UV 范围无效。"));
        for (int j = 0; j < points; ++j) item.uvs.append(QPointF(moc.number(78, uv + j * 2), moc.number(78, uv + j * 2 + 1)));
        const int begin = moc.integer(35, i), count = moc.integer(36, i);
        if (begin < 0 || count < 1 || begin > moc.count(9) - count) fail(QStringLiteral("网格关键形范围无效。"));
        item.bindings = bindings(moc.integer(34, i), count);
        for (int j = 0; j < count; ++j) {
            Form form;
            form.opacity = moc.number(68, begin + j);
            form.drawOrder = moc.number(69, begin + j);
            form.points = moc.points(moc.integer(70, begin + j), points);
            colors(form, 141, begin + j);
            item.forms.append(std::move(form));
        }
        const int triangleBegin = moc.integer(45, i), triangleCount = moc.integer(46, i);
        if (triangleBegin < 0 || triangleCount < 0 || triangleCount % 3 || triangleBegin > moc.count(16) - triangleCount)
            fail(QStringLiteral("三角形索引范围无效。"));
        for (int j = 0; j < triangleCount; ++j) {
            const int vertex = moc.index16(79, triangleBegin + j);
            moc.index(vertex, points);
            item.triangles.append(vertex);
        }
        const int maskBegin = moc.integer(47, i), maskCount = moc.integer(48, i);
        if (maskBegin < 0 || maskCount < 0 || maskBegin > moc.count(17) - maskCount) fail(QStringLiteral("剪贴蒙版范围无效。"));
        for (int j = 0; j < maskCount; ++j) {
            const int mask = moc.integer(80, maskBegin + j);
            moc.index(mask, moc.count(4));
            item.masks.append(mask);
        }
        model.meshes.append(std::move(item));
    }

    if (moc.count(18) == 1 && moc.count(19) == model.meshes.size() &&
        moc.integer(81, 0) == 0 && moc.integer(82, 0) == model.meshes.size()) {
        QSet<int> orderedMeshes;
        bool supportedOrder = true;
        for (int i = 0; i < moc.count(19); ++i) {
            if (moc.integer(86, i) != 0 || moc.integer(88, i) != -1) {
                supportedOrder = false;
                break;
            }
            const int mesh = moc.integer(87, i);
            moc.index(mesh, model.meshes.size());
            if (orderedMeshes.contains(mesh)) fail(QStringLiteral("绘制顺序包含重复网格。"));
            orderedMeshes.insert(mesh);
            model.drawOrderSequence.append(mesh);
        }
        if (!supportedOrder) {
            model.drawOrderSequence.clear();
            model.unsupported.append(QStringLiteral("分组绘制顺序"));
        }
    } else model.unsupported.append(QStringLiteral("多层绘制顺序分组"));

    ids.clear();
    for (int i = 0; i < moc.count(20); ++i) {
        Glue glue;
        glue.id = moc.id(90, i);
        uniqueId(ids, glue.id);
        glue.meshA = moc.integer(94, i);
        glue.meshB = moc.integer(95, i);
        moc.index(glue.meshA, model.meshes.size());
        moc.index(glue.meshB, model.meshes.size());
        const int start = moc.integer(96, i), count = moc.integer(97, i);
        if (start < 0 || count < 0 || count % 2 || start > moc.count(21) - count)
            fail(QStringLiteral("胶水连接的顶点对范围无效。"));
        for (int j = 0; j < count; j += 2) {
            const int a = moc.index16(99, start + j), b = moc.index16(99, start + j + 1);
            moc.index(a, model.meshes[glue.meshA].uvs.size());
            moc.index(b, model.meshes[glue.meshB].uvs.size());
            const float wa = moc.number(98, start + j), wb = moc.number(98, start + j + 1);
            if (wa < 0 || wa > 1 || wb < 0 || wb > 1 || std::abs(wa + wb - 1) > 0.0001f)
                fail(QStringLiteral("胶水连接权重无效。"));
            glue.verticesA.append(a); glue.verticesB.append(b);
            glue.weightsA.append(wa); glue.weightsB.append(wb);
        }
        const int begin = moc.integer(92, i), formCount = moc.integer(93, i);
        if (begin < 0 || formCount < 1 || begin > moc.count(22) - formCount)
            fail(QStringLiteral("胶水关键形范围无效。"));
        glue.bindings = bindings(moc.integer(91, i), formCount);
        for (int j = 0; j < formCount; ++j) glue.intensities.append(moc.number(100, begin + j));
        model.glues.append(std::move(glue));
    }

    for (int i = 0; i < moc.count(30); ++i) {
        BlendConstraint constraint;
        constraint.parameter = moc.integer(132, i);
        moc.index(constraint.parameter, model.parameters.size());
        const int begin = moc.integer(133, i), count = moc.integer(134, i);
        if (begin < 0 || count < 1 || begin > moc.count(31) - count)
            fail(QStringLiteral("叠加关键形约束范围无效。"));
        for (int j = 0; j < count; ++j) {
            const float key = moc.number(135, begin + j);
            const float weight = moc.number(136, begin + j);
            if ((!constraint.keys.isEmpty() && key <= constraint.keys.last()) || weight < 0 || weight > 1)
                fail(QStringLiteral("叠加关键形约束曲线无效。"));
            constraint.keys.append(key); constraint.weights.append(weight);
        }
        model.blendConstraints.append(std::move(constraint));
    }
    QVector<int> blendOwners(moc.count(25), -1);
    for (int i = 0; i < model.parameters.size(); ++i) {
        const int begin = moc.integer(115, i), count = moc.integer(116, i);
        if (begin < 0 || count < 0 || begin > moc.count(25) - count)
            fail(QStringLiteral("叠加参数绑定范围无效。"));
        for (int j = 0; j < count; ++j) {
            if (blendOwners[begin + j] != -1 || model.parameters[i].type != 1)
                fail(QStringLiteral("叠加参数绑定归属无效。"));
            blendOwners[begin + j] = i;
        }
    }
    QVector<int> warpObjects(moc.count(2), -1), rotationObjects(moc.count(3), -1);
    for (int i = 0; i < model.deformers.size(); ++i) {
        const int local = moc.integer(18, i);
        if (model.deformers[i].kind == Deformer::Kind::Warp) warpObjects[local] = i;
        else rotationObjects[local] = i;
    }
    QSet<int> usedTracks;
    const auto decodeTracks = [&](int objectTable, int objectCount, int kind) {
        QSet<int> usedObjects;
        for (int item = 0; item < objectCount; ++item) {
            const int local = moc.integer(objectTable, item);
            int target = local, pointCount = 0;
            if (kind == 0) {
                moc.index(local, warpObjects.size()); target = warpObjects[local];
                pointCount = model.deformers[target].forms.first().points.size();
            } else if (kind == 1) {
                moc.index(local, rotationObjects.size()); target = rotationObjects[local];
            } else {
                moc.index(local, model.meshes.size()); pointCount = model.meshes[target].uvs.size();
            }
            if (usedObjects.contains(local)) fail(QStringLiteral("叠加目标对象重复。"));
            usedObjects.insert(local);
            const int begin = moc.integer(objectTable + 1, item), count = moc.integer(objectTable + 2, item);
            if (begin < 0 || count < 1 || begin > moc.count(26) - count)
                fail(QStringLiteral("叠加目标轨道范围无效。"));
            for (int j = 0; j < count; ++j) {
                const int record = begin + j;
                if (usedTracks.contains(record)) fail(QStringLiteral("叠加轨道重复归属。"));
                usedTracks.insert(record);
                const int slot = moc.integer(120, record);
                moc.index(slot, blendOwners.size());
                BlendTrack track;
                track.parameter = blendOwners[slot];
                moc.index(track.parameter, model.parameters.size());
                const int keyBegin = moc.integer(117, slot), keyCount = moc.integer(118, slot);
                track.neutralKey = moc.integer(119, slot);
                if (keyBegin < 0 || keyCount < 1 || keyBegin > moc.count(14) - keyCount)
                    fail(QStringLiteral("叠加轨道键值范围无效。"));
                moc.index(track.neutralKey, keyCount);
                for (int k = 0; k < keyCount; ++k) {
                    const float key = moc.number(77, keyBegin + k);
                    if (!track.keys.isEmpty() && key <= track.keys.last()) fail(QStringLiteral("叠加轨道键值未严格递增。"));
                    track.keys.append(key);
                }
                const int formBegin = moc.integer(121, record), formCount = moc.integer(122, record);
                const int available = moc.count(kind == 0 ? 7 : kind == 1 ? 8 : 9);
                if (formBegin < 0 || formCount != keyCount || formBegin > available - formCount)
                    fail(QStringLiteral("叠加轨道形态范围无效。"));
                const int constraintBegin = moc.integer(123, record), constraintCount = moc.integer(124, record);
                if (constraintBegin < 0 || constraintCount < 0 || constraintBegin > moc.count(29) - constraintCount)
                    fail(QStringLiteral("叠加轨道约束索引范围无效。"));
                for (int k = 0; k < constraintCount; ++k) {
                    const int constraint = moc.integer(131, constraintBegin + k);
                    moc.index(constraint, model.blendConstraints.size());
                    track.constraints.append(constraint);
                }
                for (int k = 0; k < formCount; ++k) {
                    const int index = formBegin + k;
                    Form form;
                    form.scale = 0;
                    if (kind == 0) {
                        form.opacity = moc.number(59, index);
                        form.points = moc.points(moc.integer(60, index), pointCount);
                        colors(form, 137, index);
                    } else if (kind == 1) {
                        form.opacity = moc.number(61, index);
                        form.angle = moc.number(62, index);
                        form.origin = QPointF(moc.number(63, index), moc.number(64, index));
                        form.scale = moc.number(65, index);
                        if (moc.integer(66, index) || moc.integer(67, index))
                            fail(QStringLiteral("尚未验证叠加旋转的反射变化。"));
                        colors(form, 139, index);
                    } else {
                        form.opacity = moc.number(68, index);
                        form.drawOrder = moc.number(69, index);
                        form.points = moc.points(moc.integer(70, index), pointCount);
                        colors(form, 141, index);
                    }
                    track.deltas.append(std::move(form));
                }
                if (kind == 2) model.meshes[target].blends.append(std::move(track));
                else model.deformers[target].blends.append(std::move(track));
            }
        }
    };
    decodeTracks(125, moc.count(27), 0);
    decodeTracks(128, moc.count(28), 2);
    decodeTracks(146, moc.count(33), 1);
    if (usedTracks.size() != moc.count(26)) fail(QStringLiteral("存在未归属的叠加轨道。"));
    if (moc.count(32) || moc.count(34))
        model.unsupported.append(QStringLiteral("尚未验证的扩展关键形映射"));
    try {
        displayLayout(model.displayInfo, model.parameters);
    } catch (const std::exception& error) {
        model.unsupported.append(QString::fromUtf8(error.what()));
    }
    model.observations.append(QStringLiteral("已校验数据段边界、对象引用、父级循环、关键形组合和三角形索引。"));
    return model;
}

} // namespace recovery
