#include "project_writer.h"
#include "physics_xml.h"
#include "display_xml.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMap>
#include <QTemporaryFile>
#include <QSet>
#include <QUuid>
#include <QXmlStreamWriter>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace recovery {
namespace {
[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toUtf8().constData()); }
QString number(double v) {
    if (!std::isfinite(v)) fail(QStringLiteral("Project contains a non-finite numeric value."));
    return QString::number(v, 'g', 10);
}
struct Element;
using Node = std::shared_ptr<Element>;
struct Element { QString tag, text; QMap<QString, QString> attrs; QVector<Node> children; };
Node node(const QString& tag, const QString& name = {}) {
    auto n = std::make_shared<Element>(); n->tag = tag;
    if (!name.isEmpty()) n->attrs["xs.n"] = name;
    return n;
}
Node scalar(const QString& tag, const QString& name, const QString& value) {
    auto n = node(tag, name); n->text = value; return n;
}
Node integer(const QString& name, qint64 value) { return scalar("i", name, QString::number(value)); }
Node real(const QString& name, double value) { return scalar("f", name, number(value)); }
Node boolean(const QString& name, bool value) { return scalar("b", name, value ? "true" : "false"); }
Node text(const QString& name, const QString& value = {}) { return scalar("s", name, value); }
Node enumeration(const QString& tag, const QString& name, const QString& value) {
    auto n = node(tag, name); n->attrs["v"] = value; return n;
}
Node list(const QString& name, const QVector<Node>& values = {}, const QString& kind = "carray_list") {
    auto n = node(kind, name); n->attrs["count"] = QString::number(values.size()); n->children = values; return n;
}
Node object(const QString& tag, const QString& name, const QVector<Node>& children) {
    auto n = node(tag, name); n->children = children; return n;
}
Node id(const QString& tag, const QString& name, const QString& value) {
    auto n = node(tag, name); n->attrs["idstr"] = value; return n;
}
Node guid(const QString& tag, const QString& name = {}) {
    auto n = node(tag, name); n->attrs["uuid"] = QUuid::createUuid().toString(QUuid::WithoutBraces); n->attrs["note"] = ""; return n;
}
Node reference(const Node& target, const QString& name = {}) {
    if (!target) return node("null", name);
    if (target->attrs.value("xs.id").isEmpty()) fail("An object reference has no shared identity.");
    auto n = node(target->tag, name); n->attrs["xs.ref"] = target->attrs.value("xs.id"); return n;
}
Node affine(const QString& name) {
    auto n = node("CAffine", name);
    for (const auto& key : {"m00", "m01", "m02", "m10", "m11", "m12"})
        n->attrs[key] = (QString(key) == "m00" || QString(key) == "m11") ? "1.0" : "0.0";
    return n;
}
Node colors(const QString& name, bool white) {
    auto n = node("CFloatColor", name);
    for (const auto& key : {"red", "green", "blue"}) n->attrs[key] = white ? "1.0" : "0.0";
    n->attrs["alpha"] = "1.0"; return n;
}
Node colors(const QString& name, const std::array<float, 3>& values) {
    auto n = node("CFloatColor", name); n->attrs["red"] = number(values[0]); n->attrs["green"] = number(values[1]); n->attrs["blue"] = number(values[2]); n->attrs["alpha"] = "1.0"; return n;
}
Node label() {
    auto n = object("CLabelColor", "labelColor", {enumeration("CLabelColorType", "labelType", "UNDEFINED")});
    n->attrs["customizedColorInt"] = "-1"; return n;
}
Node floats(const QString& name, const QVector<QPointF>& points) {
    QStringList values; values.reserve(points.size() * 2);
    for (const auto& p : points) { values << number(p.x()) << number(p.y()); }
    auto n = scalar("float-array", name, values.join(' ')); n->attrs["count"] = QString::number(values.size()); return n;
}
Node integers(const QString& tag, const QString& name, const QVector<int>& values) {
    QStringList out; out.reserve(values.size()); for (int v : values) out << QString::number(v);
    auto n = scalar(tag, name, out.join(' ')); n->attrs["count"] = QString::number(values.size()); return n;
}
void serialize(QXmlStreamWriter& out, const Node& n) {
    out.writeStartElement(n->tag);
    for (auto it = n->attrs.cbegin(); it != n->attrs.cend(); ++it) out.writeAttribute(it.key(), it.value());
    if (!n->text.isEmpty()) out.writeCharacters(n->text);
    for (const auto& child : n->children) serialize(out, child);
    out.writeEndElement();
}
void checkObjectReferences(const Node& n, const QMap<QString, QString>& identities, bool sharedRoot = false) {
    if (n->attrs.contains("xs.id") && !sharedRoot) fail("A shared object is defined twice in the project graph.");
    const auto target = n->attrs.value("xs.ref");
    if (!target.isEmpty() && identities.value(target) != n->tag) fail("An object reference is missing or has the wrong type.");
    for (const auto& child : n->children) checkObjectReferences(child, identities);
}
Node readElement(QXmlStreamReader& input) {
    auto n = node(input.name().toString());
    for (const auto& attr : input.attributes()) n->attrs[attr.qualifiedName().toString()] = attr.value().toString();
    while (!input.atEnd()) {
        input.readNext();
        if (input.isStartElement()) n->children << readElement(input);
        else if (input.isEndElement()) break;
        else if (input.isCharacters() && !input.isWhitespace()) n->text += input.text();
    }
    return n;
}
Node readFragment(const QByteArray& bytes) {
    QXmlStreamReader reader(bytes); reader.setNamespaceProcessing(false);
    if (!reader.readNextStartElement()) fail("Physics serialization produced no element.");
    auto result = readElement(reader); if (reader.hasError()) fail("Physics serialization produced malformed XML."); return result;
}

// CAFF uses network-order integers. Its directory has two one-byte-length
// UTF-8 strings, an absolute 64-bit offset, a 32-bit byte count and 10 flags.
// A zero mask is valid: it keeps the container readable without obfuscation.
void be32(QByteArray& b, quint32 value) { for (int i = 3; i >= 0; --i) b.append(char(value >> (i * 8))); }
void le16(QByteArray& b, quint16 value) { b.append(char(value)); b.append(char(value >> 8)); }
void le32(QByteArray& b, quint32 value) { le16(b, quint16(value)); le16(b, quint16(value >> 16)); }
quint32 crc32(const QByteArray& b) {
    quint32 result = 0xffffffffU;
    for (unsigned char ch : b) {
        result ^= ch;
        for (int bit = 0; bit < 8; ++bit) result = (result >> 1) ^ ((result & 1U) ? 0xedb88320U : 0U);
    }
    return ~result;
}
QByteArray zipDeflated(const QByteArray& bytes) {
    // Qt provides the general-purpose compressor. ZIP stores its raw DEFLATE
    // stream, without qCompress's length prefix or the zlib header/checksum.
    const QByteArray compressed = qCompress(bytes, 6);
    if (compressed.size() < 10) fail("Cannot compress the project document.");
    const QByteArray payload = compressed.mid(6, compressed.size() - 10);
    const QByteArray name("contents"); const quint32 crc = crc32(bytes);
    QByteArray result; le32(result, 0x04034b50); le16(result, 20); le16(result, 0x800);
    le16(result, 8); le16(result, 0); le16(result, 33); le32(result, crc);
    le32(result, quint32(payload.size())); le32(result, quint32(bytes.size()));
    le16(result, quint16(name.size())); le16(result, 0); result += name; result += payload;
    const quint32 centralOffset = quint32(result.size());
    le32(result, 0x02014b50); le16(result, 20); le16(result, 20); le16(result, 0x800);
    le16(result, 8); le16(result, 0); le16(result, 33); le32(result, crc);
    le32(result, quint32(payload.size())); le32(result, quint32(bytes.size()));
    le16(result, quint16(name.size())); le16(result, 0); le16(result, 0); le16(result, 0);
    le16(result, 0); le32(result, 0); le32(result, 0); result += name;
    const quint32 centralSize = quint32(result.size()) - centralOffset;
    le32(result, 0x06054b50); le16(result, 0); le16(result, 0); le16(result, 1); le16(result, 1);
    le32(result, centralSize); le32(result, centralOffset); le16(result, 0); return result;
}
struct Asset { QByteArray name, type, bytes; quint8 format; };
QByteArray container(const QVector<Asset>& assets) {
    quint64 offset = 58;
    for (const auto& a : assets) offset += 24 + a.name.size() + a.type.size();
    QByteArray b("CAFF"); be32(b, 45); b += QByteArray("---", 3); b += QByteArray(15, '\0');
    b += QByteArray::fromHex("7f7f"); b += QByteArray(26, '\0'); be32(b, quint32(assets.size()));
    for (const auto& a : assets) {
        if (offset + quint64(a.bytes.size()) > 0xffffffffULL) fail("Project exceeds the supported container size.");
        b.append(char(a.name.size())); b += a.name; b.append(char(a.type.size())); b += a.type;
        be32(b, 0); be32(b, quint32(offset)); be32(b, quint32(a.bytes.size()));
        b.append(char(1)); b.append(char(a.format)); b += QByteArray(8, '\0'); offset += a.bytes.size();
    }
    for (const auto& a : assets) b += a.bytes;
    return b;
}

class Project {
public:
    explicit Project(const Model& input) : m(input) {}
    QByteArray build();
private:
    const Model& m;
    QVector<Node> shared, parameterGuids, partGuids, partSources, deformerGuids, deformerSources, meshGuids, meshSources;
    QVector<Node> images, textures, modelImageGuids, glueSources, glueGuids;
    QVector<Node> groupGuids, groupSources;
    DisplayLayout display;
    Node imageGroup, rawImage, rawImageGuid, rawRoot, imageFilter;
    QVector<QSize> textureSizes;
    QVector<Asset> assets;
    Node rootPartGuid, rootPart, rootGroupGuid, rootGroup, rootDeformerGuid, coordCanvas, coordLocal, coordBasic;
    Node share(const Node& n) { n->attrs["xs.id"] = "#" + QString::number(shared.size()); shared << n; return n; }
    Node part(int index) const { return index < 0 ? rootPartGuid : partGuids.value(index); }
    Node deformer(int index) const { return index < 0 ? rootDeformerGuid : deformerGuids.value(index); }
    QVector<QPointF> positions(const QVector<QPointF>& points, int parent) const;
    Node grid(const QVector<Binding>& bindings, const QVector<Node>& formGuids);
    Node controllable(const QString& name, int partIndex, const Node& gridNode, const QVector<Node>& extensions = {});
    Node formBase(const Node& source, const Node& formGuid);
    Node drawForm(const Node& source, const Node& formGuid, const Form& form, int parent, bool drawable);
    void makeParameters();
    void makeParts();
    void makeTextures();
    Node makeImageFilter();
    void makeDeformers();
    void makeMeshes();
    void makeGlues();
    Node effectGroups();
    void addMorphs(const Node& base, const Node& source, const QVector<Binding>& bindings, const QVector<BlendTrack>& blends, QVector<Form>& forms, QVector<Node>& guids);
};

QVector<QPointF> Project::positions(const QVector<QPointF>& points, int parent) const {
    QVector<QPointF> result; result.reserve(points.size());
    for (auto p : points) {
        if (parent < 0) p = {p.x() * m.pixelsPerUnit + m.canvasOrigin.x(), p.y() * m.pixelsPerUnit + m.canvasOrigin.y()};
        result << p;
    }
    return result;
}
Node Project::grid(const QVector<Binding>& bindings, const QVector<Node>& formGuids) {
    auto g = share(node("KeyformGridSource")); QVector<Node> bound;
    quint64 combinations = 1;
    for (const auto& binding : bindings) {
        if (binding.parameter < 0 || binding.parameter >= parameterGuids.size() || binding.keys.isEmpty()) fail("Invalid parameter binding.");
        combinations *= binding.keys.size();
        auto b = share(node("KeyformBindingSource")); QVector<Node> values;
        for (float key : binding.keys) values << real({}, key);
        b->children = {reference(g, "_gridSource"), reference(parameterGuids[binding.parameter], "parameterGuid"), list("keys", values, "array_list"),
            enumeration("InterpolationType", "interpolationType", "LINEAR"), enumeration("ExtendedInterpolationType", "extendedInterpolationType", "LINEAR"),
            integer("insertPointCount", 1), real("extendedInterpolationScale", 1), text("description")}; bound << b;
    }
    if (combinations != quint64(formGuids.size())) fail("Keyform count does not match its parameter grid. Blend-shape keyforms require a separate representation.");
    QVector<Node> keyforms;
    for (int i = 0; i < formGuids.size(); ++i) {
        QVector<Node> access; int remainder = i;
        // The first binding varies fastest in the stored keyform array.
        QVector<int> indexes(bindings.size());
        for (int j = 0; j < bindings.size(); ++j) { indexes[j] = remainder % bindings[j].keys.size(); remainder /= bindings[j].keys.size(); }
        for (int j = 0; j < bound.size(); ++j) access << object("KeyOnParameter", {}, {reference(bound[j], "binding"), integer("keyIndex", indexes[j])});
        keyforms << object("KeyformOnGrid", {}, {object("KeyformGridAccessKey", "accessKey", {list("_keyOnParameterList", access, "array_list")}), reference(formGuids[i], "keyformGuid")});
    }
    QVector<Node> refs; for (const auto& b : bound) refs << reference(b);
    g->children = {list("keyformsOnGrid", keyforms, "array_list"), list("keyformBindings", refs, "array_list")}; return g;
}
Node Project::controllable(const QString& name, int partIndex, const Node& gridNode, const QVector<Node>& extensions) {
    return object("ACParameterControllableSource", "super", {text("localName", name), boolean("isVisible", true), boolean("isLocked", false),
        reference(part(partIndex), "parentGuid"), reference(gridNode, "keyformGridSource"),
        object("KeyFormMorphTargetSet", "keyformMorphTargetSet", {list("_morphTargets"), object("MorphTargetBlendWeightConstraintSet", "blendWeightConstraintSet", {list("_constraints")})}),
        list("_extensions", extensions), node("null", "internalColor_direct_argb"), label()});
}
Node Project::formBase(const Node& source, const Node& formGuid) {
    return object("ACForm", "super", {reference(formGuid, "guid"), boolean("isAnimatedForm", false), boolean("isLocalAnimatedForm", false), reference(source, "_source"), node("null", "name"), text("notes")});
}
Node Project::drawForm(const Node& source, const Node& formGuid, const Form& form, int parent, bool drawable) {
    auto n = object(drawable ? "ACDrawableForm" : "ACDeformerForm", "super", {formBase(source, formGuid)});
    if (drawable) n->children << integer("drawOrder", qRound(form.drawOrder));
    n->children << real("opacity", form.opacity) << colors("multiplyColor", form.multiplyColor) << colors("screenColor", form.screenColor)
        << reference(parent < 0 ? coordCanvas : coordLocal, "coordType"); return n;
}

void Project::makeParameters() {
    for (const auto& p : m.parameters) {
        if (p.type != 0 && p.type != 1) fail("Unknown parameter type.");
        parameterGuids << share(guid("CParameterGuid"));
    }
    display = displayLayout(m.displayInfo, m.parameters);
    groupGuids << rootGroupGuid; groupSources << rootGroup;
    for (int i = 1; i < display.groups.size(); ++i) { groupGuids << share(guid("CParameterGroupGuid")); groupSources << share(node("CParameterGroup")); }
    for (int i = 0; i < display.groups.size(); ++i) {
        const auto& group = display.groups[i]; QVector<Node> childGuids;
        for (int p : group.parameters) childGuids << reference(parameterGuids[p]);
        for (int g : group.childGroups) childGuids << reference(groupGuids[g]);
        groupSources[i]->children = {text("name", group.name), text("description"), boolean("folderIsOpened", i == 0), reference(groupGuids[i], "guid"),
            reference(group.parent < 0 ? Node{} : groupGuids[group.parent], "parentGroupGuid"), list("_childGuids", childGuids), id("CParameterGroupId", "id", group.id), label()};
    }
}

Node Project::effectGroups() {
    QHash<QString, int> byId; for (int i = 0; i < m.parameters.size(); ++i) byId[m.parameters[i].id] = i;
    QVector<Node> entries;
    for (const auto& value : m.effectGroups) {
        const auto group = value.toObject(); const auto name = group.value("Name").toString(); QVector<Node> parameters;
        if (group.value("Target").toString() != "Parameter" || (name != "EyeBlink" && name != "LipSync")) fail("Unsupported effect parameter group.");
        for (const auto& parameterValue : group.value("Ids").toArray()) {
            const auto parameter = parameterValue.toString(); if (!byId.contains(parameter)) fail("Effect group references an unknown parameter."); parameters << reference(parameterGuids[byId[parameter]]);
        }
        entries << object("entry", {}, {enumeration("EffectParameterType", "key", name == "EyeBlink" ? "EYE_BLINK" : "LIPSYNC"), list("value", parameters)});
    }
    auto groups = list("_parameterGroups", entries, "hash_map"); if (entries.isEmpty()) groups->attrs["keyType"] = "string";
    return object("CEffectParameterGroups", "_effectParameterGroups", {groups});
}

void Project::addMorphs(const Node& base, const Node& source, const QVector<Binding>& bindings, const QVector<BlendTrack>& blends, QVector<Form>& forms, QVector<Node>& guids) {
    if (blends.isEmpty()) return;
    if (forms.isEmpty()) fail("Blend shapes need a base keyform.");
    Form neutral = forms.front(); neutral.opacity = 0; neutral.drawOrder = 0; neutral.angle = 0; neutral.scale = 0; neutral.origin = {};
    neutral.multiplyColor = {0, 0, 0}; neutral.screenColor = {0, 0, 0}; for (auto& p : neutral.points) p = {};
    QVector<QVector<double>> weights;
    for (const auto& b : bindings) {
        QVector<double> w(b.keys.size(), 0); const double value = m.parameters[b.parameter].initial;
        if (value <= b.keys.front()) w[0] = 1;
        else if (value >= b.keys.back()) w.back() = 1;
        else for (int i = 0; i + 1 < b.keys.size(); ++i) if (value >= b.keys[i] && value <= b.keys[i + 1]) {
            const double fraction = (value - b.keys[i]) / (b.keys[i + 1] - b.keys[i]); w[i] = 1 - fraction; w[i + 1] = fraction; break;
        }
        weights << w;
    }
    double strongestWeight = -1;
    for (int index = 0; index < forms.size(); ++index) {
        int remainder = index; double weight = 1;
        for (int j = 0; j < weights.size(); ++j) { weight *= weights[j][remainder % weights[j].size()]; remainder /= weights[j].size(); }
        if (weight == 0) continue;
        const auto& f = forms[index]; if (f.points.size() != neutral.points.size()) fail("Blend base point counts are inconsistent.");
        for (int p = 0; p < neutral.points.size(); ++p) neutral.points[p] += f.points[p] * weight;
        neutral.opacity += f.opacity * float(weight); neutral.drawOrder += f.drawOrder * float(weight); neutral.angle += f.angle * float(weight); neutral.scale += f.scale * float(weight); neutral.origin += f.origin * weight;
        for (int c = 0; c < 3; ++c) { neutral.multiplyColor[c] += f.multiplyColor[c] * float(weight); neutral.screenColor[c] += f.screenColor[c] * float(weight); }
        if (weight > strongestWeight) { strongestWeight = weight; neutral.reflectX = f.reflectX; neutral.reflectY = f.reflectY; }
    }
    QVector<Node> targets, constraints;
    for (const auto& blend : blends) {
        if (blend.parameter < 0 || blend.parameter >= parameterGuids.size() || blend.deltas.size() != blend.keys.size()) fail("Invalid blend shape track.");
        for (int k = 0; k < blend.keys.size(); ++k) {
            if (k == blend.neutralKey) continue;
            const auto& delta = blend.deltas[k]; Form f = neutral;
            if (!delta.points.isEmpty() && delta.points.size() != f.points.size()) fail("Blend shape point counts are inconsistent.");
            for (int p = 0; p < delta.points.size(); ++p) f.points[p] += delta.points[p];
            f.opacity += delta.opacity; f.drawOrder += delta.drawOrder; f.angle += delta.angle; f.scale += delta.scale; f.origin += delta.origin;
            for (int c = 0; c < 3; ++c) { f.multiplyColor[c] += delta.multiplyColor[c]; f.screenColor[c] += delta.screenColor[c]; }
            auto fg = share(guid("CFormGuid")); forms << f; guids << fg;
            targets << object("KeyFormMorphTarget", {}, {reference(parameterGuids[blend.parameter], "parameterGuid"), real("keyValue", blend.keys[k]), reference(fg, "keyformGuid"), reference(source, "owner"),
                object("MorphTargetEditBaseParameterMap", "editBaseParameterMap", {node("null", "baseParameterMap")})});
        }
        for (int constraintIndex : blend.constraints) {
            if (constraintIndex < 0 || constraintIndex >= m.blendConstraints.size()) fail("Invalid blend shape constraint reference.");
            const auto& c = m.blendConstraints[constraintIndex];
            if (c.parameter < 0 || c.parameter >= parameterGuids.size() || c.keys.size() != c.weights.size()) fail("Invalid blend shape constraint.");
            for (int k = 0; k < c.keys.size(); ++k) constraints << object("MorphTargetBlendWeightConstraint", {}, {reference(parameterGuids[blend.parameter], "morphTargetParameterGuid"), reference(parameterGuids[c.parameter], "constraintParameterGuid"), real("constraintParameterValue", c.keys[k]), real("blendWeight", c.weights[k])});
        }
    }
    base->children[5] = object("KeyFormMorphTargetSet", "keyformMorphTargetSet", {list("_morphTargets", targets), object("MorphTargetBlendWeightConstraintSet", "blendWeightConstraintSet", {list("_constraints", constraints)})});
}
void Project::makeParts() {
    QVector<int> meshRanks(m.meshes.size());
    for (int rank = 0; rank < m.meshes.size(); ++rank)
        meshRanks[m.drawOrderSequence.isEmpty() ? rank : m.drawOrderSequence[rank]] = rank;
    QVector<int> partFirst(m.parts.size(), m.meshes.size()), partLast(m.parts.size(), -1), partCount(m.parts.size(), 0);
    for (int mesh = 0; mesh < m.meshes.size(); ++mesh) {
        for (int parent = m.meshes[mesh].part; parent >= 0; parent = m.parts[parent].parent) {
            partFirst[parent] = std::min(partFirst[parent], meshRanks[mesh]);
            partLast[parent] = std::max(partLast[parent], meshRanks[mesh]);
            ++partCount[parent];
        }
    }
    for (int part = 0; part < m.parts.size(); ++part)
        if (partCount[part] && partLast[part] - partFirst[part] + 1 != partCount[part])
            fail("The mesh draw-order sequence cannot be represented by the part hierarchy.");
    QVector<Node> sources = {rootPart}; sources += partSources;
    for (int index = -1; index < m.parts.size(); ++index) {
        auto source = sources[index + 1]; auto fg = share(guid("CFormGuid")); auto g = grid({}, {fg});
        auto base = controllable(index < 0 ? "Parts" : m.parts[index].name, index < 0 ? -1 : m.parts[index].parent, g);
        if (index < 0) base->children[3] = node("null", "parentGuid");
        auto form = object("CPartForm", {}, {formBase(source, fg), integer("drawOrder", 500), real("opacity", 1), colors("multiplyColor", true), colors("screenColor", false)});
        // Equal draw orders are resolved by reverse traversal of the part tree.
        // Merge parts and meshes before reversing so their relative order remains
        // valid even when a part is between two meshes in its parent's list.
        QVector<QPair<int, Node>> orderedChildren;
        for (int k = 0; k < m.parts.size(); ++k) if (m.parts[k].parent == index)
            orderedChildren << qMakePair(partCount[k] ? partFirst[k] : -1, partGuids[k]);
        for (int k = 0; k < m.meshes.size(); ++k) if (m.meshes[k].part == index)
            orderedChildren << qMakePair(meshRanks[k], meshGuids[k]);
        std::stable_sort(orderedChildren.begin(), orderedChildren.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        QVector<Node> children;
        for (const auto& child : orderedChildren) children << reference(child.second);
        for (int k = 0; k < m.deformers.size(); ++k) if (m.deformers[k].part == index) children << reference(deformerGuids[k]);
        if (index < 0) for (const auto& glueGuid : glueGuids) children << reference(glueGuid);
        source->children = {base, reference(part(index), "guid"), id("CPartId", "id", index < 0 ? "__RootPart__" : m.parts[index].id), list("keyforms", {form}),
            boolean("enableDrawOrderGroup", false), integer("defaultOrder_forEditor", 500), boolean("isSketch", false), node("CColor", "partsEditColor"), list("_childGuids", children),
            boolean("useOffscreen", false), list("clipGuidList"), boolean("invertClippingMask", false), enumeration("ColorComposition", "colorComposition", "NORMAL"), enumeration("AlphaComposition", "alphaComposition", "OVER")};
    }
}

void Project::makeGlues() {
    for (int i = 0; i < m.glues.size(); ++i) {
        const auto& glue = m.glues[i]; auto source = glueSources[i];
        if (glue.meshA < 0 || glue.meshA >= meshGuids.size() || glue.meshB < 0 || glue.meshB >= meshGuids.size()
            || glue.verticesA.size() != glue.verticesB.size() || glue.verticesA.size() != glue.weightsA.size() || glue.verticesB.size() != glue.weightsB.size()) fail("Invalid glue mesh references or vertex weights.");
        QVector<Node> fgs; for (int j = 0; j < glue.intensities.size(); ++j) fgs << share(guid("CFormGuid"));
        auto g = grid(glue.bindings, fgs); QVector<Node> forms;
        for (int j = 0; j < fgs.size(); ++j) forms << object("CGlueForm", {}, {object("ACAffecterForm", "super", {formBase(source, fgs[j])}), real("intensity", glue.intensities[j])});
        QStringList weights, vertexUids;
        for (int j = 0; j < glue.verticesA.size(); ++j) {
            weights << number(glue.weightsA[j]) << number(glue.weightsB[j]);
            vertexUids << QString::number(glue.verticesA[j]) << QString::number(glue.verticesB[j]);
        }
        auto weightArray = scalar("float-array", "weights", weights.join(' ')); weightArray->attrs["count"] = QString::number(weights.size());
        auto vertexArray = scalar("long-array", "bindVertexUids", vertexUids.join(' ')); vertexArray->attrs["count"] = QString::number(vertexUids.size());
        QSet<int> ancestry; int a = m.meshes[glue.meshA].deformer;
        while (a >= 0) { ancestry.insert(a); a = m.deformers[a].parent; }
        int common = m.meshes[glue.meshB].deformer; while (common >= 0 && !ancestry.contains(common)) common = m.deformers[common].parent;
        source->children = {object("ACAffecterSource", "super", {controllable(glue.id, -1, g), reference(glueGuids[i], "guid"), id("CAffecterId", "id", glue.id), reference(deformer(common), "targetDeformerGuid"), integer("editVersion", 0)}),
            weightArray, list("keyforms", forms), reference(meshGuids[glue.meshA], "targetArtMeshA_guid"), reference(meshGuids[glue.meshB], "targetArtMeshB_guid"), vertexArray,
            object("GVector2", "tabPosOnCanvas", {real("x", 0), real("y", 0)})};
    }
}
void Project::makeTextures() {
    rawImage = share(node("CLayeredImage")); rawImageGuid = share(guid("CLayeredImageGuid")); rawRoot = share(node("CLayerGroup"));
    imageGroup = share(node("CModelImageGroup")); imageFilter = makeImageFilter();
    QVector<Node> layers, modelImages;
    auto emptyMap = [](const QString& name) { auto n = list(name, {}, "hash_map"); n->attrs["keyType"] = "string"; return n; };
    auto entry = [](const Node& key, const Node& value) { return object("entry", {}, {key, value}); };
    auto layerBase = [&](const QString& name, const Node& group) {
        return object("ACLayerEntry", "super", {text("name", name), text("memo"), boolean("isVisible", true), boolean("isClipping", false), node("CBlend_Normal", "blend"),
            guid("CLayerGuid", "guid"), reference(group, "group"), integer("opacity255", 255), boolean("isTransparencyShapesLayer", true), emptyMap("_optionOfIOption"), reference(rawImage, "_layeredImage")});
    };
    for (int i = 0; i < m.textures.size(); ++i) {
        QImage image(m.textures[i]); if (image.isNull()) fail("Cannot read a texture image.");
        QByteArray bytes; QBuffer output(&bytes); output.open(QIODevice::WriteOnly);
        if (!image.save(&output, "PNG")) fail("Cannot encode a texture image.");
        QByteArray path = "texture_" + QByteArray::number(i) + ".png";
        assets << Asset{path, {}, bytes, 0x10}; textureSizes << image.size();
        auto resource = share(node("CImageResource"));
        resource->attrs["width"] = QString::number(image.width()); resource->attrs["height"] = QString::number(image.height());
        resource->attrs["type"] = "INT_ARGB"; resource->attrs["imageFileBuf_size"] = QString::number(bytes.size()); resource->attrs["previewFileBuf_size"] = "0";
        auto file = node("file", "imageFileBuf"); file->attrs["path"] = QString::fromLatin1(path); resource->children = {file}; images << resource;
        auto texture = share(node("GTexture2D"));
        texture->children = {object("GTexture", "super", {text("name", "Texture " + QString::number(i + 1)), enumeration("WrapMode", "wrapMode", "CLAMP_TO_BORDER"),
            object("FilterMode", "filterMode", {reference(texture, "owner"), enumeration("MinFilter", "minFilter", "LINEAR_MIPMAP_LINEAR"), enumeration("MagFilter", "magFilter", "LINEAR")}),
            guid("GTextureGuid", "guid"), enumeration("Anisotropy", "anisotropy", "ON")}), reference(resource, "srcImageResource"), affine("transformImageResource01toLogical01"), integer("mipmapLevel", 64), boolean("isPremultiplied", false)};
        textures << texture;
        auto layer = share(node("CLayer")); layers << layer;
        layer->children = {object("ACImageLayer", "super", {layerBase("Texture " + QString::number(i + 1), rawRoot)}), reference(resource, "imageResource"),
            object("CRect", "boundsOnImageDoc", {integer("x", 0), integer("y", 0), integer("width", image.width()), integer("height", image.height())}),
            object("CLayerIdentifier", "layerIdentifier", {text("layerName", "Texture " + QString::number(i + 1)), text("layerId", QString::number(i)), integer("layerIdValue_testImpl", i)}),
            node("null", "icon16"), node("null", "icon64"), list("layerInfo", {}, "linked_map"), emptyMap("_optionOfIOption")};
        auto modelGuid = share(guid("CModelImageGuid")); modelImageGuids << modelGuid;
        auto modelImage = node("CModelImage"); modelImage->attrs["modelImageVersion"] = "1";
        auto envValue = [&](const QString& key, const Node& value) {
            return entry(id("FilterValueId", "key", key), object("EnvValueSet", "value", {id("FilterValueId", "id", key), value, scalar("l", "updateTimeMs", "0")}));
        };
        auto layerInput = object("CLayerSelectorMap", "value", {list("_imageToLayerInput", {entry(reference(rawImageGuid, "key"), list("value", {object("CLayerInputData", {}, {reference(layer, "layer"), affine("affine"), node("null", "clippingOnTexturePx")})}, "array_list"))}, "linked_map")});
        auto rawSize = node("CSize", "rawImageSize"); rawSize->attrs["width"] = QString::number(image.width()); rawSize->attrs["height"] = QString::number(image.height());
        modelImage->children = {reference(modelGuid, "guid"), text("name", "Texture " + QString::number(i + 1)), reference(imageFilter, "inputFilter"),
            object("ModelImageFilterEnv", "inputFilterEnv", {object("FilterEnv", "super", {node("null", "parentEnv"), list("envValues", {envValue("mi_currentImageGuid", reference(rawImageGuid, "value")), envValue("mi_input_layerInputData", layerInput)}, "hash_map")})}),
            reference(resource, "_filteredImage"), node("null", "icon16"), affine("_materialLocalToCanvasTransform"), reference(imageGroup, "_group"), list("linkedRawImageGuids", {reference(rawImageGuid)}),
            object("CCachedImageManager", "cachedImageManager", {enumeration("CachedImageType", "defaultCacheType", "SCALE_1"), reference(resource, "rawImage"),
                list("cachedImages", {object("CCachedImage", {}, {reference(resource, "_cachedImageResource"), boolean("isSharedImage", true), rawSize, integer("reductionRatio", 1), integer("mipmapLevel", 64), boolean("hasMargin", false), boolean("isCleaned", false), affine("transformRawImageToCachedImage")})}, "array_list"), integer("requiredMipmapLevel", 64)}), text("memo")};
        modelImages << modelImage;
    }
    QVector<Node> layerRefs; for (const auto& layer : layers) layerRefs << reference(layer);
    rawRoot->children = {object("ACLayerGroup", "super", {layerBase("Textures", {}), list("_children", layerRefs)}), node("null", "layerIdentifier")};
    layerRefs.prepend(reference(rawRoot));
    rawImage->children = {text("name", "Recovered textures"), text("memo"), integer("width", qRound64(m.canvasWidth)), integer("height", qRound64(m.canvasHeight)), node("null", "psdFile"), text("description"),
        reference(rawImageGuid, "guid"), node("null", "psdBytes"), scalar("l", "psdFileLastModified", "0"), reference(rawRoot, "_rootLayer"), object("LayerSet", "layerSet", {reference(rawImage, "_layeredImage"), list("_layerEntryList", layerRefs)}), node("null", "icon16"), node("null", "icon64")};
    imageGroup->children = {text("memo"), text("groupName", "Textures"), list("_linkedRawImageGuids", {reference(rawImageGuid)}), list("_modelImages", modelImages)};
}

Node Project::makeImageFilter() {
    auto filters = share(node("ModelImageFilterSet")); auto selector = share(node("FilterInstance")); auto raster = share(node("FilterInstance")); auto output = share(node("FilterOutputValueConnector"));
    selector->attrs["filterName"] = "CLayerSelector"; raster->attrs["filterName"] = "CLayerFilter";
    auto def = [](const QString& name, const QString& key, const QString& title) { return object("FilterValue", name, {text("name", title), id("FilterValueId", "id", key), node("null", "defaultValueInitializer")}); };
    auto entry = [](const Node& key, const Node& value) { return object("entry", {}, {key, value}); };
    auto env = [&](const QString& local, const QString& external) { return entry(id("FilterValueId", "key", local), object("EnvValueConnector", "value", {node("AValueConnector", "super"), id("FilterValueId", "envValueId", external)})); };
    auto staticGuid = [](const QString& value) { auto n = node("StaticFilterDefGuid", "filterDefGuid"); n->attrs["uuid"] = value; n->attrs["note"] = ""; return n; };
    // These two UUIDs identify the editor's built-in filter definitions.
    selector->children = {staticGuid("5e9fe1ea-0ec3-4d68-a5fa-018fc7abe301"), node("null", "filterDef"), id("FilterInstanceId", "filterId", "selector"),
        list("inputConnectors", {env("ilf_inputLayerData", "mi_input_layerInputData"), env("ilf_currentImageGuid", "mi_currentImageGuid")}, "hash_map"),
        list("outputConnectors", {entry(id("FilterValueId", "key", "ilf_outputLayerData"), reference(output, "value"))}, "hash_map"), reference(filters, "ownerFilterSet")};
    raster->children = {staticGuid("4083cd1f-40ba-4eda-8400-379019d55ed8"), node("null", "filterDef"), id("FilterInstanceId", "filterId", "raster"),
        list("inputConnectors", {entry(id("FilterValueId", "key", "ilf_inputLayer"), reference(output, "value"))}, "hash_map"), list("outputConnectors", {}, "hash_map"), reference(filters, "ownerFilterSet")};
    output->children = {node("AValueConnector", "super"), reference(selector, "instance"), id("FilterValueId", "id", "ilf_outputLayerData"), def("valueDef", "ilf_outputLayerData", "output layer")};
    auto connection = [&](const QString& external, const QString& internal, const Node& filter) {
        return entry(id("FilterValueId", "key", external), object("EnvConnection", "value", {def("_envValueDef", external, external), reference(filter, "filter"), def("filterValueDef", internal, internal)}));
    };
    filters->children = {object("FilterSet", "super", {list("filterMap", {entry(id("FilterInstanceId", "key", "selector"), reference(selector, "value")), entry(id("FilterInstanceId", "key", "raster"), reference(raster, "value"))}, "linked_map"),
        list("_externalInputs", {connection("mi_input_layerInputData", "ilf_inputLayerData", selector), connection("mi_currentImageGuid", "ilf_currentImageGuid", selector)}, "linked_map"),
        list("_externalOutputs", {connection("mi_output_image", "ilf_outputImageRes", raster), connection("mi_output_transform", "ilf_outputTransform", raster)}, "linked_map")})};
    return filters;
}
void Project::makeDeformers() {
    for (int i = 0; i < m.deformers.size(); ++i) {
        const auto& d = m.deformers[i]; auto source = deformerSources[i]; QVector<Node> fgs;
        for (int k = 0; k < d.forms.size(); ++k) fgs << share(guid("CFormGuid"));
        auto g = grid(d.bindings, fgs);
        auto control = controllable(d.name, d.part, g); auto allForms = d.forms; addMorphs(control, source, d.bindings, d.blends, allForms, fgs);
        source->children = {object("ACDeformerSource", "super", {control, reference(deformerGuids[i], "guid"), id("CDeformerId", "id", d.id), reference(deformer(d.parent), "targetDeformerGuid")})};
        QVector<Node> forms;
        for (int k = 0; k < allForms.size(); ++k) {
            const auto& f = allForms[k]; auto n = node(d.kind == Deformer::Kind::Warp ? "CWarpDeformerForm" : "CRotationDeformerForm");
            n->children = {drawForm(source, fgs[k], f, d.parent, false)};
            if (d.kind == Deformer::Kind::Warp) {
                if (f.points.size() != (d.rows + 1) * (d.columns + 1)) fail("Warp grid dimensions do not match its point data.");
                n->children << floats("positions", positions(f.points, d.parent));
            } else {
                auto origin = positions({f.origin}, d.parent).front();
                n->attrs["angle"] = number(f.angle); n->attrs["originX"] = number(origin.x()); n->attrs["originY"] = number(origin.y());
                // The conversion from normalized coordinates to rotation-local
                // pixels happens once along a parent chain. An intervening warp
                // retains that coordinate system; it does not start a new one.
                bool hasRotationAncestor = false;
                for (int ancestor = d.parent; ancestor >= 0; ancestor = m.deformers[ancestor].parent) {
                    if (m.deformers[ancestor].kind == Deformer::Kind::Rotation) { hasRotationAncestor = true; break; }
                }
                n->attrs["scale"] = number(f.scale * (hasRotationAncestor ? 1.0 : m.pixelsPerUnit)); n->attrs["isReflectX"] = f.reflectX ? "true" : "false"; n->attrs["isReflectY"] = f.reflectY ? "true" : "false";
            }
            forms << n;
        }
        if (d.kind == Deformer::Kind::Warp) source->children << integer("col", d.columns) << integer("row", d.rows) << boolean("isQuadTransform", true);
        else source->children << boolean("useBoneUi_testImpl", true);
        source->children << list("keyforms", forms);
        if (d.kind == Deformer::Kind::Rotation) source->children << real("handleLengthOnCanvas", 100) << real("circleRadiusOnCanvas", 30) << real("baseAngle", d.baseAngle);
    }
}
void Project::makeMeshes() {
    for (int i = 0; i < m.meshes.size(); ++i) {
        const auto& mesh = m.meshes[i]; auto source = meshSources[i];
        if (mesh.forms.isEmpty() || mesh.uvs.isEmpty() || mesh.texture < 0 || mesh.texture >= textures.size()) fail("A mesh is missing geometry or its texture.");
        QVector<Node> fgs; for (int k = 0; k < mesh.forms.size(); ++k) fgs << share(guid("CFormGuid"));
        auto g = grid(mesh.bindings, fgs);
        QVector<int> edges, priorities, pointUids; QSet<quint64> seen;
        for (int k = 0; k < mesh.triangles.size(); k += 3) for (int j = 0; j < 3; ++j) {
            int a = mesh.triangles[k + j], b = mesh.triangles[k + (j + 1) % 3]; if (a > b) std::swap(a, b);
            quint64 key = (quint64(quint32(a)) << 32) | quint32(b); if (seen.contains(key)) continue;
            seen.insert(key); edges << a << b; priorities << 30;
        }
        QVector<QPointF> basePoints; const auto size = textureSizes[mesh.texture];
        for (int k = 0; k < mesh.uvs.size(); ++k) { auto uv = mesh.uvs[k]; basePoints << QPointF(uv.x() * size.width(), uv.y() * size.height()); pointUids << k; }
        auto editable = object("GEditableMesh2", "editableMesh", {floats("point", basePoints), integers("byte-array", "pointPriority", QVector<int>(basePoints.size(), 10)),
            integers("short-array", "edge", edges), integers("byte-array", "edgePriority", priorities), integers("int-array", "pointUid", pointUids), guid("GEditableMeshGuid", "meshGuid"), reference(coordBasic, "coordType")});
        editable->attrs["nextPointUid"] = QString::number(basePoints.size()); editable->attrs["useDelaunayTriangulation"] = "false";
        auto extension = object("CEditableMeshExtension", {}, {object("ACExtension", "super", {guid("CExtensionGuid", "guid"), reference(source, "_owner")}), editable, boolean("isLocked", false)});
        QVector<Node> masks; for (int mask : mesh.masks) { if (mask < 0 || mask >= meshGuids.size()) fail("Mesh clipping reference is out of range."); masks << reference(meshGuids[mask]); }
        auto textureExtension = share(node("CTextureInputExtension")); auto textureInput = share(node("CTextureInput_ModelImage"));
        textureInput->children = {object("ACTextureInput", "super", {affine("optionalTransformOnCanvas"), reference(textureExtension, "_owner")}), reference(modelImageGuids[mesh.texture], "_modelImageGuid")};
        textureExtension->children = {object("ACExtension", "super", {guid("CExtensionGuid", "guid"), reference(source, "_owner")}), list("_textureInputs", {reference(textureInput)}), reference(textureInput, "currentTextureInputData")};
        auto control = controllable(mesh.name, mesh.part, g, {reference(textureExtension), extension}); auto allForms = mesh.forms; addMorphs(control, source, mesh.bindings, mesh.blends, allForms, fgs);
        source->children = {object("ACDrawableSource", "super", {control, id("CDrawableId", "id", mesh.id),
            reference(meshGuids[i], "guid"), reference(deformer(mesh.deformer), "targetDeformerGuid"), list("clipGuidList", masks), boolean("invertClippingMask", mesh.flags & 8), node("null", "icon32"), node("null", "icon16")}),
            integers("int-array", "indices", mesh.triangles)};
        QVector<Node> forms;
        for (int k = 0; k < allForms.size(); ++k) {
            const auto& f = allForms[k]; if (f.points.size() != mesh.uvs.size()) fail("Mesh keyform point count is inconsistent.");
            forms << object("CArtMeshForm", {}, {drawForm(source, fgs[k], f, mesh.deformer, true), floats("positions", positions(f.points, mesh.deformer))});
        }
        const auto& uv = mesh.uvs;
        source->children << list("keyforms", forms) << floats("positions", basePoints) << floats("uvs", uv) << reference(textures[mesh.texture], "texture")
            << enumeration("ColorComposition", "colorComposition", (mesh.flags & 1) ? "ADD" : (mesh.flags & 2) ? "MULTIPLY" : "NORMAL")
            << boolean("culling", !(mesh.flags & 4)) << enumeration("TextureState", "textureState", "MODEL_IMAGE") << text("userData") << enumeration("AlphaComposition", "alphaComposition", "OVER");
    }
}

QByteArray Project::build() {
    if (!m.unsupported.isEmpty()) fail("Unsupported model features: " + m.unsupported.join(", "));
    if (!(m.canvasWidth > 0 && m.canvasHeight > 0 && m.pixelsPerUnit > 0)) fail("Invalid canvas dimensions.");
    auto checkIndex = [](int index, qsizetype count) { if (index < -1 || index >= count) fail("A parent reference is out of range."); };
    for (int i = 0; i < m.parts.size(); ++i) {
        QSet<int> ancestors; int p = i;
        while (p >= 0) { checkIndex(p, m.parts.size()); if (ancestors.contains(p)) fail("Part hierarchy contains a cycle."); ancestors.insert(p); p = m.parts[p].parent; }
    }
    for (int i = 0; i < m.deformers.size(); ++i) {
        checkIndex(m.deformers[i].part, m.parts.size()); QSet<int> ancestors; int p = i;
        while (p >= 0) { checkIndex(p, m.deformers.size()); if (ancestors.contains(p)) fail("Deformer hierarchy contains a cycle."); ancestors.insert(p); p = m.deformers[p].parent; }
    }
    for (const auto& mesh : m.meshes) {
        checkIndex(mesh.part, m.parts.size()); checkIndex(mesh.deformer, m.deformers.size());
        if (mesh.uvs.size() > 32767 || mesh.triangles.size() % 3 != 0) fail("Mesh topology exceeds the supported editable-mesh range.");
        for (int vertex : mesh.triangles) if (vertex < 0 || vertex >= mesh.uvs.size()) fail("Triangle vertex index is out of range.");
    }
    if (!m.drawOrderSequence.isEmpty()) {
        if (m.drawOrderSequence.size() != m.meshes.size()) fail("Draw order is not a complete mesh permutation.");
        QSet<int> seen;
        for (int mesh : m.drawOrderSequence) { if (mesh < 0 || mesh >= m.meshes.size() || seen.contains(mesh)) fail("Invalid draw order mesh index."); seen.insert(mesh); }
    }
    rootPartGuid = share(guid("CPartGuid")); rootPart = share(node("CPartSource"));
    rootGroupGuid = share(guid("CParameterGroupGuid")); rootGroup = share(node("CParameterGroup"));
    rootDeformerGuid = share(guid("CDeformerGuid"));
    // Verified against an editor-created roundtrip: this is the ROOT sentinel,
    // not a model-specific deformer identifier.
    rootDeformerGuid->attrs["uuid"] = "71fae776-e218-4aee-873e-78e8ac0cb48a";
    rootDeformerGuid->attrs["note"] = "ROOT";
    coordCanvas = share(object("CoordType", {}, {text("coordName", "Canvas")}));
    coordLocal = share(object("CoordType", {}, {text("coordName", "DeformerLocal")}));
    coordBasic = share(object("CoordType", {}, {text("coordName", "Basic Coord")}));
    for (const auto& p : m.parts) { Q_UNUSED(p); partGuids << share(guid("CPartGuid")); partSources << share(node("CPartSource")); }
    for (const auto& d : m.deformers) { deformerGuids << share(guid("CDeformerGuid")); deformerSources << share(node(d.kind == Deformer::Kind::Warp ? "CWarpDeformerSource" : "CRotationDeformerSource")); }
    for (const auto& mesh : m.meshes) { Q_UNUSED(mesh); meshGuids << share(guid("CDrawableGuid")); meshSources << share(node("CArtMeshSource")); }
    for (const auto& glue : m.glues) { Q_UNUSED(glue); glueGuids << share(guid("CAffecterGuid")); glueSources << share(node("CGlueSource")); }
    makeParameters(); makeParts(); makeTextures(); makeDeformers(); makeMeshes(); makeGlues();
    QVector<Node> params;
    QHash<QString, QString> parameterReferences;
    for (int i = 0; i < m.parameters.size(); ++i) {
        const auto& p = m.parameters[i];
        parameterReferences[p.id] = parameterGuids[i]->attrs.value("xs.id");
        params << object("CParameterSource", {}, {integer("decimalPlaces", p.decimals), reference(parameterGuids[i], "guid"), real("snapEpsilon", 0.001),
            real("minValue", p.minimum), real("maxValue", p.maximum), real("defaultValue", p.initial), boolean("isRepeat", p.repeat), id("CParameterId", "id", p.id),
            enumeration("Type", "paramType", p.type == 0 ? "NORMAL" : "MORPH_TARGET"), text("name", p.name), text("description"), boolean("combined", display.combined[i]), reference(groupGuids[display.parameterParents[i]], "parentGroupGuid")});
    }
    auto references = [](const QVector<Node>& values) { QVector<Node> out; for (const auto& n : values) out << reference(n); return out; };
    QVector<Node> drawableRefs;
    if (m.drawOrderSequence.isEmpty()) drawableRefs = references(meshSources);
    else {
        if (m.drawOrderSequence.size() != meshSources.size()) fail("Draw order is not a complete mesh permutation.");
        QSet<int> seen;
        for (int mesh : m.drawOrderSequence) {
            if (mesh < 0 || mesh >= meshSources.size() || seen.contains(mesh)) fail("Invalid draw order mesh index.");
            seen.insert(mesh); drawableRefs << reference(meshSources[mesh]);
        }
    }
    auto partRefs = references(partSources); partRefs.prepend(reference(rootPart));
    auto emptyMap = [](const QString& name) { auto n = list(name, {}, "hash_map"); n->attrs["keyType"] = "string"; return n; };
    auto main = object("CModelSource", {}, {guid("CModelGuid", "guid"), text("name", m.displayName), object("EditorEdition", "editorEdition", {integer("edition", 15)}),
        object("CImageCanvas", "canvas", {integer("pixelWidth", qRound64(m.canvasWidth)), integer("pixelHeight", qRound64(m.canvasHeight)), node("CColor", "background")}),
        object("CParameterSourceSet", "parameterSourceSet", {list("_sources", params)}),
        object("CTextureManager", "textureManager", {object("TextureImageGroup", "textureList", {list("children")}), list("_rawImages", {object("LayeredImageWrapper", {}, {reference(rawImage, "image"), scalar("l", "importedTimeMSec", "0"), scalar("l", "lastModifiedTimeMSec", "0"), boolean("isReplaced", false)})}), list("_modelImageGroups", {reference(imageGroup)}), list("_textureAtlases"), boolean("isTextureInputModelImageMode", true), integer("previewReductionRatio", 1), list("artPathBrushUsingLayeredImageIds")}),
        boolean("useLegacyDrawOrder__testImpl", false), object("CDrawableSourceSet", "drawableSourceSet", {list("_sources", drawableRefs)}),
        object("CDeformerSourceSet", "deformerSourceSet", {list("_sources", references(deformerSources))}), object("CAffecterSourceSet", "affecterSourceSet", {list("_sources", references(glueSources))}),
        object("CPartSourceSet", "partSourceSet", {list("_sources", partRefs)}), readFragment(physicsXml(m.physics, parameterReferences)), reference(rootPart, "rootPart"),
        object("CParameterGroupSet", "parameterGroupSet", {list("_groups", references(groupSources))}), reference(rootGroup, "rootParameterGroup"),
        object("CModelInfo", "modelInfo", {real("pixelsPerUnit", m.pixelsPerUnit), object("CPoint", "originInPixels", {integer("x", qRound64(m.canvasOrigin.x())), integer("y", qRound64(m.canvasOrigin.y()))}), effectGroups()}),
        emptyMap("modelOptions"), node("null", "_icon64"), node("null", "_icon32"), node("null", "_icon16"),
        object("CGameMotionSet", "gameMotionSet", {list("gameMotions"), list("gameMotionGroups")}), object("ModelViewerSetting", "modelViewerSetting", {list("trackCursorSettings", {}, "array_list")}),
        object("CGuidesSetting", "guides", {list("guidesModeling")}), integer("targetVersionNo", 5030000), integer("latestVersionOfLastModelerNo", 5030000),
        object("CArtPathBrushSetting", "artPathBrushesSetting", {list("brushes")}), object("CRandomPoseSettingManager", "randomPoseSetting", {list("_settings", {}, "array_list")}),
        object("CMotionSyncSettingSourceSet", "motionSyncSettingsSet", {list("_settingSourceSetMotionSync", {}, "linked_set")})});
    main->attrs["isDefaultKeyformLocked"] = "false";
    QMap<QString, QString> identities;
    for (const auto& n : shared) {
        const auto identity = n->attrs.value("xs.id");
        if (identity.isEmpty() || identities.contains(identity)) fail("A shared object has a duplicate identity.");
        identities[identity] = n->tag;
    }
    for (const auto& n : shared) checkObjectReferences(n, identities, true);
    checkObjectReferences(main, identities);
    QByteArray xml; QXmlStreamWriter writer(&xml); writer.setAutoFormatting(true); writer.writeStartDocument();
    for (const auto& version : {"CArtMeshSource:5", "KeyformGridSource:1", "CParameterGroup:4", "KeyFormMorphTarget:2", "SerializeFormatVersion:2", "CModelSource:15", "CFloatColor:1", "CRotationDeformerForm:1", "CPartForm:2", "CPartSource:2", "CLabelColor:0", "CModelImage:3"}) writer.writeProcessingInstruction("version", version);
    // Import names are serialization schema identifiers, not implementation dependencies.
    const QString schemaImports = QString::fromLatin1(R"SCHEMA(
com.live2d.cubism.doc.gameData.motions.CGameMotionSet
com.live2d.cubism.doc.gameData.physics.CPhysicsController$CPhysicsSourceType
com.live2d.cubism.doc.gameData.physics.CPhysicsInput
com.live2d.cubism.doc.gameData.physics.CPhysicsOutput
com.live2d.cubism.doc.gameData.physics.CPhysicsSettingsSource
com.live2d.cubism.doc.gameData.physics.CPhysicsSettingsSourceSet
com.live2d.cubism.doc.gameData.physics.CPhysicsVertex
com.live2d.cubism.doc.model.ACForm
com.live2d.cubism.doc.model.ACParameterControllableSource
com.live2d.cubism.doc.model.CEffectParameterGroups
com.live2d.cubism.doc.model.EffectParameterType
com.live2d.cubism.doc.model.CLabelColor
com.live2d.cubism.doc.model.CLabelColorType
com.live2d.cubism.doc.model.CModelInfo
com.live2d.cubism.doc.model.CModelSource
com.live2d.cubism.doc.model.affecter.ACAffecterForm
com.live2d.cubism.doc.model.affecter.ACAffecterSource
com.live2d.cubism.doc.model.affecter.CAffecterSourceSet
com.live2d.cubism.doc.model.affecter.glue.CGlueForm
com.live2d.cubism.doc.model.affecter.glue.CGlueSource
com.live2d.cubism.doc.model.deformer.ACDeformerForm
com.live2d.cubism.doc.model.deformer.ACDeformerSource
com.live2d.cubism.doc.model.deformer.CDeformerSourceSet
com.live2d.cubism.doc.model.deformer.rotation.CRotationDeformerForm
com.live2d.cubism.doc.model.deformer.rotation.CRotationDeformerSource
com.live2d.cubism.doc.model.deformer.warp.CWarpDeformerForm
com.live2d.cubism.doc.model.deformer.warp.CWarpDeformerSource
com.live2d.cubism.doc.model.drawable.ACDrawableForm
com.live2d.cubism.doc.model.drawable.ACDrawableSource
com.live2d.cubism.doc.model.drawable.AlphaComposition
com.live2d.cubism.doc.model.drawable.CDrawableSourceSet
com.live2d.cubism.doc.model.drawable.ColorComposition
com.live2d.cubism.doc.model.drawable.TextureState
com.live2d.cubism.doc.model.drawable.artMesh.CArtMeshForm
com.live2d.cubism.doc.model.drawable.artMesh.CArtMeshSource
com.live2d.cubism.doc.model.drawable.artPath.Line.CArtPathBrushSetting
com.live2d.cubism.doc.model.extension.ACExtension
com.live2d.cubism.doc.model.extension.editableMesh.CEditableMeshExtension
com.live2d.cubism.doc.model.extension.textureInput.ACTextureInput
com.live2d.cubism.doc.model.extension.textureInput.CTextureInputExtension
com.live2d.cubism.doc.model.extension.textureInput.CTextureInput_ModelImage
com.live2d.cubism.doc.model.extension.textureInput.inputFilter.CLayerInputData
com.live2d.cubism.doc.model.extension.textureInput.inputFilter.CLayerSelectorMap
com.live2d.cubism.doc.model.extension.textureInput.inputFilter.ModelImageFilterEnv
com.live2d.cubism.doc.model.extension.textureInput.inputFilter.ModelImageFilterSet
com.live2d.cubism.doc.model.id.CAffecterId
com.live2d.cubism.doc.model.id.CDeformerId
com.live2d.cubism.doc.model.id.CDrawableId
com.live2d.cubism.doc.model.id.CParameterGroupId
com.live2d.cubism.doc.model.id.CParameterId
com.live2d.cubism.doc.model.id.CPartId
com.live2d.cubism.doc.model.id.CPhysicsSettingId
com.live2d.cubism.doc.model.interpolator.InterpolationType
com.live2d.cubism.doc.model.interpolator.KeyOnParameter
com.live2d.cubism.doc.model.interpolator.KeyformBindingSource
com.live2d.cubism.doc.model.interpolator.KeyformGridAccessKey
com.live2d.cubism.doc.model.interpolator.KeyformGridSource
com.live2d.cubism.doc.model.interpolator.KeyformOnGrid
com.live2d.cubism.doc.model.interpolator.extendedInterpolation.ExtendedInterpolationType
com.live2d.cubism.doc.model.morphTarget.KeyFormMorphTarget
com.live2d.cubism.doc.model.morphTarget.KeyFormMorphTargetSet
com.live2d.cubism.doc.model.morphTarget.MorphTargetBlendWeightConstraint
com.live2d.cubism.doc.model.morphTarget.MorphTargetBlendWeightConstraintSet
com.live2d.cubism.doc.model.morphTarget.MorphTargetEditBaseParameterMap
com.live2d.cubism.doc.model.motionSync.CMotionSyncSettingSourceSet
com.live2d.cubism.doc.model.options.edition.EditorEdition
com.live2d.cubism.doc.model.param.CParameterSource
com.live2d.cubism.doc.model.param.CParameterSource$Type
com.live2d.cubism.doc.model.param.CParameterSourceSet
com.live2d.cubism.doc.model.param.group.CParameterGroup
com.live2d.cubism.doc.model.param.group.CParameterGroupSet
com.live2d.cubism.doc.model.parts.CPartForm
com.live2d.cubism.doc.model.parts.CPartSource
com.live2d.cubism.doc.model.parts.CPartSourceSet
com.live2d.cubism.doc.model.randomPose.CRandomPoseSetting
com.live2d.cubism.doc.model.randomPose.CRandomPoseSetting$CRandomPoseGroupData
com.live2d.cubism.doc.model.randomPose.CRandomPoseSetting$CRandomPoseParamData
com.live2d.cubism.doc.model.randomPose.CRandomPoseSettingManager
com.live2d.cubism.doc.model.texture.CTextureManager
com.live2d.cubism.doc.model.texture.LayeredImageWrapper
com.live2d.cubism.doc.model.texture.TextureImageGroup
com.live2d.cubism.doc.model.texture.modelImage.CModelImage
com.live2d.cubism.doc.model.texture.modelImage.CModelImageGroup
com.live2d.cubism.doc.modeling.ui.guide.CGuidesSetting
com.live2d.cubism.doc.modeling.ui.viewer.ModelViewerSetting
com.live2d.cubism.doc.resources.ACImageLayer
com.live2d.cubism.doc.resources.ACLayerEntry
com.live2d.cubism.doc.resources.ACLayerGroup
com.live2d.cubism.doc.resources.CLayer
com.live2d.cubism.doc.resources.CLayerGroup
com.live2d.cubism.doc.resources.CLayerIdentifier
com.live2d.cubism.doc.resources.CLayeredImage
com.live2d.cubism.doc.resources.LayerSet
com.live2d.doc.CoordType
com.live2d.graphics.CImageCanvas
com.live2d.graphics.CImageResource
com.live2d.graphics.CWritableImage
com.live2d.graphics.cachedImage.CCachedImage
com.live2d.graphics.cachedImage.CCachedImageManager
com.live2d.graphics.cachedImage.CachedImageType
com.live2d.graphics.filter.AValueConnector
com.live2d.graphics.filter.FilterEnv
com.live2d.graphics.filter.FilterEnv$EnvValueSet
com.live2d.graphics.filter.FilterSet
com.live2d.graphics.filter.FilterSet$EnvConnection
com.live2d.graphics.filter.FilterValue
com.live2d.graphics.filter.concreteConnector.EnvValueConnector
com.live2d.graphics.filter.concreteConnector.FilterOutputValueConnector
com.live2d.graphics.filter.filterInstance.FilterInstance
com.live2d.graphics.filter.id.FilterInstanceId
com.live2d.graphics.filter.id.FilterValueId
com.live2d.graphics.psd.blend.ACBlend
com.live2d.graphics.psd.blend.CBlend_Normal
com.live2d.graphics3d.editableMesh.GEditableMesh2
com.live2d.graphics3d.texture.Anisotropy
com.live2d.graphics3d.texture.GTexture
com.live2d.graphics3d.texture.GTexture$FilterMode
com.live2d.graphics3d.texture.GTexture2D
com.live2d.graphics3d.texture.MagFilter
com.live2d.graphics3d.texture.MinFilter
com.live2d.graphics3d.texture.WrapMode
com.live2d.graphics3d.type.GVector2
com.live2d.type.CAffecterGuid
com.live2d.type.CAffine
com.live2d.type.CColor
com.live2d.type.CDeformerGuid
com.live2d.type.CDrawableGuid
com.live2d.type.CExtensionGuid
com.live2d.type.CFloatColor
com.live2d.type.CFormGuid
com.live2d.type.CImageIcon
com.live2d.type.CLayerGuid
com.live2d.type.CLayeredImageGuid
com.live2d.type.CModelGuid
com.live2d.type.CModelImageGuid
com.live2d.type.CParameterGroupGuid
com.live2d.type.CParameterGuid
com.live2d.type.CPartGuid
com.live2d.type.CPhysicsDataGuid
com.live2d.type.CPhysicsSettingsGuid
com.live2d.type.CPoint
com.live2d.type.CRect
com.live2d.type.CSize
com.live2d.type.GEditableMeshGuid
com.live2d.type.GTextureGuid
com.live2d.type.StaticFilterDefGuid
)SCHEMA");
    for (const auto& imported : schemaImports.split('\n', Qt::SkipEmptyParts)) writer.writeProcessingInstruction("import", imported);
    for (const auto& imported : physicsXmlImports()) if (!schemaImports.contains(imported)) writer.writeProcessingInstruction("import", imported);
    writer.writeStartElement("root"); writer.writeAttribute("fileFormatVersion", "503000005"); writer.writeStartElement("shared");
    for (const auto& n : shared) serialize(writer, n);
    writer.writeEndElement(); writer.writeStartElement("main"); serialize(writer, main); writer.writeEndElement(); writer.writeEndElement(); writer.writeEndDocument();
    if (writer.hasError()) fail("Failed to serialize project XML.");
    assets << Asset{"main.xml", "main_xml", zipDeflated(xml), 0x21};
    return container(assets);
}
} // namespace

void writeProject(const Model& model, const QString& outputPath) {
    if (QFileInfo::exists(outputPath)) fail("The output file already exists. Choose a new filename.");
    const auto bytes = Project(model).build();
    const QString absolutePath = QFileInfo(outputPath).absoluteFilePath();
    QTemporaryFile output(QFileInfo(absolutePath).absoluteDir().filePath(".recovery-XXXXXX.tmp"));
    if (!output.open()) fail("Cannot create the output project: " + output.errorString());
    if (output.write(bytes) != bytes.size() || !output.flush()) fail("Cannot finish writing the project: " + output.errorString());
    output.close();
    // QFile's rename refuses an existing destination, even if another process
    // created it while the project was being assembled.
    if (!output.rename(absolutePath)) fail("Cannot publish the new project without replacing a file: " + output.errorString());
    output.setAutoRemove(false);
}
} // namespace recovery
